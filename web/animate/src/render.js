// ============================================================================
// render.js — geometry for vector objects, tweening, and frame compositing
// ============================================================================
const Render = (App.Render = {});
const imgCache = new Map();
function imageFor(src) {
  let im = imgCache.get(src);
  if (!im) { im = new Image(); im.onload = () => changed("image"); im.src = src; imgCache.set(src, im); }
  return im;
}

// ---------------------------------------------------------------- local geometry
Render.localPath = function (o, p = new Path2D()) {
  switch (o.type) {
    case "rect": {
      const r = Math.min(o.r || 0, Math.abs(o.w) / 2, Math.abs(o.h) / 2);
      if (r > 0 && p.roundRect) p.roundRect(0, 0, o.w, o.h, r); else p.rect(0, 0, o.w, o.h);
      break;
    }
    case "ellipse": p.ellipse(o.w / 2, o.h / 2, Math.abs(o.w / 2), Math.abs(o.h / 2), 0, 0, Math.PI * 2); break;
    case "poly": {
      const n = o.star ? o.n * 2 : o.n;
      for (let i = 0; i < n; i++) {
        const r = o.star && i % 2 ? o.inner : 1, a = (Math.PI * 2 * i) / n - Math.PI / 2;
        const x = o.w / 2 + Math.cos(a) * r * o.w / 2, y = o.h / 2 + Math.sin(a) * r * o.h / 2;
        i ? p.lineTo(x, y) : p.moveTo(x, y);
      }
      p.closePath(); break;
    }
    case "path": {
      // a node with start: true begins a new sub-path (compound paths, e.g. a shape with holes)
      const N = o.nodes; if (!N.length) break;
      const seg = (a, b) => (a.ox === a.x && a.oy === a.y && b.ix === b.x && b.iy === b.y) ? p.lineTo(b.x, b.y) : p.bezierCurveTo(a.ox, a.oy, b.ix, b.iy, b.x, b.y);
      const closeSub = (a, b) => { if (o.closed && b > a) { seg(N[b], N[a]); p.closePath(); } };
      let s0 = 0;
      p.moveTo(N[0].x, N[0].y);
      for (let i = 1; i < N.length; i++) {
        if (N[i].start) { closeSub(s0, i - 1); s0 = i; p.moveTo(N[i].x, N[i].y); }
        else seg(N[i - 1], N[i]);
      }
      closeSub(s0, N.length - 1);
      break;
    }
    case "brush": brushOutline(o, p); break;
    case "rings":
      // polygons left after partial erasing: [[outer, hole, …], …], each ring a flat x,y list
      for (const poly of o.polys) for (const r of poly) {
        if (r.length < 6) continue;
        p.moveTo(r[0], r[1]);
        for (let i = 2; i < r.length; i += 2) p.lineTo(r[i], r[i + 1]);
        p.closePath();
      }
      break;
    case "image": p.rect(0, 0, o.w, o.h); break;
    case "text": { const b = textBox(o); p.rect(b.x, b.y, b.w, b.h); break; }
  }
  return p;
};

