#!/usr/bin/env bash
# Bundles src/*.js into index.html (one self-contained page).
set -euo pipefail
cd "$(dirname "$0")"
python3 - <<'PY'
order = ["core", "render", "media", "tools", "timeline", "ui", "export", "main"]
js = "(() => {\n" + "\n".join(open(f"src/{n}.js").read() for n in order) + "\n})();"
tpl = open("template.html").read()
open("index.html", "w").write(tpl.replace("/*BUNDLE*/", js))
print("index.html", len(tpl) + len(js), "bytes")
PY
