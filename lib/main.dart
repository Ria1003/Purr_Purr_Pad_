// lib/main.dart
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:cloud_firestore/cloud_firestore.dart';

import 'firebase_options.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp(
    options: DefaultFirebaseOptions.currentPlatform,
  );

  final auth = FirebaseAuth.instance;
  if (auth.currentUser == null) {
    await auth.signInAnonymously();
  }

  runApp(const PurrPurrPadApp());
}

class PurrPurrPadApp extends StatelessWidget {
  const PurrPurrPadApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Purr Purr Pad',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.deepPurple),
        useMaterial3: true,
      ),
      home: const ReadingsPage(),
    );
  }
}

class ReadingsPage extends StatefulWidget {
  const ReadingsPage({super.key});
  @override
  State<ReadingsPage> createState() => _ReadingsPageState();
}

class _ReadingsPageState extends State<ReadingsPage> {
  late final String myUid;
  late String targetUid;
  final _uidCtrl = TextEditingController();

  @override
  void initState() {
    super.initState();
    myUid = FirebaseAuth.instance.currentUser!.uid;
    targetUid = myUid; // default to current app user
    _uidCtrl.text = targetUid;
  }

  // ---------- helpers: robust parsing ----------
  double? _asDouble(dynamic v) {
    if (v == null) return null;
    if (v is num) return v.toDouble();
    if (v is String) return double.tryParse(v);
    return null;
  }

  double? _readBpm(Map<String, dynamic> d) {
    return _asDouble(d['bpm'] ?? d['BPM'] ?? d['beat'] ?? d['rate']);
  }

  double? _readAvgBpm(Map<String, dynamic> d) {
    return _asDouble(
      d['avgBpm'] ?? d['avg_bpm'] ?? d['averageBpm'] ?? d['avg'] ?? d['meanBpm'],
    );
  }

  String _readStatus(Map<String, dynamic> d) {
    final raw = (d['status'] ?? d['state'] ?? d['label'])?.toString().toLowerCase();
    return (raw == null || raw.isEmpty) ? 'unknown' : raw;
  }

  bool _readAlert(Map<String, dynamic> d) {
    final v = d['alert'] ?? d['alarm'] ?? d['warn'];
    if (v is bool) return v;
    if (v is num) return v != 0;
    if (v is String) return v.toLowerCase() == 'true' || v == '1';
    return false;
  }

  Color _statusColor(String status, BuildContext context) {
    switch (status.toLowerCase()) {
      case 'tachy':
        return Colors.red;
      case 'brady':
        return Colors.orange;
      case 'normal':
        return Colors.green;
      default:
        return Theme.of(context).colorScheme.secondary;
    }
  }

  String _statusLabel(String status) {
    switch (status.toLowerCase()) {
      case 'tachy':
        return 'TACHY';
      case 'brady':
        return 'BRADY';
      case 'normal':
        return 'NORMAL';
      default:
        return 'UNKNOWN';
    }
  }