/** Variable-width outline of a pressure brush stroke (pts = x,y,p triplets). */
function brushOutline(o, p) {
  const pts = o.pts, n = pts.length / 3;
  if (!n) return;
  const w = (i) => {
    let r = (o.size * ((1 - o.pressure) + o.pressure * pts[i * 3 + 2])) / 2;
    if (o.taper && n > 6) { const k = 4; if (i < k) r *= 0.35 + (0.65 * i) / k; if (i > n - 1 - k) r *= 0.35 + (0.65 * (n - 1 - i)) / k; }
    return Math.max(0.3, r);
  };
  if (n === 1) { p.arc(pts[0], pts[1], w(0), 0, Math.PI * 2); return; }
  const lx = [], ly = [], rx = [], ry = [];
  for (let i = 0; i < n; i++) {
    const a = Math.max(0, i - 1), b = Math.min(n - 1, i + 1);
    let tx = pts[b * 3] - pts[a * 3], ty = pts[b * 3 + 1] - pts[a * 3 + 1];
    const len = Math.hypot(tx, ty) || 1; tx /= len; ty /= len;
    const r = w(i);
    lx[i] = pts[i * 3] - ty * r; ly[i] = pts[i * 3 + 1] + tx * r;
    rx[i] = pts[i * 3] + ty * r; ry[i] = pts[i * 3 + 1] - tx * r;
  }
  p.moveTo(lx[0], ly[0]);
  for (let i = 1; i < n - 1; i++) p.quadraticCurveTo(lx[i], ly[i], (lx[i] + lx[i + 1]) / 2, (ly[i] + ly[i + 1]) / 2);
  p.lineTo(lx[n - 1], ly[n - 1]);
  const e = n - 1, ae = Math.atan2(ly[e] - pts[e * 3 + 1], lx[e] - pts[e * 3]);
  p.arc(pts[e * 3], pts[e * 3 + 1], w(e), ae, ae - Math.PI, true);
  for (let i = n - 2; i >= 1; i--) p.quadraticCurveTo(rx[i], ry[i], (rx[i] + rx[i - 1]) / 2, (ry[i] + ry[i - 1]) / 2);
  p.lineTo(rx[0], ry[0]);
  const a0 = Math.atan2(ry[0] - pts[1], rx[0] - pts[0]);
  p.arc(pts[0], pts[1], w(0), a0, a0 - Math.PI, true);
  p.closePath();
}

const measureCtx = document.createElement("canvas").getContext("2d");
function textFont(o) { return `${o.italic ? "italic " : ""}${o.weight || 400} ${o.size}px "${o.font}", sans-serif`; }
function textBox(o) {
  measureCtx.font = textFont(o);
  const lines = String(o.text).split("\n");
  const w = Math.max(1, ...lines.map((l) => measureCtx.measureText(l).width));
  const lh = o.size * 1.15;
  const x = o.align === "center" ? -w / 2 : o.align === "right" ? -w : 0;
  return { x, y: 0, w, h: lh * lines.length, lines, lh };
}
Render.textBox = textBox;

