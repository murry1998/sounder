#!/usr/bin/env node
'use strict';

const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');

const MODELS_DIR = path.join(__dirname, '..', 'resources', 'models');

const HF_BASE = 'https://huggingface.co';

const FILES = [
  // HTDemucs stem separation model
  {
    url: `${HF_BASE}/Xenova/htdemucs/resolve/main/onnx/model.onnx`,
    dest: path.join(MODELS_DIR, 'htdemucs.onnx'),
    label: 'HTDemucs (stem separation, ~166 MB)'
  },
  // MusicGen Small ONNX models
  {
    url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/onnx/decoder_model_merged.onnx`,
    dest: path.join(MODELS_DIR, 'musicgen-small', 'onnx', 'decoder_model_merged.onnx'),
    label: 'MusicGen decoder (~1.6 GB)'
  },
  {
    url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/onnx/text_encoder.onnx`,
    dest: path.join(MODELS_DIR, 'musicgen-small', 'onnx', 'text_encoder.onnx'),
    label: 'MusicGen text encoder (~418 MB)'
  },
  {
    url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/onnx/encodec_decode.onnx`,
    dest: path.join(MODELS_DIR, 'musicgen-small', 'onnx', 'encodec_decode.onnx'),
    label: 'MusicGen EnCodec decoder (~113 MB)'
  },
  // MusicGen config/tokenizer files
  {
    url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/config.json`,
    dest: path.join(MODELS_DIR, 'musicgen-small', 'config.json'),
    label: 'MusicGen config.json'
  },
  {
    url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/generation_config.json`,
    dest: path.join(MODELS_DIR, 'musicgen-small', 'generation_config.json'),
    label: 'MusicGen generation_config.json'
  },
  {
    url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/tokenizer.json`,
    dest: path.join(MODELS_DIR, 'musicgen-small', 'tokenizer.json'),
    label: 'MusicGen tokenizer.json'
  },
  {
    url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/tokenizer_config.json`,
    dest: path.join(MODELS_DIR, 'musicgen-small', 'tokenizer_config.json'),
    label: 'MusicGen tokenizer_config.json'
  },
  {
    url: `${HF_BASE}/Xenova/musicgen-small/resolve/main/preprocessor_config.json`,
    dest: path.join(MODELS_DIR, 'musicgen-small', 'preprocessor_config.json'),
    label: 'MusicGen preprocessor_config.json'
  }
];

function formatBytes(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
  if (bytes < 1073741824) return (bytes / 1048576).toFixed(1) + ' MB';
  return (bytes / 1073741824).toFixed(2) + ' GB';
}

function download(url, dest) {
  return new Promise((resolve, reject) => {
    const dir = path.dirname(dest);
    fs.mkdirSync(dir, { recursive: true });

    const get = url.startsWith('https') ? https.get : http.get;
    get(url, (res) => {
      // Follow redirects
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        return download(res.headers.location, dest).then(resolve).catch(reject);
      }
      if (res.statusCode !== 200) {
        return reject(new Error(`HTTP ${res.statusCode} for ${url}`));
      }

      const total = parseInt(res.headers['content-length'] || '0', 10);
      let received = 0;
      const file = fs.createWriteStream(dest);

      res.on('data', (chunk) => {
        received += chunk.length;
        if (total > 0) {
          const pct = ((received / total) * 100).toFixed(1);
          process.stdout.write(`\r  ${formatBytes(received)} / ${formatBytes(total)} (${pct}%)`);
        } else {
          process.stdout.write(`\r  ${formatBytes(received)}`);
        }
      });

      res.pipe(file);
      file.on('finish', () => {
        file.close();
        process.stdout.write('\n');
        resolve();
      });
      file.on('error', (err) => {
        fs.unlink(dest, () => {});
        reject(err);
      });
    }).on('error', reject);
  });
}

async function main() {
  console.log('Sounder — AI Model Setup');
  console.log('========================\n');
  console.log(`Downloading models to: ${MODELS_DIR}\n`);

  let skipped = 0;
  let downloaded = 0;

  for (const file of FILES) {
    if (fs.existsSync(file.dest)) {
      console.log(`[skip] ${file.label} (already exists)`);
      skipped++;
      continue;
    }
    console.log(`[download] ${file.label}`);
    try {
      await download(file.url, file.dest);
      downloaded++;
    } catch (err) {
      console.error(`  FAILED: ${err.message}`);
      console.error('  You may need to download this file manually.');
    }
  }

  console.log(`\nDone. ${downloaded} downloaded, ${skipped} skipped.`);
  if (downloaded + skipped === FILES.length) {
    console.log('All models are ready.');
  }
}

main().catch((err) => {
  console.error('Setup failed:', err.message);
  process.exit(1);
});
