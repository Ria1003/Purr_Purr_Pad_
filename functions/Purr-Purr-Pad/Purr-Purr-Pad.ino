/*
  ESP32 + 3×FSR → BPM + HTTPS → Firebase Function (adaptive detector)

  - Pins (ADC1): 34, 35, 32 (change PIN_FSR if needed)
  - Serial commands: 'D' dump /events.csv, 'C' clear /events.csv
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "FS.h"
#include "SPIFFS.h"

// ====== CONFIG (EDIT THESE) ======
const char* WIFI_SSID       = "AppleTV";
const char* WIFI_PASSWORD   = "888888888";
// NEW function URL you deployed (v2 name recommended)
// in your .ino
const char* FUNCTION_URL = "https://addreadinghttp-6pvuc2y5ra-uc.a.run.app";
const char* UID = "QeMlbBwBAPYm5ja0IZ7QQwphZ9i2";

const char* INGEST_KEY      = "vectoria77";                    // Must match your function

// ====== HARDWARE ======
const int NSENS = 3;                    // number of FSR channels
const int PIN_FSR[NSENS] = {34, 35, 32}; // ADC1 pins (WiFi-safe)

// ====== FILTERS / DETECTION ======
// Moving average for each channel
int   SMOOTH_N   = 4;        // 3–8 is reasonable
// Baseline EMA per channel
float ALPHA_BASE = 0.15f;    // faster re-zero to track load changes
// Adaptive envelope stats (for fused signal)
float ALPHA_ENV  = 0.10f;    // EMA for mean of devF
float ALPHA_MAD  = 0.10f;    // EMA for mean absolute deviation

// Peak detection using adaptive z-score thresholds
float Z_ARM   = 3.0f;        // arm when z > 3 (spike relative to noise)
float Z_RELEASE = 1.0f;      // trigger peak when z falls back below 1
unsigned long REFRACTORY_MS_MIN = 250;   // ~240 BPM upper bound
unsigned long REFRACTORY_MS_MAX = 6000;  // ~10 BPM lower bound

// Tachy/Brady thresholds for status
float TACHY_BPM = 50.0f;
float BRADY_BPM = 12.0f;

// Tachy event logging thresholds
float TACHY_RELEASE = 45.0f;
unsigned long MIN_DURATION_MS = 30000;
const int BPM_AVG_WINDOW = 5;

// Auto-calibration
const unsigned long CALIB_MS = 2000;     // time to capture initial baseline (keep pad idle)
const float MAD_FLOOR = 5.0f;            // avoid divide-by-small for z-score

// ====== STATE ======
const int BUF_MAX = 24;
int   buf[NSENS][BUF_MAX];
long  sumv[NSENS];
int   idxMv[NSENS];
float baseline[NSENS];
bool  inited = false;

unsigned long startMs = 0;
unsigned long lastPeak = 0;
bool armed = false;          // for z-arm/release logic

// Adaptive envelope stats
float envMean = 0.0f;        // EMA of devF
float envMAD  = 1.0f;        // EMA of |devF - envMean|
unsigned long lastEnvUpdateMs = 0;

// Tachy event state machine
enum EvState { EV_NORMAL, EV_ARMED, EV_ACTIVE };
EvState evState = EV_NORMAL;
unsigned long armStartMs = 0, evStartMs = 0;
float evMaxBPM = 0.0f, evSumBPM = 0.0f; int evCount = 0;

// Recent BPM smoothing
float bpmRing[BPM_AVG_WINDOW]; int bpmN=0, bpmPtr=0;
void pushBpm(float bpm){
  bpmRing[bpmPtr] = bpm;
  bpmPtr = (bpmPtr+1)%BPM_AVG_WINDOW;
  if (bpmN < BPM_AVG_WINDOW) bpmN++;
}
float getBpmAvgSmooth(){
  if (bpmN == 0) return 0.0f;
  float s=0; for(int i=0;i<bpmN;i++) s+=bpmRing[i];
  return s / bpmN;
}
float getLastRawBpm(){
  if (bpmN == 0) return 0.0f;
  int lastIdx = (bpmPtr + BPM_AVG_WINDOW - 1) % BPM_AVG_WINDOW;
  return bpmRing[lastIdx];
}

// ====== SPIFFS LOGGING ======
void ensureHeader() {
  if (!SPIFFS.exists("/events.csv")) {
    File f = SPIFFS.open("/events.csv", FILE_WRITE);
    if (f) { f.println("start_ms,end_ms,duration_s,max_bpm,avg_bpm"); f.close(); }
  }
}
void dumpLog(){
  ensureHeader();
  File f = SPIFFS.open("/events.csv", FILE_READ);
  if (!f) { Serial.println("open /events.csv failed"); return; }
  while (f.available()) Serial.write(f.read());
  f.close();
}
void clearLog(){
  SPIFFS.remove("/events.csv");
  ensureHeader();
  Serial.println("events.csv cleared.");
}

// ====== Smoothing ======
inline float movingAvg(int k, int x){
  sumv[k] -= buf[k][idxMv[k]];
  buf[k][idxMv[k]] = x;
  sumv[k] += x;
  idxMv[k] = (idxMv[k] + 1) % SMOOTH_N;
  return (float)sumv[k] / SMOOTH_N;
}

// ====== WiFi + HTTPS POST ======
bool wifiConnected = false;
unsigned long lastWiFiAttempt = 0;

void connectWiFiIfNeeded() {
  if (wifiConnected && WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWiFiAttempt < 5000) return;
  lastWiFiAttempt = now;

  Serial.printf("WiFi connecting to %s ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected) {
    Serial.print("WiFi OK. IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected.");
  }
}

const char* computeStatus(float bpmSmooth) {
  if (bpmSmooth <= 0) return "unknown";
  if (bpmSmooth >= TACHY_BPM) return "tachy";
  if (bpmSmooth <= BRADY_BPM) return "brady";
  return "normal";
}

bool postReading(float fusedValue, float bpmSmooth) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, FUNCTION_URL)) {
    Serial.println("[HTTPS] begin() failed"); return false;
  }

  https.addHeader("Content-Type", "application/json");
  if (INGEST_KEY && strlen(INGEST_KEY) > 0) https.addHeader("X-INGEST-KEY", INGEST_KEY);

  const char* status = computeStatus(bpmSmooth);
  bool alert = (strcmp(status, "tachy") == 0) || (strcmp(status, "brady") == 0);

  StaticJsonDocument<384> doc;
  doc["uid"]     = UID;
  doc["value"]   = fusedValue;         // debug envelope
  doc["bpm"]     = getLastRawBpm();    // last beat
  doc["avgBpm"]  = bpmSmooth;          // smoothed
  doc["status"]  = status;
  doc["alert"]   = alert;
  doc["source"]  = "esp32";

  String body; serializeJson(doc, body);
  Serial.print("[POST body] "); Serial.println(body);
  int code = https.POST(body);
  if (code > 0) {
    String resp = https.getString();
    Serial.print("[HTTPS] POST "); Serial.print(code); Serial.print(" ");
    Serial.println(resp);
  } else {
    Serial.print("[HTTPS] POST failed, error: "); Serial.println(code);
  }
  https.end();
  return code >= 200 && code < 300;
}

// Rate-limit posts; only send when we have BPM
unsigned long lastPostMs = 0;
const unsigned long POST_INTERVAL_MS = 1500;

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(200);

  if (!SPIFFS.begin(true)) Serial.println("SPIFFS mount failed");
  else ensureHeader();

  analogReadResolution(12);
  for (int i=0;i<NSENS;i++) analogSetPinAttenuation(PIN_FSR[i], ADC_11db);

  for (int k=0;k<NSENS;k++){
    sumv[k]=0; idxMv[k]=0; baseline[k]=0;
    for (int i=0;i<BUF_MAX;i++) buf[k][i]=0;
  }

  Serial.println("Ready (3x FSR). Keep pad idle for 2s to calibrate...");
  startMs = millis();
  inited = false;
  envMean = 0.0f; envMAD = MAD_FLOOR;

  connectWiFiIfNeeded();
}

// ====== LOOP ======
void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c=='D'||c=='d') dumpLog();
    if (c=='C'||c=='c') clearLog();
  }

  connectWiFiIfNeeded();

  // Read & per-channel smoothing/baseline
  int   raw[NSENS];
  float sig[NSENS];
  float dev[NSENS];

  for (int k=0;k<NSENS;k++){
    raw[k] = analogRead(PIN_FSR[k]);
    sig[k] = movingAvg(k, raw[k]);
    if (!inited) baseline[k] = sig[k];
  }

  unsigned long now = millis();
  if (!inited && (now - startMs) >= CALIB_MS) {
    inited = true; lastPeak = 0; armed = false;
    Serial.println("Calibration done.");
  }

  for (int k=0;k<NSENS;k++){
    // Faster baseline tracking (so DC load doesn’t look like a peak forever)
    baseline[k] = (1.0f - ALPHA_BASE) * baseline[k] + ALPHA_BASE * sig[k];
    float d = sig[k] - baseline[k];
    dev[k] = (d > 0) ? d : 0;   // upward envelope only
  }

  // Fuse: strongest channel envelope
  float devF = dev[0];
  if (dev[1] > devF) devF = dev[1];
  if (dev[2] > devF) devF = dev[2];

  // Adaptive envelope stats for z-score
  // EMA updates even before calibration finishes (harmless)
  float prevMean = envMean;
  envMean = (1.0f - ALPHA_ENV) * envMean + ALPHA_ENV * devF;
  float absDev = fabsf(devF - prevMean);
  envMAD = (1.0f - ALPHA_MAD) * envMAD + ALPHA_MAD * absDev;
  if (envMAD < MAD_FLOOR) envMAD = MAD_FLOOR;

  float z = (devF - envMean) / envMAD; // normalized spike measure

  // Adaptive arm/trigger logic + refractory between peaks
  bool justPeaked = false;
  if (!armed && z > Z_ARM) {
    armed = true;
  }
  if (armed && z < Z_RELEASE) {
    // Count a peak at this moment if within physiologic intervals
    unsigned long dt = (lastPeak == 0) ? 0 : (now - lastPeak);
    if (lastPeak != 0 && dt >= REFRACTORY_MS_MIN && dt <= REFRACTORY_MS_MAX) {
      float bpm_raw = 60000.0f / (float)dt;
      if (bpm_raw > 0.0f && bpm_raw <= 240.0f) {
        pushBpm(bpm_raw);
        float bpmSmooth = getBpmAvgSmooth();

        Serial.print("BPM: ");
        Serial.print(bpm_raw, 1);
        Serial.print("  (avg ");
        Serial.print(bpmSmooth, 1);
        Serial.println(")");
        justPeaked = true;

        // Tachy event tracker (optional logging)
        switch (evState) {
          case EV_NORMAL:
            if (bpmSmooth > TACHY_BPM) { evState = EV_ARMED; armStartMs = now; }
            break;
          case EV_ARMED:
            if (bpmSmooth > TACHY_BPM) {
              if (now - armStartMs >= MIN_DURATION_MS) {
                evState   = EV_ACTIVE;
                evStartMs = armStartMs;
                evMaxBPM  = bpmSmooth;
                evSumBPM  = bpmSmooth;
                evCount   = 1;
                Serial.println("[ALERT] Tachypnea ACTIVE");
              }
            } else {
              evState = EV_NORMAL;
            }
            break;
          case EV_ACTIVE:
            if (bpmSmooth > evMaxBPM) evMaxBPM = bpmSmooth;
            evSumBPM += bpmSmooth; evCount++;
            if (bpmSmooth < TACHY_RELEASE) {
              unsigned long evEndMs = now;
              float duration_s = (evEndMs - evStartMs)/1000.0f;
              float avgBPM = (evCount>0)? (evSumBPM/evCount) : evMaxBPM;
              File f = SPIFFS.open("/events.csv", FILE_APPEND);
              if (f) {
                f.printf("%lu,%lu,%.1f,%.1f,%.1f\n",
                         evStartMs, evEndMs, duration_s, evMaxBPM, avgBPM);
                f.close();
              }
              Serial.println("[ALERT] Tachypnea ENDED & LOGGED");
              evState=EV_NORMAL; armStartMs=0; evStartMs=0;
              evMaxBPM=0; evSumBPM=0; evCount=0; bpmN=0;
            }
            break;
        }
      }
    }
    lastPeak = now;
    armed = false;
  }

  // Auto re-zero: if devF stays high (held load) for long & no peaks, nudge baselines faster
  static unsigned long noPeakSince = 0;
  if (justPeaked) { noPeakSince = now; }
  else if (now - noPeakSince > 8000 && bpmN == 0) {
    // Speed up baseline to chase DC offset
    for (int k=0;k<NSENS;k++){
      baseline[k] = 0.8f * baseline[k] + 0.2f * sig[k];
    }
  }

  // -------- Periodic cloud POST (value + bpm) --------
  float bpmSmooth = getBpmAvgSmooth();
  bool haveBpm = bpmSmooth > 0.0f;
  if ((now - lastPostMs >= POST_INTERVAL_MS && haveBpm) || justPeaked) {
    (void)postReading(devF, bpmSmooth);
    lastPostMs = now;
  }

  // Debug prints for tuning
  Serial.print("raw[0..2]=");
  Serial.print(raw[0]); Serial.print(",");
  Serial.print(raw[1]); Serial.print(",");
  Serial.print(raw[2]); Serial.print("  ");

  Serial.print("devF="); Serial.print((int)devF);
  Serial.print(" z="); Serial.print(z, 2);
  Serial.print(" bpmN="); Serial.print(bpmN);
  Serial.print(" wifi="); Serial.println((WiFi.status()==WL_CONNECTED)?1:0);

  delay(20);  // ~50 Hz loop
}
