'use strict';

const path = require('path');
const fs = require('fs');
const { fork, execSync } = require('child_process');

// Resolve bundled model path (works in dev and packaged builds)
function getModelsDir() {
  const devPath = path.join(__dirname, '../../resources/models');
  const packedPath = devPath.replace('app.asar', 'app.asar.unpacked');
  if (fs.existsSync(packedPath)) return packedPath;
  return devPath;
}

// Model registry - all models are bundled locally
const AI_MODELS = {
  'musicgen-small': {
    name: 'MusicGen Small',
    localDir: 'musicgen-small',
    type: 'text-to-audio',
    maxDuration: 30,
    sampleRate: 32000,
    description: 'Text-to-music generation (Meta, 300M params)'
  },
};

let _worker = null;
let _workerReady = false;

function ensureWorker() {
  if (_worker && !_worker.killed) return;
  const workerPath = path.join(__dirname, 'ai-worker.js');

  // Electron's binary causes ABI mismatches with onnxruntime-node. Use system Node.
  let nodeBin;
  try {
    const cmd = process.platform === 'win32' ? 'where node' : 'which node';
    nodeBin = execSync(cmd, { encoding: 'utf8' }).trim().split('\n')[0];
  } catch (_) {
    const candidates = process.platform === 'darwin'
      ? ['/opt/homebrew/bin/node', '/usr/local/bin/node']
      : ['/usr/local/bin/node', '/usr/bin/node'];
    nodeBin = candidates.find(p => fs.existsSync(p)) || 'node';
  }
  _worker = fork(workerPath, [], {
    stdio: ['pipe', 'pipe', 'pipe', 'ipc'],
    execPath: nodeBin
  });
  _workerReady = false;

  _worker.stdout.on('data', (d) => process.stdout.write('[AIWorker] ' + d));
  _worker.stderr.on('data', (d) => process.stderr.write('[AIWorker:err] ' + d));

  _worker.on('error', (err) => {
    console.error('[AIAudio] Worker error:', err.message);
  });

  _worker.on('exit', (code) => {
    console.log('[AIAudio] Worker exited with code', code);
    _worker = null;
    _workerReady = false;
  });
}

function waitForReady() {
  return new Promise((resolve, reject) => {
    if (_workerReady) return resolve();
    if (!_worker) return reject(new Error('Worker not started'));

    const timeout = setTimeout(() => {
      if (_worker) _worker.removeListener('message', onMsg);
      reject(new Error('Worker startup timed out'));
    }, 15000);

    const onMsg = (msg) => {
      if (msg.type === 'ready') {
        clearTimeout(timeout);
        _workerReady = true;
        if (_worker) _worker.removeListener('message', onMsg);
        resolve();
      }
    };
    const onExit = () => {
      clearTimeout(timeout);
      if (_worker) _worker.removeListener('message', onMsg);
      reject(new Error('Worker exited before ready'));
    };

    _worker.on('message', onMsg);
    _worker.once('exit', onExit);
  });
}

function getModels() {
  const modelsDir = getModelsDir();
  const models = [];
  for (const [id, info] of Object.entries(AI_MODELS)) {
    const modelPath = path.join(modelsDir, info.localDir);
    const available = fs.existsSync(path.join(modelPath, 'onnx', 'decoder_model_merged.onnx'));
    models.push({
      id,
      name: info.name,
      type: info.type,
      maxDuration: info.maxDuration,
      sampleRate: info.sampleRate,
      description: info.description,
      available
    });
  }
  return { models };
}

async function generateAudio(modelId, prompt, config, onProgress) {
  const model = AI_MODELS[modelId];
  if (!model) return { error: `Unknown model: ${modelId}` };
  if (!prompt || prompt.trim().length === 0) return { error: 'Prompt is required' };

  const modelsDir = getModelsDir();
  const modelPath = path.join(modelsDir, model.localDir);
  if (!fs.existsSync(modelPath)) {
    return { error: 'Model files not found. Reinstall the app.' };
  }

  try {
    ensureWorker();
    await waitForReady();
  } catch (err) {
    return { error: 'Failed to start AI worker: ' + err.message };
  }

  return new Promise((resolve) => {
    let resolved = false;

    const cleanup = () => {
      if (_worker) {
        _worker.removeListener('message', onMessage);
        _worker.removeListener('exit', onExit);
      }
    };

    const onMessage = (msg) => {
      if (msg.type === 'progress') {
        if (onProgress) onProgress(msg);
      } else if (msg.type === 'result') {
        if (resolved) return;
        resolved = true;
        cleanup();
        if (msg.error) {
          resolve({ error: msg.error });
        } else {
          resolve({
            waveform: msg.waveform,
            sampleRate: msg.sampleRate,
            numChannels: msg.numChannels,
            numSamples: msg.numSamples,
            duration: msg.duration
          });
        }
      }
    };

    const onExit = (code) => {
      if (resolved) return;
      resolved = true;
      cleanup();
      resolve({ error: 'AI worker process exited unexpectedly (code ' + code + '). Try again.' });
    };

    _worker.on('message', onMessage);
    _worker.once('exit', onExit);

    _worker.send({
      type: 'generate',
      modelId,
      modelPath,
      prompt: prompt.trim(),
      config,
      sampleRate: model.sampleRate
    });
  });
}

function cancelGeneration() {
  if (_worker && !_worker.killed) {
    _worker.kill();
    _worker = null;
    _workerReady = false;
    return { ok: true };
  }
  return { ok: false, message: 'No generation in progress' };
}

module.exports = {
  getModels,
  generateAudio,
  cancelGeneration
};
