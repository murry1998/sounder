'use strict';

// This runs in a SEPARATE child process to avoid ONNX Runtime symbol
// conflicts with the native sounder_engine.node addon.

const path = require('path');

let _transformers = null;
let _tokenizer = null;
let _model = null;
let _loadedModelId = null;

async function getTransformers() {
  if (!_transformers) {
    _transformers = await import('@huggingface/transformers');
    _transformers.env.allowRemoteModels = false;
    _transformers.env.allowLocalModels = true;
    _transformers.env.backends.onnx.wasm.proxy = false;
  }
  return _transformers;
}

async function handleGenerate(msg) {
  const { modelId, modelPath, prompt, config, sampleRate } = msg;

  try {
    const T = await getTransformers();

    if (!_model || _loadedModelId !== modelId) {
      process.send({ type: 'progress', status: 'loading', message: 'Loading AI model...' });

      _tokenizer = await T.AutoTokenizer.from_pretrained(modelPath, {
        local_files_only: true
      });

      _model = await T.MusicgenForConditionalGeneration.from_pretrained(modelPath, {
        local_files_only: true,
        dtype: 'fp32'
      });

      _loadedModelId = modelId;
    }

    process.send({ type: 'progress', status: 'generating', message: 'Generating audio... this may take a minute' });

    const durationSec = Math.min(config.duration || 8, 30);
    const maxNewTokens = Math.round(durationSec * 50);

    const genOptions = { max_new_tokens: maxNewTokens };
    if (config.temperature && config.temperature !== 1.0) {
      genOptions.temperature = config.temperature;
      genOptions.do_sample = true;
    }

    const inputs = _tokenizer(prompt.trim());
    const output = await _model.generate({ ...inputs, ...genOptions });

    let audioData;
    if (output && output.ort_tensor && output.ort_tensor.cpuData) {
      audioData = output.ort_tensor.cpuData;
    } else if (output && output.cpuData) {
      audioData = output.cpuData;
    } else if (output && output.data) {
      audioData = output.data;
    } else {
      process.send({ type: 'result', error: 'Unexpected output format from model' });
      return;
    }

    if (!audioData || audioData.length === 0) {
      process.send({ type: 'result', error: 'Generation produced no audio' });
      return;
    }

    // Resample from model rate (32000) to engine rate (48000) so the
    // native AudioTrack plays back at the correct speed and pitch.
    const targetRate = msg.targetSampleRate || 48000;
    let finalData = audioData;
    let finalRate = sampleRate;
    if (sampleRate !== targetRate) {
      const ratio = targetRate / sampleRate;
      const outLen = Math.round(audioData.length * ratio);
      const resampled = new Float32Array(outLen);
      for (let i = 0; i < outLen; i++) {
        const srcPos = i / ratio;
        const idx = Math.floor(srcPos);
        const frac = srcPos - idx;
        const s0 = audioData[idx] || 0;
        const s1 = audioData[Math.min(idx + 1, audioData.length - 1)] || 0;
        resampled[i] = s0 + frac * (s1 - s0);
      }
      finalData = resampled;
      finalRate = targetRate;
    }

    process.send({
      type: 'result',
      waveform: Array.from(finalData),
      sampleRate: finalRate,
      numChannels: 1,
      numSamples: finalData.length,
      duration: finalData.length / finalRate
    });
  } catch (err) {
    process.send({ type: 'result', error: err.message || String(err) });
  }
}

process.on('message', (msg) => {
  if (msg.type === 'generate') {
    handleGenerate(msg).catch(err => {
      process.send({ type: 'result', error: err.message || String(err) });
    });
  }
});

process.send({ type: 'ready' });
