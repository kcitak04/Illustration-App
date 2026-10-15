// DATTRACK plugin: AudioWorklet tap at the end of DATTRACK's master chain.
// Batches the stereo output into 1024-frame chunks stamped with their AudioContext frame number
// and hands them to the forwarding worker (which posts them to the plugin over localhost).
class DTTap extends AudioWorkletProcessor {
  constructor() {
    super();
    this.N = 1024; this.buf = new Float32Array(this.N * 2); this.n = 0; this.out = null;
    this.port.onmessage = e => { if (e.data && e.data.port) this.out = e.data.port; };
  }
  process(inputs) {
    const inp = inputs[0], L = inp && inp[0], R = inp && (inp[1] || inp[0]), len = L ? L.length : 128;
    for (let i = 0; i < len; i++) {
      this.buf[this.n * 2] = L ? L[i] : 0; this.buf[this.n * 2 + 1] = R ? R[i] : 0;
      if (++this.n === this.N) {
        if (this.out) this.out.postMessage({ frame: currentFrame + i + 1 - this.N, data: this.buf }, [this.buf.buffer]);
        this.buf = new Float32Array(this.N * 2); this.n = 0;
      }
    }
    return true;
  }
}
registerProcessor('dt-tap', DTTap);