/** Local-space bounds (before the object matrix). */
Render.localBounds = function (o) {
  switch (o.type) {
    case "rect": case "ellipse": case "poly": case "image":
      return { x: Math.min(0, o.w), y: Math.min(0, o.h), w: Math.abs(o.w), h: Math.abs(o.h) };
    case "text": return textBox(o);
    case "path": {
      let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
      for (const n of o.nodes) for (const [x, y] of [[n.x, n.y], [n.ix, n.iy], [n.ox, n.oy]]) { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
      return { x: x0, y: y0, w: Math.max(1, x1 - x0), h: Math.max(1, y1 - y0) };
    }
    case "rings": {
      let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
      for (const poly of o.polys) for (const r of poly) for (let i = 0; i < r.length; i += 2) { x0 = Math.min(x0, r[i]); y0 = Math.min(y0, r[i + 1]); x1 = Math.max(x1, r[i]); y1 = Math.max(y1, r[i + 1]); }
      if (x0 === Infinity) return { x: 0, y: 0, w: 1, h: 1 };
      return { x: x0, y: y0, w: Math.max(1, x1 - x0), h: Math.max(1, y1 - y0) };
    }
    case "brush": {
      let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
      for (let i = 0; i < o.pts.length; i += 3) { x0 = Math.min(x0, o.pts[i]); y0 = Math.min(y0, o.pts[i + 1]); x1 = Math.max(x1, o.pts[i]); y1 = Math.max(y1, o.pts[i + 1]); }
      const r = o.size / 2; return { x: x0 - r, y: y0 - r, w: x1 - x0 + 2 * r, h: y1 - y0 + 2 * r };
    }
  }
  return { x: 0, y: 0, w: 1, h: 1 };
};

/** Axis-aligned bounds in document space. */
Render.docBounds = function (o) {
  const b = Render.localBounds(o);
  const pts = [[b.x, b.y], [b.x + b.w, b.y], [b.x + b.w, b.y + b.h], [b.x, b.y + b.h]].map(([x, y]) => M.apply(o.m, x, y));
  const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
  const pad = o.stroke ? (o.sw || 0) * M.scaleFactor(o.m) / 2 : 0;
  return { x: Math.min(...xs) - pad, y: Math.min(...ys) - pad, w: Math.max(...xs) - Math.min(...xs) + 2 * pad, h: Math.max(...ys) - Math.min(...ys) + 2 * pad };
};

function fillStyleFor(ctx, o) {
  const f = o.fill;
  if (!f) return null;
  if (typeof f === "string") return f;
  const b = Render.localBounds(o);
  const cx = b.x + b.w / 2, cy = b.y + b.h / 2;
  let g;
  if (f.type === "radial") g = ctx.createRadialGradient(cx, cy, 0, cx, cy, Math.max(b.w, b.h) / 2);
  else {
    const a = (f.angle || 0) * Math.PI / 180;
    const half = (Math.abs(b.w * Math.cos(a)) + Math.abs(b.h * Math.sin(a))) / 2;
    g = ctx.createLinearGradient(cx - Math.cos(a) * half, cy - Math.sin(a) * half, cx + Math.cos(a) * half, cy + Math.sin(a) * half);
  }
  g.addColorStop(0, f.c1); g.addColorStop(1, f.c2);
  return g;
}

Render.drawObj = function (ctx, o) {
  ctx.save();
  ctx.transform(o.m[0], o.m[1], o.m[2], o.m[3], o.m[4], o.m[5]);
  ctx.globalAlpha *= o.opacity ?? 1;
  if (o.blend && o.blend !== "source-over") ctx.globalCompositeOperation = o.blend;
  if (o.type === "image") {
    const im = imageFor(o.src);
    if (im.complete && im.naturalWidth) ctx.drawImage(im, 0, 0, o.w, o.h);
  } else if (o.type === "text") {
    const b = textBox(o);
    ctx.font = textFont(o); ctx.textBaseline = "top"; ctx.textAlign = o.align || "left";
    const fs = fillStyleFor(ctx, o);
    b.lines.forEach((line, i) => {
      if (o.stroke && o.sw) { ctx.lineJoin = "round"; ctx.lineWidth = o.sw; ctx.strokeStyle = o.stroke; ctx.strokeText(line, 0, i * b.lh); }
      if (fs) { ctx.fillStyle = fs; ctx.fillText(line, 0, i * b.lh); }
    });
  } else {
    const p = Render.localPath(o);
    const fs = fillStyleFor(ctx, o);
    if (fs) { ctx.fillStyle = fs; ctx.fill(p, Render.fillRule(o)); }
    if (o.stroke && o.sw > 0 && o.type !== "brush") {
      ctx.lineWidth = o.sw; ctx.strokeStyle = o.stroke;
      ctx.lineCap = o.cap || "round"; ctx.lineJoin = o.join || "round";
      ctx.stroke(p);
    }
  }
  ctx.restore();
};

Render.isCompound = (o) => o.type === "path" && o.nodes.some((n, i) => i > 0 && n.start);
Render.fillRule = (o) => (o.type === "rings" || Render.isCompound(o) ? "evenodd" : "nonzero");

const hitCtx = document.createElement("canvas").getContext("2d");
Render.hit = function (o, x, y, tol) {
  const b = Render.docBounds(o);
  if (x < b.x - tol || y < b.y - tol || x > b.x + b.w + tol || y > b.y + b.h + tol) return false;
  const inv = M.inv(o.m); const [lx, ly] = M.apply(inv, x, y);
  if (o.type === "text" || o.type === "image") { const lb = Render.localBounds(o); return lx >= lb.x && ly >= lb.y && lx <= lb.x + lb.w && ly <= lb.y + lb.h; }
  const p = Render.localPath(o);
  const s = M.scaleFactor(o.m);
  if (o.fill && hitCtx.isPointInPath(p, lx, ly, Render.fillRule(o))) return true;
  hitCtx.lineWidth = ((o.stroke ? o.sw : 0) + (2 * tol) / s);
  hitCtx.lineCap = "round"; hitCtx.lineJoin = "round";
  return hitCtx.isPointInStroke(p, lx, ly);
};

// ---------------------------------------------------------------- tweening
const EASE = {
  linear: (t) => t,
  easeIn: (t) => t * t * t,
  easeOut: (t) => 1 - Math.pow(1 - t, 3),
  easeInOut: (t) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2)
};
Render.EASE = EASE;

