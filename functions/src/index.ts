import { onRequest } from "firebase-functions/v2/https";
import * as logger from "firebase-functions/logger";
import { initializeApp } from "firebase-admin/app";
import { getFirestore, FieldValue } from "firebase-admin/firestore";

initializeApp();
const db = getFirestore();

// keep in sync with ESP32 header
const REQUIRED_INGEST_KEY = process.env.INGEST_KEY || "vectoria77";

export const addReadingHttp = onRequest(async (req, res): Promise<void> => {
  try {
    // CORS
    res.set("Access-Control-Allow-Origin", "*");
    res.set("Access-Control-Allow-Headers", "Content-Type, X-INGEST-KEY");
    res.set("Access-Control-Allow-Methods", "POST, OPTIONS");

    if (req.method === "OPTIONS") { res.status(204).send(""); return; }
    if (req.method !== "POST")   { res.status(405).json({ ok:false, error:"Use POST" }); return; }

    // optional key
    const hdrKey = req.get("X-INGEST-KEY") || "";
    if (REQUIRED_INGEST_KEY && hdrKey !== REQUIRED_INGEST_KEY) {
      res.status(401).json({ ok:false, error:"Bad ingest key" }); return;
    }

    const body = (req.body ?? {}) as {
      uid?: string; value?: number; bpm?: number; avgBpm?: number;
      status?: string; alert?: boolean; source?: string;
    };

    const { uid, value, bpm, avgBpm, status, alert, source } = body;

    if (!uid || typeof uid !== "string") {
      res.status(400).json({ ok:false, error:"Missing uid" }); return;
    }

    // Build doc with only present fields. NOTE: we write under /users/{uid}/readings
    const data: Record<string, unknown> = {
      createdAt: FieldValue.serverTimestamp(),
      source: source ? String(source) : "ingest",
    };
    if (value !== undefined)  data.value  = Number(value);
    if (bpm !== undefined)    data.bpm    = Number(bpm);
    if (avgBpm !== undefined) data.avgBpm = Number(avgBpm);
    if (status !== undefined) data.status = String(status);
    if (alert !== undefined)  data.alert  = Boolean(alert);

    const ref = await db.collection("users").doc(uid).collection("readings").add(data);

    // helpful echo so you can verify with curl
    logger.info("addReadingHttp wrote", { uid, doc: ref.id, data });
    res.status(200).json({ ok:true, id: ref.id, wrote: data }); return;
  } catch (e) {
    logger.error(e);
    res.status(500).json({ ok:false, error:String(e) }); return;
  }
});
