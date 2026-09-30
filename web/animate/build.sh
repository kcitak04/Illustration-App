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

# Android app variant: a full HTML document with the libraries bundled locally (works offline).
import os
page = tpl.replace("/*BUNDLE*/", js)
page = page.replace("https://cdn.jsdelivr.net/npm/mp4-muxer@5.2.2/build/mp4-muxer.js", "lib/mp4-muxer.js")
page = page.replace("https://cdn.jsdelivr.net/npm/webm-muxer@5.1.4/build/webm-muxer.js", "lib/webm-muxer.js")
page = page.replace("https://cdn.jsdelivr.net/npm/gifenc@1.0.3/dist/gifenc.esm.js", "/lib/gifenc.esm.js")
page = page.replace("body { margin: 0;", "body { margin: 0; -webkit-user-select: none; user-select: none; -webkit-tap-highlight-color: transparent;")
head = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">'
        '<style>body{padding-inline:6px!important}</style></head><body>')
out = "../../frames/src/main/assets/www"
os.makedirs(out, exist_ok=True)
open(f"{out}/index.html", "w").write(head + page + "</body></html>")
print("android asset written")
PY