function lerpMatrix(a, b, t) {
  const A = M.decompose(a), B = M.decompose(b);
  let dr = B.rot - A.rot; dr -= Math.PI * 2 * Math.round(dr / (Math.PI * 2));
  return M.compose({ tx: lerp(A.tx, B.tx, t), ty: lerp(A.ty, B.ty, t), rot: A.rot + dr * t, sx: lerp(A.sx, B.sx, t), sy: lerp(A.sy, B.sy, t), skew: lerp(A.skew, B.skew, t) });
}
function lerpFill(a, b, t) {
  if (typeof a === "string" && typeof b === "string") return Color.mix(a, b, t);
  if (a && b && typeof a === "object" && typeof b === "object") return { ...a, c1: Color.mix(a.c1, b.c1, t), c2: Color.mix(a.c2, b.c2, t), angle: lerp(a.angle || 0, b.angle || 0, t) };
  return t < 0.5 ? a : b;
}
/** Interpolates two versions of the same object (same id). Shape tween when the geometry matches. */
function lerpObj(a, b, t) {
  const o = cloneObj(a);
  o.m = lerpMatrix(a.m, b.m, t);
  o.opacity = lerp(a.opacity ?? 1, b.opacity ?? 1, t);
  o.fill = lerpFill(a.fill, b.fill, t);
  if (a.stroke && b.stroke) o.stroke = Color.mix(a.stroke, b.stroke, t);
  o.sw = lerp(a.sw || 0, b.sw || 0, t);
  if (a.type === b.type) {
    if (["rect", "ellipse", "poly", "image"].includes(a.type)) { o.w = lerp(a.w, b.w, t); o.h = lerp(a.h, b.h, t); if (a.type === "rect") o.r = lerp(a.r || 0, b.r || 0, t); if (a.type === "poly") o.inner = lerp(a.inner, b.inner, t); }
    if (a.type === "text") o.size = lerp(a.size, b.size, t);
    if (a.type === "path" && a.nodes.length === b.nodes.length) {
      o.nodes = a.nodes.map((n, i) => { const m = b.nodes[i]; return { x: lerp(n.x, m.x, t), y: lerp(n.y, m.y, t), ix: lerp(n.ix, m.ix, t), iy: lerp(n.iy, m.iy, t), ox: lerp(n.ox, m.ox, t), oy: lerp(n.oy, m.oy, t), smooth: n.smooth, start: n.start }; });
    }
    if (a.type === "brush" && a.pts.length === b.pts.length) o.pts = a.pts.map((v, i) => lerp(v, b.pts[i], t));
    if (a.type === "rings" && a.polys.length === b.polys.length && a.polys.every((P, i) => P.length === b.polys[i].length && P.every((r, j) => r.length === b.polys[i][j].length)))
      o.polys = a.polys.map((P, i) => P.map((r, j) => r.map((v, k) => lerp(v, b.polys[i][j][k], t))));
  }
  return o;
}