  @override
  Widget build(BuildContext context) {
    // Build the Query for the *selected* UID
    final query = FirebaseFirestore.instance
        .collection('users')
        .doc(targetUid)
        .collection('readings')
        .orderBy('createdAt', descending: true)
        .limit(50);

    final Stream<QuerySnapshot<Map<String, dynamic>>> readingsStream =
        query.snapshots();

    final uidMatches = targetUid == myUid;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Purr Purr Pad Readings'),
        actions: [
          IconButton(
            tooltip: 'Sign out',
            icon: const Icon(Icons.logout),
            onPressed: () async {
              await FirebaseAuth.instance.signOut();
              await FirebaseAuth.instance.signInAnonymously();
              if (!mounted) return;
              setState(() {
                final newUid = FirebaseAuth.instance.currentUser!.uid;
                myUid = newUid;
                // keep targetUid unchanged — you control it manually
              });
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('Re-signed in anonymously')),
              );
            },
          ),
        ],
      ),
      body: Column(
        children: [
          // UID selector banner
          Container(
            width: double.infinity,
            color: Theme.of(context).colorScheme.secondaryContainer,
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('App UID (read-only):',
                    style: Theme.of(context).textTheme.labelMedium),
                SelectableText(
                  myUid,
                  style: Theme.of(context).textTheme.bodyLarge,
                ),
                const SizedBox(height: 8),
                Text('Target UID to read (paste ESP32 UID here):',
                    style: Theme.of(context).textTheme.labelMedium),
                Row(
                  children: [
                    Expanded(
                      child: TextField(
                        controller: _uidCtrl,
                        decoration: const InputDecoration(
                          hintText: 'users/{UID}/readings',
                          isDense: true,
                          border: OutlineInputBorder(),
                        ),
                        style: Theme.of(context).textTheme.bodyMedium,
                      ),
                    ),
                    const SizedBox(width: 8),
                    TextButton(
                      onPressed: () async {
                        final data = await Clipboard.getData(Clipboard.kTextPlain);
                        if (data?.text != null && data!.text!.trim().isNotEmpty) {
                          _uidCtrl.text = data.text!.trim();
                        }
                      },
                      child: const Text('Paste'),
                    ),
                    const SizedBox(width: 4),
                    FilledButton(
                      onPressed: () {
                        final newUid = _uidCtrl.text.trim();
                        if (newUid.isNotEmpty) {
                          setState(() => targetUid = newUid);
                        }
                      },
                      child: const Text('Apply'),
                    ),
                  ],
                ),
                const SizedBox(height: 6),
                Row(
                  children: [
                    FilterChip(
                      label: const Text('Use my UID'),
                      selected: uidMatches,
                      onSelected: (_) {
                        setState(() {
                          targetUid = myUid;
                          _uidCtrl.text = myUid;
                        });
                      },
                    ),
                    const SizedBox(width: 8),
                    Text(
                      'Reading from: $targetUid',
                      style: Theme.of(context).textTheme.bodySmall,
                    ),
                  ],
                ),
              ],
            ),
          ),

          // Streamed readings for targetUid
          Expanded(
            child: StreamBuilder<QuerySnapshot<Map<String, dynamic>>>(
              stream: readingsStream,
              builder: (context, snapshot) {
                if (snapshot.hasError) {
                  return Padding(
                    padding: const EdgeInsets.all(16),
                    child: Text(
                      'Error: ${snapshot.error}',
                      style: Theme.of(context)
                          .textTheme
                          .bodyLarge
                          ?.copyWith(color: Colors.red),
                    ),
                  );
                }
                if (snapshot.connectionState == ConnectionState.waiting) {
                  return const Center(child: CircularProgressIndicator());
                }

                final docs = snapshot.data?.docs ?? [];
                if (docs.isEmpty) {
                  return const Center(child: Text('No readings yet.'));
                }

                return ListView.separated(
                  padding: const EdgeInsets.symmetric(vertical: 8),
                  itemCount: docs.length,
                  separatorBuilder: (_, __) => const Divider(height: 1),
                  itemBuilder: (context, index) {
                    final data = docs[index].data();

                    final bpm   = _readBpm(data);
                    final avg   = _readAvgBpm(data);
                    final stat  = _readStatus(data);
                    final alert = _readAlert(data);

                    final value = _asDouble(data['value']); // debug
                    final createdAt = data['createdAt'];

                    DateTime? ts;
                    if (createdAt is Timestamp) {
                      ts = createdAt.toDate();
                    } else if (createdAt is String) {
                      ts = DateTime.tryParse(createdAt);
                    }

                    final keys = data.keys.join(', '); // debug helper

                    return ListTile(
                      leading: Icon(
                        Icons.monitor_heart,
                        color: alert ? Colors.red : Theme.of(context).colorScheme.primary,
                      ),
                      title: Row(
                        children: [
                          Text(
                            'BPM: ${bpm?.toStringAsFixed(1) ?? "--"}',
                            style: Theme.of(context).textTheme.titleMedium,
                          ),
                          const SizedBox(width: 12),
                          Text(
                            'Avg: ${avg?.toStringAsFixed(1) ?? "--"}',
                            style: Theme.of(context).textTheme.titleSmall,
                          ),
                          const SizedBox(width: 12),
                          Chip(
                            label: Text(_statusLabel(stat)),
                            labelStyle: Theme.of(context)
                                .textTheme
                                .labelSmall
                                ?.copyWith(color: Colors.white),
                            backgroundColor: _statusColor(stat, context),
                            visualDensity: VisualDensity.compact,
                            materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                          ),
                        ],
                      ),
                      subtitle: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(ts != null ? ts.toLocal().toString() : 'no timestamp'),
                          if (value != null)
                            Text('value (debug): $value',
                                style: Theme.of(context)
                                    .textTheme
                                    .bodySmall
                                    ?.copyWith(color: Colors.grey)),
                          Text('keys: $keys',
                              style: Theme.of(context)
                                  .textTheme
                                  .bodySmall
                                  ?.copyWith(color: Colors.grey)),
                        ],
                      ),
                      dense: true,
                    );
                  },
                );
              },
            ),
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: () async {
          final now = FieldValue.serverTimestamp();
          await FirebaseFirestore.instance
              .collection('users')
              .doc(targetUid) // <-- write to currently selected UID
              .collection('readings')
              .add({
            'value': 18.0,
            'bpm': 52.3,
            'avgBpm': 49.8,
            'status': 'tachy',
            'alert': true,
            'createdAt': now,
            'source': 'app-test',
          });
        },
        label: const Text('Add test'),
        icon: const Icon(Icons.add),
      ),
    );
  }
}
