'use strict';

const path = require('path');
const fs = require('fs');
const https = require('https');
const http = require('http');
const { fork, execSync } = require('child_process');
const { app } = require('electron');

const HF_BASE = 'https://huggingface.co';

// Resolve model path — prefers user data dir (writable) in packaged app
function getModelsDir() {
  if (app && app.isPackaged) {
    const userModels = path.join(app.getPath('userData'), 'models');
    fs.mkdirSync(userModels, { recursive: true });
    // Copy bundled JSON configs from app resources into writable models dir
    seedBundledConfigs(userModels);
    return userModels;
  }
  const devPath = path.join(__dirname, '../../resources/models');
  const packedPath = devPath.replace('app.asar', 'app.asar.unpacked');
  if (fs.existsSync(packedPath)) return packedPath;
  return devPath;
}

// Copy bundled JSON config files from the asar resources to the writable models dir
function seedBundledConfigs(userModelsDir) {
  const bundledDir = path.join(__dirname, '../../resources/models');
  const mgSrc = path.join(bundledDir, 'musicgen-small');
  const mgDest = path.join(userModelsDir, 'musicgen-small');

  if (!fs.existsSync(mgSrc)) return;
  fs.mkdirSync(mgDest, { recursive: true });

  const jsonFiles = ['config.json', 'generation_config.json', 'tokenizer.json',
                     'tokenizer_config.json', 'preprocessor_config.json'];
  for (const file of jsonFiles) {
    const src = path.join(mgSrc, file);
    const dest = path.join(mgDest, file);
    if (fs.existsSync(src) && !fs.existsSync(dest)) {
      try { fs.copyFileSync(src, dest); } catch (_) {}
    }
  }
}

// Model registry
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

// Files required for each model download
const MODEL_FILES = {
  'musicgen-small': [
    // ONNX model files only — JSON configs are bundled in the app and seeded automatically
    { url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/onnx/decoder_model_merged.onnx`, rel: 'onnx/decoder_model_merged.onnx', label: 'MusicGen decoder (~1.6 GB)' },
    { url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/onnx/text_encoder.onnx`, rel: 'onnx/text_encoder.onnx', label: 'MusicGen text encoder (~418 MB)' },
    { url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/onnx/encodec_decode.onnx`, rel: 'onnx/encodec_decode.onnx', label: 'MusicGen EnCodec decoder (~113 MB)' },
  ],
  'htdemucs': [
    { url: `${HF_BASE}/Xenova/htdemucs/resolve/main/onnx/model.onnx`, rel: 'htdemucs.onnx', label: 'HTDemucs (stem separation, ~166 MB)', root: true },
  ]
};

function downloadFile(url, dest) {
  return new Promise((resolve, reject) => {
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    const get = url.startsWith('https') ? https.get : http.get;
    get(url, (res) => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        return downloadFile(res.headers.location, dest).then(resolve).catch(reject);
      }
      if (res.statusCode !== 200) {
        return reject(new Error(`HTTP ${res.statusCode} for ${url}`));
      }
      const total = parseInt(res.headers['content-length'] || '0', 10);
      let received = 0;
      const file = fs.createWriteStream(dest);
      res.on('data', (chunk) => {
        received += chunk.length;
        if (_downloadProgress) _downloadProgress({ received, total });
      });
      res.pipe(file);
      file.on('finish', () => { file.close(); resolve(); });
      file.on('error', (err) => { fs.unlink(dest, () => {}); reject(err); });
    }).on('error', reject);
  });
}

let _downloadProgress = null;

async function downloadModels(modelId, onProgress) {
  const files = MODEL_FILES[modelId];
  if (!files) return { error: `Unknown model: ${modelId}` };

  const modelsDir = getModelsDir();
  const modelDir = AI_MODELS[modelId] ? path.join(modelsDir, AI_MODELS[modelId].localDir) : modelsDir;
  let downloaded = 0;

  for (let i = 0; i < files.length; i++) {
    const f = files[i];
    const dest = f.root ? path.join(modelsDir, f.rel) : path.join(modelDir, f.rel);

    if (fs.existsSync(dest)) {
      downloaded++;
      if (onProgress) onProgress({ file: f.label, fileIndex: i, totalFiles: files.length, status: 'skip' });
      continue;
    }

    if (onProgress) onProgress({ file: f.label, fileIndex: i, totalFiles: files.length, status: 'downloading', received: 0, total: 0 });

    _downloadProgress = (data) => {
      if (onProgress) onProgress({ file: f.label, fileIndex: i, totalFiles: files.length, status: 'downloading', received: data.received, total: data.total });
    };

    try {
      await downloadFile(f.url, dest);
      downloaded++;
      if (onProgress) onProgress({ file: f.label, fileIndex: i, totalFiles: files.length, status: 'done' });
    } catch (err) {
      _downloadProgress = null;
      return { error: `Failed to download ${f.label}: ${err.message}` };
    }
  }

  _downloadProgress = null;
  return { ok: true, downloaded };
}

function getMissingModels() {
  const modelsDir = getModelsDir();
  const missing = [];
  for (const [id, info] of Object.entries(AI_MODELS)) {
    const modelPath = path.join(modelsDir, info.localDir);
    if (!fs.existsSync(path.join(modelPath, 'onnx', 'decoder_model_merged.onnx'))) {
      missing.push(id);
    }
  }
  // Check HTDemucs
  if (!fs.existsSync(path.join(modelsDir, 'htdemucs.onnx'))) {
    missing.push('htdemucs');
  }
  return missing;
}

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
    return { error: 'Model files not found. Use the Download Models button in the AI panel.' };
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
  getModelsDir,
  getMissingModels,
  downloadModels,
  generateAudio,
  cancelGeneration
};
