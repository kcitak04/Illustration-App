// Stands in for "onnxruntime-node" inside the demucs package: the worker loads ONNX Runtime Web
// with importScripts (see build.sh banner), which defines the global `ort`.
const o = self.ort;
export const env = o.env;
export const InferenceSession = o.InferenceSession;
export const Tensor = o.Tensor;