/** The objects a draw layer shows at frame f (handles holds and tweens). */
Render.cellObjectsAt = function (L, f) {
  const k = keyIndexAt(L, f);
  if (k < 0) return [];
  const cell = L.cells[k];
  if (k === f || !cell.tween) return cell.objects;
  const k2 = nextKeyIndex(L, k);
  if (k2 < 0) return cell.objects;
  const t = (EASE[cell.tween] || EASE.linear)((f - k) / (k2 - k));
  const next = new Map(L.cells[k2].objects.map((o) => [o.id, o]));
  return cell.objects.map((o) => (next.has(o.id) ? lerpObj(o, next.get(o.id), t) : o));
};
Render.cellRasterAt = function (L, f) { const k = keyIndexAt(L, f); return k >= 0 ? L.cells[k].raster : null; };

// ---------------------------------------------------------------- compositing
const layerBuf = document.createElement("canvas");
function drawDrawLayer(ctx, L, f) {
  const raster = Render.cellRasterAt(L, f);
  if (raster) ctx.drawImage(raster, 0, 0);
  for (const o of Render.cellObjectsAt(L, f)) Render.drawObj(ctx, o);
}

/**
 * Draws frame f of the document into ctx (document coordinates).
 * opts: { background: bool, skipLayer: layerIndex, only: layerIndex, mediaTime: fn }
 */
Render.frame = function (ctx, f, opts = {}) {
  const d = App.doc;
  if (opts.background !== false && !d.transparent) { ctx.fillStyle = d.background; ctx.fillRect(0, 0, d.width, d.height); }
  d.layers.forEach((L, i) => {
    if (!L.visible || i === opts.skipLayer || (opts.only !== undefined && i !== opts.only)) return;
    const simple = L.opacity >= 1 && (L.blend || "source-over") === "source-over";
    let tctx = ctx;
    if (!simple) {
      if (layerBuf.width !== d.width || layerBuf.height !== d.height) { layerBuf.width = d.width; layerBuf.height = d.height; }
      tctx = layerBuf.getContext("2d");
      tctx.setTransform(1, 0, 0, 1, 0, 0); tctx.globalAlpha = 1; tctx.globalCompositeOperation = "source-over";
      tctx.clearRect(0, 0, d.width, d.height);
    }
    if (L.kind === "draw") drawDrawLayer(tctx, L, f);
    else if (L.kind === "media") Media.draw(tctx, L, f);
    if (!simple) {
      ctx.save(); ctx.globalAlpha = L.opacity; ctx.globalCompositeOperation = L.blend || "source-over";
      ctx.drawImage(layerBuf, 0, 0); ctx.restore();
    }
  });
};

/** Onion skin: neighbouring frames of the active layer tinted red (before) / green (after). */
const onionBuf = document.createElement("canvas");
Render.onion = function (ctx, f) {
  const d = App.doc, o = d.onion, L = activeLayer();
  if (!o.on || !L || L.kind !== "draw") return;
  if (onionBuf.width !== d.width || onionBuf.height !== d.height) { onionBuf.width = d.width; onionBuf.height = d.height; }
  const octx = onionBuf.getContext("2d");
  const draw = (frame, color, alpha) => {
    if (frame < 0 || frame >= d.frames) { if (!d.loop) return; frame = (frame + d.frames) % d.frames; }
    if (keyIndexAt(L, frame) === keyIndexAt(L, f) && !L.cells[keyIndexAt(L, f)]?.tween) return;
    octx.setTransform(1, 0, 0, 1, 0, 0); octx.globalCompositeOperation = "source-over"; octx.globalAlpha = 1;
    octx.clearRect(0, 0, d.width, d.height);
    drawDrawLayer(octx, L, frame);
    octx.globalCompositeOperation = "source-in"; octx.fillStyle = color; octx.fillRect(0, 0, d.width, d.height);
    ctx.save(); ctx.globalAlpha = alpha; ctx.drawImage(onionBuf, 0, 0); ctx.restore();
  };
  for (let i = o.before; i >= 1; i--) draw(f - i, "#e53935", o.opacity * (1 - (i - 1) / (o.before + 1)));
  for (let i = o.after; i >= 1; i--) draw(f + i, "#16a34a", o.opacity * (1 - (i - 1) / (o.after + 1)));
};
