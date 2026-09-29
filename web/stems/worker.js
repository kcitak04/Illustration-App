importScripts("https://cdn.jsdelivr.net/npm/onnxruntime-web@1.23.0/dist/ort.min.js");
(() => {
  var __defProp = Object.defineProperty;
  var __defNormalProp = (obj, key, value) => key in obj ? __defProp(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
  var __publicField = (obj, key, value) => __defNormalProp(obj, typeof key !== "symbol" ? key + "" : key, value);

  // src/ort-shim.js
  var o = self.ort;
  var env = o.env;
  var InferenceSession = o.InferenceSession;
  var Tensor = o.Tensor;

  // ../../../../../tmp/tmp.UCJ2xT1FrC/demucs/package/dist/onnx-htdemucs.js
  env.wasm.wasmPaths = "./";
  var ONNXHTDemucs = class _ONNXHTDemucs {
    constructor() {
      __publicField(this, "sources", ["drums", "bass", "other", "vocals"]);
      __publicField(this, "audioChannels", 2);
      __publicField(this, "samplerate", 44100);
      __publicField(this, "segment", 7.8);
      // @ts-expect-error these are initialized in the async initializer
      __publicField(this, "session");
      // @ts-expect-error
      __publicField(this, "inputNames");
      // @ts-expect-error
      __publicField(this, "outputNames");
    }
    static async init(modelWeights) {
      let instance = new _ONNXHTDemucs();
      let opts = typeof self === "undefined" || typeof self.location === "undefined" ? void 0 : { executionProviders: ["webgpu", "wasm"] };
      let session = await InferenceSession.create(modelWeights, opts);
      instance.session = session;
      instance.inputNames = session.inputNames;
      instance.outputNames = session.outputNames;
      return instance;
    }
    validLength(length) {
      const trainingLength = Math.floor(this.segment * this.samplerate);
      if (trainingLength < length) {
        throw new Error(`Given length ${length} is longer than training length ${trainingLength}`);
      }
      return trainingLength;
    }
    async forward(mix, magspec) {
      const mixTensor = new Tensor("float32", mix.data, mix.shape);
      const magspecTensor = new Tensor("float32", magspec.data, magspec.shape);
      const feeds = {};
      feeds[this.inputNames[0]] = mixTensor;
      feeds[this.inputNames[1]] = magspecTensor;
      const results = await this.session.run(feeds);
      const outX = results[this.outputNames[0]];
      const outXt = results[this.outputNames[1]];
      return {
        outX: { data: outX.data, shape: outX.dims },
        outXt: { data: outXt.data, shape: outXt.dims }
      };
    }
  };

  // ../../../../../tmp/tmp.UCJ2xT1FrC/demucs/package/dist/dsp.js
  function fft(realInput, imagInput = null) {
    const n = realInput.length;
    if ((n & n - 1) !== 0) {
      throw new Error("FFT size must be power of 2");
    }
    const real = new Float32Array(realInput);
    const imag = imagInput ? new Float32Array(imagInput) : new Float32Array(n);
    let j = 0;
    for (let i = 0; i < n - 1; i++) {
      if (i < j) {
        [real[i], real[j]] = [real[j], real[i]];
        [imag[i], imag[j]] = [imag[j], imag[i]];
      }
      let k = n >> 1;
      while (k <= j) {
        j -= k;
        k >>= 1;
      }
      j += k;
    }
    for (let len = 2; len <= n; len <<= 1) {
      const halfLen = len >> 1;
      const angle = -2 * Math.PI / len;
      for (let i = 0; i < n; i += len) {
        let wr = 1, wi = 0;
        for (let j2 = 0; j2 < halfLen; j2++) {
          const k = i + j2;
          const l = k + halfLen;
          const tr = wr * real[l] - wi * imag[l];
          const ti = wr * imag[l] + wi * real[l];
          real[l] = real[k] - tr;
          imag[l] = imag[k] - ti;
          real[k] += tr;
          imag[k] += ti;
          const wtemp = wr;
          wr = wtemp * Math.cos(angle) - wi * Math.sin(angle);
          wi = wtemp * Math.sin(angle) + wi * Math.cos(angle);
        }
      }
    }
    return { real, imag };
  }
  function ifft(realInput, imagInput) {
    const n = realInput.length;
    const imagConj = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      imagConj[i] = -imagInput[i];
    }
    const result = fft(realInput, imagConj);
    for (let i = 0; i < n; i++) {
      result.real[i] /= n;
      result.imag[i] = -result.imag[i] / n;
    }
    return result;
  }
  function hannWindow(n) {
    const window = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      window[i] = 0.5 * (1 - Math.cos(2 * Math.PI * i / n));
    }
    return window;
  }
  function padReflect(arr, padLeft, padRight) {
    const length = arr.length;
    const newLength = length + padLeft + padRight;
    const result = new Float32Array(newLength);
    for (let i = 0; i < length; i++) {
      result[padLeft + i] = arr[i];
    }
    for (let i = 0; i < padLeft; i++) {
      const srcIdx = padLeft - i;
      result[i] = result[srcIdx];
    }
    for (let i = 0; i < padRight; i++) {
      const dstIdx = padLeft + length + i;
      const srcIdx = padLeft + length - 2 - i;
      result[dstIdx] = result[srcIdx];
    }
    return result;
  }
  function pad1d(x, paddings, mode = "constant") {
    const length = x.shape[x.shape.length - 1];
    const [paddingLeft, paddingRight] = paddings;
    let xData = x.data;
    let xShape = x.shape;
    let actualPaddingLeft = paddingLeft;
    let actualPaddingRight = paddingRight;
    if (mode === "reflect") {
      const maxPad = Math.max(paddingLeft, paddingRight);
      if (length <= maxPad) {
        const extraPad = maxPad - length + 1;
        const extraPadRight = Math.min(paddingRight, extraPad);
        const extraPadLeft = extraPad - extraPadRight;
        const shape2 = [...xShape];
        shape2[shape2.length - 1] = length + extraPadLeft + extraPadRight;
        const totalSize2 = shape2.reduce((a, b) => a * b, 1);
        const tempData = new Float32Array(totalSize2);
        const outerSize2 = totalSize2 / shape2[shape2.length - 1];
        const newLength = shape2[shape2.length - 1];
        for (let i = 0; i < outerSize2; i++) {
          for (let j = 0; j < length; j++) {
            tempData[i * newLength + extraPadLeft + j] = xData[i * length + j];
          }
        }
        xData = tempData;
        xShape = shape2;
        actualPaddingLeft = paddingLeft - extraPadLeft;
        actualPaddingRight = paddingRight - extraPadRight;
      }
    }
    const currentLength = xShape[xShape.length - 1];
    const shape = [...xShape];
    shape[shape.length - 1] = currentLength + actualPaddingLeft + actualPaddingRight;
    const totalSize = shape.reduce((a, b) => a * b, 1);
    const out = new Float32Array(totalSize);
    const outerSize = totalSize / shape[shape.length - 1];
    const outputLength = shape[shape.length - 1];
    for (let i = 0; i < outerSize; i++) {
      const srcOffset = i * currentLength;
      const dstOffset = i * outputLength;
      if (mode === "constant") {
        for (let j = 0; j < currentLength; j++) {
          out[dstOffset + actualPaddingLeft + j] = xData[srcOffset + j];
        }
      } else if (mode === "reflect") {
        const slice = xData.slice(srcOffset, srcOffset + currentLength);
        const padded = padReflect(slice, actualPaddingLeft, actualPaddingRight);
        for (let j = 0; j < outputLength; j++) {
          out[dstOffset + j] = padded[j];
        }
      }
    }
    return { data: out, shape };
  }
  function stft(x, nFft, hopLength, window, normalized = true, center = true, padMode = "reflect") {
    const [batch, length] = x.shape;
    let inputData = x.data;
    let inputLength = length;
    if (center) {
      const pad = Math.floor(nFft / 2);
      const padded = pad1d(x, [pad, pad], padMode);
      inputData = padded.data;
      inputLength = padded.shape[padded.shape.length - 1];
    }
    const numFrames = Math.floor((inputLength - nFft) / hopLength) + 1;
    const numFreqs = Math.floor(nFft / 2) + 1;
    const realOut = new Float32Array(batch * numFreqs * numFrames);
    const imagOut = new Float32Array(batch * numFreqs * numFrames);
    const norm = normalized ? 1 / Math.sqrt(nFft) : 1;
    for (let b = 0; b < batch; b++) {
      for (let frame = 0; frame < numFrames; frame++) {
        const frameStart = frame * hopLength;
        const frameData = new Float32Array(nFft);
        for (let i = 0; i < nFft; i++) {
          const idx = b * inputLength + frameStart + i;
          frameData[i] = inputData[idx] * window[i] * norm;
        }
        const { real, imag } = fft(frameData);
        for (let freq = 0; freq < numFreqs; freq++) {
          const outIdx = b * numFreqs * numFrames + freq * numFrames + frame;
          realOut[outIdx] = real[freq];
          imagOut[outIdx] = imag[freq];
        }
      }
    }
    return {
      real: { data: realOut, shape: [batch, numFreqs, numFrames] },
      imag: { data: imagOut, shape: [batch, numFreqs, numFrames] }
    };
  }
  function spectro(x, nFft = 512, hopLength = null) {
    if (hopLength === null) {
      hopLength = Math.floor(nFft / 4);
    }
    const originalShape = x.shape;
    const length = originalShape[originalShape.length - 1];
    const otherSize = x.data.length / length;
    const reshapedX = {
      data: x.data,
      shape: [otherSize, length]
    };
    const window = hannWindow(nFft);
    const result = stft(reshapedX, nFft, hopLength, window, true, true, "reflect");
    const [batch, freqs, frames] = result.real.shape;
    const outputShape = [...originalShape.slice(0, -1), freqs, frames];
    return {
      real: { data: result.real.data, shape: outputShape },
      imag: { data: result.imag.data, shape: outputShape }
    };
  }
  function spec(x) {
    const hl = 1024;
    const lastDimLength = x.shape[x.shape.length - 1];
    const le = Math.ceil(lastDimLength / hl);
    const pad = Math.floor(hl / 2) * 3;
    const paddingRight = pad + le * hl - lastDimLength;
    const paddedX = pad1d(x, [pad, paddingRight], "reflect");
    const z = spectro(paddedX, 4096, hl);
    const zShape = z.real.shape;
    const newFreqs = zShape[zShape.length - 2] - 1;
    const frames = zShape[zShape.length - 1];
    const outerSize = z.real.data.length / (zShape[zShape.length - 2] * frames);
    const newShape = [...zShape];
    newShape[newShape.length - 2] = newFreqs;
    const newSize = newShape.reduce((a, b) => a * b, 1);
    const newReal = new Float32Array(newSize);
    const newImag = new Float32Array(newSize);
    for (let i = 0; i < outerSize; i++) {
      for (let freq = 0; freq < newFreqs; freq++) {
        for (let frame = 0; frame < frames; frame++) {
          const oldIdx = i * zShape[zShape.length - 2] * frames + freq * frames + frame;
          const newIdx = i * newFreqs * frames + freq * frames + frame;
          newReal[newIdx] = z.real.data[oldIdx];
          newImag[newIdx] = z.imag.data[oldIdx];
        }
      }
    }
    const slicedShape = [...newShape];
    slicedShape[slicedShape.length - 1] = le;
    const slicedSize = slicedShape.reduce((a, b) => a * b, 1);
    const slicedReal = new Float32Array(slicedSize);
    const slicedImag = new Float32Array(slicedSize);
    for (let i = 0; i < outerSize; i++) {
      for (let freq = 0; freq < newFreqs; freq++) {
        for (let frame = 0; frame < le; frame++) {
          const srcIdx = i * newFreqs * frames + freq * frames + (2 + frame);
          const dstIdx = i * newFreqs * le + freq * le + frame;
          slicedReal[dstIdx] = newReal[srcIdx];
          slicedImag[dstIdx] = newImag[srcIdx];
        }
      }
    }
    return {
      real: { data: slicedReal, shape: slicedShape },
      imag: { data: slicedImag, shape: slicedShape }
    };
  }
  function magnitude(z) {
    const [B, C, Fr, T] = z.real.shape;
    const outShape = [B, C * 2, Fr, T];
    const outSize = B * C * 2 * Fr * T;
    const out = new Float32Array(outSize);
    for (let b = 0; b < B; b++) {
      for (let c = 0; c < C; c++) {
        for (let fr = 0; fr < Fr; fr++) {
          for (let t = 0; t < T; t++) {
            const srcIdx = b * C * Fr * T + c * Fr * T + fr * T + t;
            const dstIdxReal = b * (C * 2) * Fr * T + c * 2 * Fr * T + fr * T + t;
            out[dstIdxReal] = z.real.data[srcIdx];
            const dstIdxImag = b * (C * 2) * Fr * T + (c * 2 + 1) * Fr * T + fr * T + t;
            out[dstIdxImag] = z.imag.data[srcIdx];
          }
        }
      }
    }
    return { data: out, shape: outShape };
  }
  function istft(z, nFft, hopLength, window, normalized = true, length = null, center = true) {
    const [batch, freqs, numFrames] = z.real.shape;
    if (2 * freqs - 2 !== nFft) {
      throw new Error(`Expected freqs = nFft/2 + 1, got freqs=${freqs}, nFft=${nFft}`);
    }
    let outputLength;
    if (length !== null) {
      outputLength = length;
    } else {
      outputLength = nFft + (numFrames - 1) * hopLength;
    }
    if (center) {
      outputLength -= nFft;
    }
    const output = new Float32Array(batch * outputLength);
    const windowSum = new Float32Array(batch * outputLength);
    const norm = normalized ? Math.sqrt(nFft) : 1;
    for (let b = 0; b < batch; b++) {
      for (let frame = 0; frame < numFrames; frame++) {
        const fullReal = new Float32Array(nFft);
        const fullImag = new Float32Array(nFft);
        for (let freq = 0; freq < freqs; freq++) {
          const idx = b * freqs * numFrames + freq * numFrames + frame;
          fullReal[freq] = z.real.data[idx];
          fullImag[freq] = z.imag.data[idx];
        }
        for (let freq = freqs; freq < nFft; freq++) {
          const mirrorFreq = nFft - freq;
          fullReal[freq] = fullReal[mirrorFreq];
          fullImag[freq] = -fullImag[mirrorFreq];
        }
        const frameData = ifft(fullReal, fullImag);
        const frameStart = frame * hopLength - (center ? nFft / 2 : 0);
        for (let i = 0; i < nFft; i++) {
          const outputIdx = frameStart + i;
          if (outputIdx >= 0 && outputIdx < outputLength) {
            const globalIdx = b * outputLength + outputIdx;
            output[globalIdx] += frameData.real[i] * window[i] * norm;
            windowSum[globalIdx] += window[i] * window[i];
          }
        }
      }
    }
    for (let i = 0; i < output.length; i++) {
      if (windowSum[i] > 1e-8) {
        output[i] /= windowSum[i];
      }
    }
    return {
      data: output,
      shape: [batch, outputLength]
    };
  }
  function ispectro(z, hopLength = null, length = null) {
    const originalShape = z.real.shape;
    const freqs = originalShape[originalShape.length - 2];
    const frames = originalShape[originalShape.length - 1];
    const nFft = 2 * freqs - 2;
    if (hopLength === null) {
      hopLength = Math.floor(nFft / 4);
    }
    const otherSize = z.real.data.length / (freqs * frames);
    const reshapedZ = {
      real: { data: z.real.data, shape: [otherSize, freqs, frames] },
      imag: { data: z.imag.data, shape: [otherSize, freqs, frames] }
    };
    const window = hannWindow(nFft);
    const result = istft(reshapedZ, nFft, hopLength, window, true, length, true);
    const outputLength = result.shape[1];
    const outputShape = [...originalShape.slice(0, -2), outputLength];
    return {
      data: result.data,
      shape: outputShape
    };
  }
  function padComplex(z, padFreq, padTime) {
    const shape = z.real.shape;
    const ndim = shape.length;
    const [padFreqLeft, padFreqRight] = padFreq;
    const [padTimeLeft, padTimeRight] = padTime;
    const oldFreqs = shape[ndim - 2];
    const oldFrames = shape[ndim - 1];
    const newFreqs = oldFreqs + padFreqLeft + padFreqRight;
    const newFrames = oldFrames + padTimeLeft + padTimeRight;
    const newShape = [...shape];
    newShape[ndim - 2] = newFreqs;
    newShape[ndim - 1] = newFrames;
    const totalSize = newShape.reduce((a, b) => a * b, 1);
    const newReal = new Float32Array(totalSize);
    const newImag = new Float32Array(totalSize);
    const outerSize = totalSize / (newFreqs * newFrames);
    for (let i = 0; i < outerSize; i++) {
      for (let freq = 0; freq < oldFreqs; freq++) {
        for (let frame = 0; frame < oldFrames; frame++) {
          const oldIdx = i * oldFreqs * oldFrames + freq * oldFrames + frame;
          const newIdx = i * newFreqs * newFrames + (freq + padFreqLeft) * newFrames + (frame + padTimeLeft);
          newReal[newIdx] = z.real.data[oldIdx];
          newImag[newIdx] = z.imag.data[oldIdx];
        }
      }
    }
    return {
      real: { data: newReal, shape: newShape },
      imag: { data: newImag, shape: newShape }
    };
  }
  function ispec(z, length) {
    const hl = 1024;
    let paddedZ = padComplex(z, [0, 1], [0, 0]);
    paddedZ = padComplex(paddedZ, [0, 0], [2, 2]);
    const pad = Math.floor(hl / 2) * 3;
    const le = hl * Math.ceil(length / hl) + 2 * pad;
    const x = ispectro(paddedZ, hl, le);
    const shape = x.shape;
    const lastDim = shape.length - 1;
    const totalLength = shape[lastDim];
    const newShape = [...shape];
    newShape[lastDim] = length;
    const totalSize = newShape.reduce((a, b) => a * b, 1);
    const newData = new Float32Array(totalSize);
    const outerSize = totalSize / length;
    for (let i = 0; i < outerSize; i++) {
      for (let j = 0; j < length; j++) {
        const oldIdx = i * totalLength + pad + j;
        const newIdx = i * length + j;
        newData[newIdx] = x.data[oldIdx];
      }
    }
    return { data: newData, shape: newShape };
  }

  // ../../../../../tmp/tmp.UCJ2xT1FrC/demucs/package/dist/wav-utils.js
  function planarize(channelData) {
    const channels = channelData.length;
    const samples = channelData[0].length;
    if (channels === 1) {
      return channelData[0];
    } else {
      let isSequential = true;
      for (let c = 1; c < channels; c++) {
        if (channelData[c].buffer !== channelData[0].buffer || channelData[c].byteOffset !== channelData[c - 1].byteOffset + channelData[c - 1].length * 4) {
          isSequential = false;
          break;
        }
      }
      if (isSequential) {
        return new Float32Array(channelData[0].buffer, channelData[0].byteOffset, channels * samples);
      } else {
        let data = new Float32Array(channels * samples);
        for (let c = 0; c < channels; c++) {
          data.set(channelData[c], c * samples);
        }
        return data;
      }
    }
  }

  // ../../../../../tmp/tmp.UCJ2xT1FrC/demucs/package/dist/apply.js
  var TensorChunk = class {
    constructor(tensor, offset = 0, length = null) {
      __publicField(this, "tensor");
      __publicField(this, "offset");
      __publicField(this, "length");
      const totalLength = tensor.shape[tensor.shape.length - 1];
      if (offset < 0)
        throw new Error("offset must be >= 0");
      if (offset >= totalLength)
        throw new Error("offset must be < totalLength");
      if (length === null) {
        length = totalLength - offset;
      } else {
        length = Math.min(totalLength - offset, length);
      }
      this.tensor = tensor;
      this.offset = offset;
      this.length = length;
    }
    padded(targetLength) {
      const delta = targetLength - this.length;
      const totalLength = this.tensor.shape[this.tensor.shape.length - 1];
      if (delta < 0)
        throw new Error("targetLength must be >= length");
      const start = this.offset - Math.floor(delta / 2);
      const end = start + targetLength;
      const correctStart = Math.max(0, start);
      const correctEnd = Math.min(totalLength, end);
      const padLeft = correctStart - start;
      const padRight = end - correctEnd;
      const shape = [...this.tensor.shape];
      const lastDim = shape.length - 1;
      shape[lastDim] = targetLength;
      const totalSize = shape.reduce((a, b) => a * b, 1);
      const out = new Float32Array(totalSize);
      const sliceLength = correctEnd - correctStart;
      const innerSize = shape[lastDim];
      const outerSize = totalSize / innerSize;
      for (let i = 0; i < outerSize; i++) {
        const srcOffset = i * totalLength + correctStart;
        const dstOffset = i * innerSize + padLeft;
        for (let j = 0; j < sliceLength; j++) {
          out[dstOffset + j] = this.tensor.data[srcOffset + j];
        }
      }
      if (out.length !== totalSize) {
        throw new Error(`Output size mismatch: ${out.length} !== ${totalSize}`);
      }
      return {
        data: out,
        shape
      };
    }
  };
  function padToTrainingLength(mix, trainingLength) {
    const currentLength = mix.shape[mix.shape.length - 1];
    if (currentLength >= trainingLength) {
      return mix;
    }
    const shape = [...mix.shape];
    shape[shape.length - 1] = trainingLength;
    const totalSize = shape.reduce((a, b) => a * b, 1);
    const out = new Float32Array(totalSize);
    const outerSize = totalSize / trainingLength;
    for (let i = 0; i < outerSize; i++) {
      const srcOffset = i * currentLength;
      const dstOffset = i * trainingLength;
      for (let j = 0; j < currentLength; j++) {
        out[dstOffset + j] = mix.data[srcOffset + j];
      }
    }
    return { data: out, shape };
  }
  function mask(m) {
    const [B, S, C, Fr, T] = m.shape;
    const C_half = C / 2;
    const outShape = [B, S, C_half, Fr, T];
    const outSize = outShape.reduce((a, b) => a * b, 1);
    const realOut = new Float32Array(outSize);
    const imagOut = new Float32Array(outSize);
    for (let b = 0; b < B; b++) {
      for (let s = 0; s < S; s++) {
        for (let c = 0; c < C_half; c++) {
          for (let fr = 0; fr < Fr; fr++) {
            for (let t = 0; t < T; t++) {
              const realChannel = c * 2;
              const imagChannel = c * 2 + 1;
              const realInIdx = b * S * C * Fr * T + s * C * Fr * T + realChannel * Fr * T + fr * T + t;
              const imagInIdx = b * S * C * Fr * T + s * C * Fr * T + imagChannel * Fr * T + fr * T + t;
              const outIdx = b * S * C_half * Fr * T + s * C_half * Fr * T + c * Fr * T + fr * T + t;
              realOut[outIdx] = m.data[realInIdx];
              imagOut[outIdx] = m.data[imagInIdx];
            }
          }
        }
      }
    }
    return {
      real: { data: realOut, shape: outShape },
      imag: { data: imagOut, shape: outShape }
    };
  }
  function addTensors(a, b) {
    if (a.shape.length !== b.shape.length || !a.shape.every((dim, i) => dim === b.shape[i])) {
      throw new Error("Tensor shapes must match for addition");
    }
    const out = new Float32Array(a.data.length);
    for (let i = 0; i < a.data.length; i++) {
      out[i] = a.data[i] + b.data[i];
    }
    return { data: out, shape: [...a.shape] };
  }
  function cropToValidLength(x, validLength) {
    const shape = x.shape;
    const currentLength = shape[shape.length - 1];
    if (validLength >= currentLength) {
      return x;
    }
    const newShape = [...shape];
    newShape[newShape.length - 1] = validLength;
    const totalSize = newShape.reduce((a, b) => a * b, 1);
    const out = new Float32Array(totalSize);
    const outerSize = totalSize / validLength;
    for (let i = 0; i < outerSize; i++) {
      for (let j = 0; j < validLength; j++) {
        const srcIdx = i * currentLength + j;
        const dstIdx = i * validLength + j;
        out[dstIdx] = x.data[srcIdx];
      }
    }
    return { data: out, shape: newShape };
  }
  function centerTrim(tensor, reference) {
    const tensorLength = tensor.shape[tensor.shape.length - 1];
    const refSize = reference;
    const delta = tensorLength - refSize;
    if (delta < 0) {
      throw new Error(`tensor must be larger than reference. Delta is ${delta}.`);
    }
    if (delta === 0) {
      return tensor;
    }
    const start = Math.floor(delta / 2);
    const end = tensorLength - (delta - Math.floor(delta / 2));
    const shape = [...tensor.shape];
    const newLength = end - start;
    shape[shape.length - 1] = newLength;
    const totalSize = shape.reduce((a, b) => a * b, 1);
    const out = new Float32Array(totalSize);
    const outerSize = totalSize / newLength;
    for (let i = 0; i < outerSize; i++) {
      for (let j = 0; j < newLength; j++) {
        const srcIdx = i * tensorLength + start + j;
        const dstIdx = i * newLength + j;
        out[dstIdx] = tensor.data[srcIdx];
      }
    }
    return { data: out, shape };
  }
  async function applyInference(model2, mix) {
    const length = mix.length;
    const validLength = model2.validLength(length);
    const paddedMix = mix.padded(validLength);
    const trainingLength = Math.floor(model2.segment * model2.samplerate);
    const paddedPaddedMix = padToTrainingLength(paddedMix, trainingLength);
    const z = spec(paddedPaddedMix);
    const magspec = magnitude(z);
    const { outX, outXt } = await model2.forward(paddedMix, magspec);
    const zout = mask(outX);
    const timeFromSpec = ispec(zout, trainingLength);
    const sumBeforeCrop = addTensors(outXt, timeFromSpec);
    const out = cropToValidLength(sumBeforeCrop, validLength);
    return centerTrim(out, length);
  }
  async function applySplits(model2, mix, progressCallback, overlap = 0.25) {
    const [batch, channels, length] = mix.shape;
    const sources = model2.sources.length;
    const outData = new Float32Array(batch * sources * channels * length);
    const sumWeight = new Float32Array(length);
    const segment = Math.floor(model2.samplerate * model2.segment);
    const stride = Math.floor((1 - overlap) * segment);
    const weight = new Float32Array(segment);
    for (let i = 0; i < Math.floor(segment / 2) + 1; i++) {
      weight[i] = i + 1;
    }
    for (let i = Math.floor(segment / 2) + 1; i < segment; i++) {
      weight[i] = segment - i;
    }
    const maxWeight = weight.reduce((max, current) => Math.max(max, current), -Infinity);
    for (let i = 0; i < segment; i++) {
      weight[i] /= maxWeight;
    }
    const total = Math.ceil(length / stride);
    progressCallback?.(0, total);
    let offset = 0;
    let chunkIndex = 0;
    while (offset < length) {
      const chunk = new TensorChunk(mix, offset, segment);
      const chunkOut = await applyInference(model2, chunk);
      const chunkLength = chunkOut.shape[chunkOut.shape.length - 1];
      for (let b = 0; b < batch; b++) {
        for (let s = 0; s < sources; s++) {
          for (let c = 0; c < channels; c++) {
            for (let t = 0; t < chunkLength; t++) {
              const outIdx = b * sources * channels * length + s * channels * length + c * length + offset + t;
              const chunkIdx = b * sources * channels * chunkLength + s * channels * chunkLength + c * chunkLength + t;
              outData[outIdx] += weight[t] * chunkOut.data[chunkIdx];
            }
          }
        }
      }
      for (let t = 0; t < chunkLength; t++) {
        sumWeight[offset + t] += weight[t];
      }
      offset += stride;
      chunkIndex++;
      progressCallback?.(chunkIndex, total);
    }
    for (let i = 0; i < outData.length; i++) {
      const timeIdx = i % length;
      outData[i] /= sumWeight[timeIdx];
    }
    return {
      data: outData,
      shape: [batch, sources, channels, length]
    };
  }
  async function applyModel(model2, mix, progressCallback, overlap = 0.25) {
    return await applySplits(model2, mix, progressCallback, overlap);
  }
  async function separateTracks(model2, rawAudio, progressCallback, overlap = 0.25) {
    const { channelData, sampleRate } = rawAudio;
    const channels = channelData.length;
    const samples = channelData[0].length;
    const data = planarize(rawAudio.channelData);
    const mix = {
      data,
      shape: [1, channels, samples]
    };
    const result = await applyModel(model2, mix, progressCallback, overlap);
    const [batch, sources, resultChannels, length] = result.shape;
    const tracks = {};
    for (let s = 0; s < sources; s++) {
      const sourceName = model2.sources[s];
      const channelData2 = [];
      for (let c = 0; c < channels; c++) {
        const startIdx = s * channels * length + c * length;
        const channelArray = new Float32Array(
          result.data.buffer,
          startIdx * 4,
          // byte offset (Float32 = 4 bytes)
          length
        );
        channelData2.push(channelArray);
      }
      tracks[sourceName] = { channelData: channelData2, sampleRate };
    }
    return tracks;
  }

  // src/worker-entry.js
  var ort = self.ort;
  var model = null;
  var post = (msg, transfer) => self.postMessage(msg, transfer || []);
  function idb() {
    return new Promise((resolve, reject) => {
      const r = indexedDB.open("stem-loop", 1);
      r.onupgradeneeded = () => r.result.createObjectStore("files");
      r.onsuccess = () => resolve(r.result);
      r.onerror = () => reject(r.error);
    });
  }
  async function idbGet(key) {
    try {
      const db = await idb();
      return await new Promise((res) => {
        const t = db.transaction("files").objectStore("files").get(key);
        t.onsuccess = () => res(t.result || null);
        t.onerror = () => res(null);
      });
    } catch (e) {
      return null;
    }
  }
  async function idbPut(key, value) {
    try {
      const db = await idb();
      await new Promise((res) => {
        const t = db.transaction("files", "readwrite");
        t.objectStore("files").put(value, key);
        t.oncomplete = res;
        t.onerror = res;
        t.onabort = res;
      });
    } catch (e) {
    }
  }
  async function fetchParts(urls, label, totalHint, onBytes) {
    const parts = [];
    for (const u of urls) {
      const res = await fetch(u);
      if (!res.ok) throw new Error(`Couldn't download ${label} (${res.status}).`);
      const reader = res.body.getReader();
      const chunks = [];
      for (; ; ) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        onBytes(value.byteLength);
      }
      parts.push(...chunks);
    }
    const size = parts.reduce((n, c) => n + c.byteLength, 0);
    const out = new Uint8Array(size);
    let o2 = 0;
    for (const c of parts) {
      out.set(c, o2);
      o2 += c.byteLength;
    }
    return out;
  }
  async function loadModel({ wasmUrls, wasmBytes, mjsUrl, modelUrls, modelBytes, cacheKey }) {
    let wasm = await idbGet(cacheKey + ":wasm");
    let weights = await idbGet(cacheKey + ":model");
    const cached = !!(wasm && weights);
    const total = (wasm ? 0 : wasmBytes) + (weights ? 0 : modelBytes);
    let loaded = 0;
    const onBytes = (n) => {
      loaded += n;
      post({ type: "progress", stage: "download", loaded, total });
    };
    if (!wasm) {
      wasm = await fetchParts(wasmUrls, "the audio engine", wasmBytes, onBytes);
      await idbPut(cacheKey + ":wasm", wasm);
    }
    if (!weights) {
      weights = await fetchParts(modelUrls, "the separation model", modelBytes, onBytes);
      await idbPut(cacheKey + ":model", weights);
    }
    post({ type: "progress", stage: "init" });
    ort.env.wasm.numThreads = 1;
    ort.env.wasm.proxy = false;
    ort.env.wasm.wasmBinary = wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength);
    ort.env.wasm.wasmPaths = { mjs: mjsUrl };
    let ep = "cpu";
    let session = null;
    if (self.navigator && navigator.gpu) {
      try {
        const adapter = await navigator.gpu.requestAdapter();
        if (adapter) {
          session = await ort.InferenceSession.create(weights, { executionProviders: ["webgpu"] });
          ep = "gpu";
        }
      } catch (e) {
        session = null;
      }
    }
    if (!session) session = await ort.InferenceSession.create(weights, { executionProviders: ["wasm"] });
    const m = new ONNXHTDemucs();
    m.session = session;
    m.inputNames = session.inputNames;
    m.outputNames = session.outputNames;
    model = m;
    post({ type: "ready", ep, cached });
  }
  async function separate({ left, right, sampleRate }) {
    if (!model) throw new Error("Model not loaded.");
    const t0 = performance.now();
    const tracks = await separateTracks(model, { channelData: [left, right], sampleRate }, (done, total) => {
      post({ type: "progress", stage: "separate", done, total, elapsed: (performance.now() - t0) / 1e3 });
    }, 0.25);
    const names = Object.keys(tracks);
    const out = {};
    const transfer = [];
    for (const n of names) {
      const [l, r] = tracks[n].channelData;
      const L = new Float32Array(l), R = new Float32Array(r);
      out[n] = [L, R];
      transfer.push(L.buffer, R.buffer);
    }
    post({ type: "stems", stems: out }, transfer);
  }
  var fftCache = /* @__PURE__ */ new Map();
  function fftTables(n) {
    let t = fftCache.get(n);
    if (t) return t;
    const rev = new Uint32Array(n);
    const bits = Math.log2(n);
    for (let i = 0; i < n; i++) {
      let r = 0, x = i;
      for (let b = 0; b < bits; b++) {
        r = r << 1 | x & 1;
        x >>= 1;
      }
      rev[i] = r;
    }
    const cos = new Float64Array(n / 2), sin = new Float64Array(n / 2);
    for (let i = 0; i < n / 2; i++) {
      cos[i] = Math.cos(2 * Math.PI * i / n);
      sin[i] = Math.sin(2 * Math.PI * i / n);
    }
    t = { rev, cos, sin };
    fftCache.set(n, t);
    return t;
  }
  function fft2(re, im, inverse) {
    const n = re.length, { rev, cos, sin } = fftTables(n);
    for (let i = 0; i < n; i++) {
      const j = rev[i];
      if (j > i) {
        let t = re[i];
        re[i] = re[j];
        re[j] = t;
        t = im[i];
        im[i] = im[j];
        im[j] = t;
      }
    }
    for (let size = 2; size <= n; size <<= 1) {
      const half = size >> 1, step = n / size;
      for (let i = 0; i < n; i += size) {
        for (let j = 0, k = 0; j < half; j++, k += step) {
          const wr = cos[k], wi = inverse ? sin[k] : -sin[k];
          const a = i + j, b = a + half;
          const tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
          re[b] = re[a] - tr;
          im[b] = im[a] - ti;
          re[a] += tr;
          im[a] += ti;
        }
      }
    }
    if (inverse) for (let i = 0; i < n; i++) {
      re[i] /= n;
      im[i] /= n;
    }
  }
  function stretch(x, alpha, onProgress) {
    const N = 4096, Hs = 1024, Ha = Hs / alpha;
    const outLen = Math.max(1, Math.round(x.length * alpha));
    const y = new Float32Array(outLen + N);
    const norm = new Float32Array(outLen + N);
    const win = new Float64Array(N);
    for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N);
    const bins = N / 2 + 1;
    const re = new Float64Array(N), im = new Float64Array(N);
    const mag = new Float64Array(bins), ph = new Float64Array(bins);
    const prevPh = new Float64Array(bins), synPh = new Float64Array(bins), prevMag = new Float64Array(bins);
    const peakOf = new Int32Array(bins);
    const frames = Math.ceil(outLen / Hs) + 1;
    let prevPos = 0;
    for (let m = 0; m < frames; m++) {
      const pos = Math.round(m * Ha) - N / 2, sPos = m * Hs - N / 2;
      for (let i = 0; i < N; i++) {
        const s = pos + i;
        re[i] = (s >= 0 && s < x.length ? x[s] : 0) * win[i];
        im[i] = 0;
      }
      fft2(re, im, false);
      let flux = 0, energy = 0;
      for (let k = 0; k < bins; k++) {
        mag[k] = Math.hypot(re[k], im[k]);
        ph[k] = Math.atan2(im[k], re[k]);
        const d = mag[k] - prevMag[k];
        if (d > 0) flux += d;
        energy += mag[k];
      }
      const transient = m === 0 || flux > 0.55 * energy + 1e-9 && energy > 1e-3;
      if (transient) {
        for (let k = 0; k < bins; k++) synPh[k] = ph[k];
      } else {
        const ha = pos - prevPos;
        const peaks = [];
        for (let k = 2; k < bins - 2; k++) if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1] && mag[k] > mag[k - 2] && mag[k] >= mag[k + 2]) peaks.push(k);
        if (!peaks.length) peaks.push(0);
        let pi = 0;
        for (let k = 0; k < bins; k++) {
          while (pi < peaks.length - 1 && Math.abs(peaks[pi + 1] - k) <= Math.abs(peaks[pi] - k)) pi++;
          peakOf[k] = peaks[pi];
        }
        const newPh = new Float64Array(peaks.length);
        for (let q = 0; q < peaks.length; q++) {
          const k = peaks[q], w = 2 * Math.PI * k / N;
          let d = ph[k] - prevPh[k] - w * ha;
          d -= 2 * Math.PI * Math.round(d / (2 * Math.PI));
          newPh[q] = synPh[k] + (w + d / Math.max(1, ha)) * Hs;
        }
        const peakSyn = /* @__PURE__ */ new Map();
        peaks.forEach((k, q) => peakSyn.set(k, newPh[q]));
        for (let k = 0; k < bins; k++) {
          const p = peakOf[k];
          synPh[k] = peakSyn.get(p) + (ph[k] - ph[p]);
        }
      }
      for (let k = 0; k < bins; k++) {
        prevPh[k] = ph[k];
        prevMag[k] = mag[k];
      }
      prevPos = pos;
      for (let k = 0; k < bins; k++) {
        re[k] = mag[k] * Math.cos(synPh[k]);
        im[k] = mag[k] * Math.sin(synPh[k]);
      }
      for (let k = 1; k < N / 2; k++) {
        re[N - k] = re[k];
        im[N - k] = -im[k];
      }
      fft2(re, im, true);
      for (let i = 0; i < N; i++) {
        const s = sPos + i;
        if (s < 0 || s >= y.length) continue;
        y[s] += re[i] * win[i];
        norm[s] += win[i] * win[i];
      }
      if (onProgress && (m & 63) === 0) onProgress(m / frames);
    }
    const out = new Float32Array(outLen);
    for (let i = 0; i < outLen; i++) out[i] = norm[i] > 1e-6 ? y[i] / norm[i] : 0;
    return out;
  }
  function resample(x, step) {
    const len = Math.max(1, Math.floor(x.length / step));
    const y = new Float32Array(len);
    for (let i = 0; i < len; i++) {
      const p = i * step, j = Math.floor(p), t = p - j;
      const a = x[j - 1] ?? x[0] ?? 0, b = x[j] ?? 0, c = x[j + 1] ?? b, d = x[j + 2] ?? c;
      y[i] = b + 0.5 * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
    }
    return y;
  }
  function lowpass(x, cutoffRatio) {
    if (cutoffRatio >= 0.98) return x;
    const rc = 1 / (2 * Math.PI * cutoffRatio * 0.5), dt = 1, a = dt / (rc + dt);
    const y = new Float32Array(x.length);
    let s1 = 0, s2 = 0;
    for (let i = 0; i < x.length; i++) {
      s1 += a * (x[i] - s1);
      s2 += a * (s1 - s2);
      y[i] = s2;
    }
    return y;
  }
  function warpChannel(x, mode, speed, semitones, onProgress) {
    if (mode === "repitch") {
      if (Math.abs(speed - 1) < 1e-6) return x.slice();
      return resample(speed > 1 ? lowpass(x, 1 / speed) : x, speed);
    }
    const r = Math.pow(2, semitones / 12);
    if (Math.abs(speed - 1) < 1e-6 && Math.abs(r - 1) < 1e-6) return x.slice();
    const s = stretch(x, r / speed, onProgress);
    if (Math.abs(r - 1) < 1e-6) return s;
    return resample(r > 1 ? lowpass(s, 1 / r) : s, r);
  }
  function warp({ id, stems, mode, speed, semitones }) {
    const names = Object.keys(stems);
    const total = names.length * 2;
    let done = 0;
    const out = {};
    const transfer = [];
    for (const n of names) {
      out[n] = stems[n].map((ch) => {
        const y = warpChannel(ch, mode, speed, semitones, (p) => post({ type: "progress", stage: "warp", id, done: done + p, total }));
        done++;
        post({ type: "progress", stage: "warp", id, done, total });
        transfer.push(y.buffer);
        return y;
      });
    }
    post({ type: "warped", id, stems: out }, transfer);
  }
  function analyze({ mono, sampleRate }) {
    const N = 1024, hop = Math.round(sampleRate / 100);
    const frames = Math.floor((mono.length - N) / hop);
    const env2 = new Float32Array(Math.max(1, frames)), low = new Float32Array(Math.max(1, frames));
    const re = new Float64Array(N), im = new Float64Array(N);
    let prev = new Float64Array(N / 2);
    const win = new Float64Array(N);
    for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N);
    for (let f = 0; f < frames; f++) {
      for (let i = 0; i < N; i++) {
        re[i] = mono[f * hop + i] * win[i];
        im[i] = 0;
      }
      fft2(re, im, false);
      let flux = 0, lowFlux = 0;
      const cur = new Float64Array(N / 2);
      const lowBins = Math.round(150 * N / sampleRate);
      for (let k = 1; k < N / 2; k++) {
        cur[k] = Math.log1p(100 * Math.hypot(re[k], im[k]));
        const d = cur[k] - prev[k];
        if (d > 0) {
          flux += d;
          if (k <= lowBins) lowFlux += d;
        }
      }
      env2[f] = flux;
      low[f] = lowFlux;
      prev = cur;
    }
    const w = 16;
    const hp = new Float32Array(env2.length);
    for (let i = 0; i < env2.length; i++) {
      let s = 0, c = 0;
      for (let j = Math.max(0, i - w); j < Math.min(env2.length, i + w); j++) {
        s += env2[j];
        c++;
      }
      hp[i] = Math.max(0, env2[i] - s / c);
    }
    const maxLag = Math.min(hp.length - 1, 1700);
    const ac = new Float64Array(maxLag + 1);
    for (let lag2 = 1; lag2 <= maxLag; lag2++) {
      let s = 0;
      for (let i = lag2; i < hp.length; i++) s += hp[i] * hp[i - lag2];
      ac[lag2] = s / (hp.length - lag2);
    }
    const at = (l) => {
      const i = Math.floor(l), f = l - i;
      return i + 1 <= maxLag ? ac[i] * (1 - f) + ac[i + 1] * f : 0;
    };
    let best = -1, bestLag = 50;
    for (let lag2 = 30; lag2 <= 100; lag2 += 0.25) {
      const bpm2 = 6e3 / lag2;
      const wgt = Math.exp(-0.5 * Math.pow(Math.log2(bpm2 / 115) / 1, 2));
      const s = (at(lag2) + 0.5 * at(lag2 * 2) + 0.35 * at(lag2 * 4) + 0.25 * at(lag2 / 2)) * wgt;
      if (s > best) {
        best = s;
        bestLag = lag2;
      }
    }
    let lag = bestLag, bestR = -1;
    for (let l = bestLag - 3; l <= bestLag + 3; l += 0.01) {
      let s = 0;
      for (let k = 1; k <= 16 && k * l < maxLag; k++) s += at(k * l);
      if (s > bestR) {
        bestR = s;
        lag = l;
      }
    }
    let bpm = 6e3 / lag;
    bpm = Math.round(bpm * 10) / 10;
    const P = 6e3 / bpm;
    let bestOff = 0, bestS = -1;
    for (let o2 = 0; o2 < P; o2 += 0.25) {
      let s = 0;
      for (let t = o2; t < hp.length; t += P) s += hp[Math.round(t)] || 0;
      if (s > bestS) {
        bestS = s;
        bestOff = o2;
      }
    }
    let bestBeat = 0, bestLow = -1;
    for (let b = 0; b < 4; b++) {
      let s = 0;
      for (let t = bestOff + b * P; t < low.length; t += 4 * P) s += Math.max(low[Math.round(t)] || 0, low[Math.round(t) + 1] || 0);
      if (s > bestLow) {
        bestLow = s;
        bestBeat = b;
      }
    }
    const fps = sampleRate / hop;
    const latency = N / 2 / sampleRate;
    let offset = (bestOff + bestBeat * P) / fps + latency;
    const bar = 4 * 60 / bpm;
    offset = (offset % bar + bar) % bar;
    if (offset > bar - 0.03) offset -= bar;
    offset = Math.max(0, offset);
    post({ type: "tempo", bpm, offset });
  }
  self.onmessage = async (e) => {
    const m = e.data;
    try {
      if (m.type === "load") await loadModel(m);
      else if (m.type === "separate") await separate(m);
      else if (m.type === "warp") warp(m);
      else if (m.type === "analyze") analyze(m);
    } catch (err) {
      post({ type: "error", stage: m.type, message: err && err.message || String(err) });
    }
  };
})();
