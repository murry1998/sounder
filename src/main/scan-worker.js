// scan-worker.js — Runs in a child_process.fork() from the main process.
// Loads the native addon in a clean process, scans plugins, saves to disk,
// then exits. If a plugin crashes during scanning, only this worker dies;
// the main app survives and reads whatever was saved incrementally.

const path = require('path');
const fs = require('fs');

let native = null;
try {
  const devPath = path.join(__dirname, '../../native/build/Release/sounder_engine.node');
  const packedPath = devPath.replace('app.asar', 'app.asar.unpacked');
  if (fs.existsSync(packedPath)) {
    native = require(packedPath);
  } else {
    native = require(devPath);
  }
} catch (e) {
  if (process.send) process.send({ type: 'error', message: e.message });
  process.exit(1);
}

process.on('message', (msg) => {
  if (msg.type === 'scan') {
    try {
      const result = native.scanPluginsWorker(msg.directory || '');
      if (process.send) process.send({ type: 'result', count: result.count });
    } catch (e) {
      if (process.send) process.send({ type: 'error', message: e.message });
    }
    process.exit(0);
  }
});

if (process.send) process.send({ type: 'ready' });
