#!/usr/bin/env bash
# Builds Stem Loop's worker and splits the model / engine into <15 MB parts for hosting.
# Usage: ./build.sh [ort-base-url]
set -euo pipefail
cd "$(dirname "$0")"
ORT_VERSION=1.23.0
DEMUCS_VERSION=1.0.0
ORT_BASE="${1:-https://cdn.jsdelivr.net/npm/onnxruntime-web@${ORT_VERSION}/dist}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

(cd "$WORK" && npm pack "demucs@${DEMUCS_VERSION}" "onnxruntime-web@${ORT_VERSION}" -q >/dev/null \
  && mkdir demucs ort && tar xzf demucs-${DEMUCS_VERSION}.tgz -C demucs && tar xzf onnxruntime-web-${ORT_VERSION}.tgz -C ort \
  && npm init -y >/dev/null && npm i esbuild@0.25.10 -s >/dev/null)

"$WORK/node_modules/.bin/esbuild" src/worker-entry.js --bundle --format=iife --target=es2020 \
  --alias:demucs-dist="$WORK/demucs/package/dist" --alias:onnxruntime-node=./src/ort-shim.js \
  --banner:js="importScripts(\"${ORT_BASE}/ort.min.js\");" \
  --outfile=worker.js --log-level=warning

rm -rf model engine && mkdir -p model engine
split -b 14000000 -d -a 2 --additional-suffix=.wasm "$WORK/demucs/package/htdemucs.onnx" model/htdemucs.
split -b 14000000 -d -a 2 --additional-suffix=.wasm "$WORK/ort/package/dist/ort-wasm-simd-threaded.jsep.wasm" engine/ort-jsep.

python3 - "$ORT_BASE" <<'PY'
import json, os, sys
def parts(d): return sorted(f"{d}/{f}" for f in os.listdir(d))
m = {
  "ortMjs": sys.argv[1] + "/ort-wasm-simd-threaded.jsep.mjs",
  "model": parts("model"), "modelBytes": sum(os.path.getsize(p) for p in parts("model")),
  "engine": parts("engine"), "engineBytes": sum(os.path.getsize(p) for p in parts("engine")),
  "cacheKey": "htdemucs-1.0.0+ort-1.23.0",
}
json.dump(m, open("manifest.json", "w"), indent=1)
print(json.dumps({k: (v if not isinstance(v, list) else len(v)) for k, v in m.items()}))
PY
