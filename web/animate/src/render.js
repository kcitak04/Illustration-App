// ============================================================================
// render.js — geometry for vector objects, tweening, and frame compositing
// ============================================================================
const Render = (App.Render = {});
const imgCache = new Map();
/** Lets a freshly made image object draw immediately (before its data URL has loaded). */
Render.cacheImage = (src, canvas) => imgCache.set(src, canvas);
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

/** Vector brush styles (the brush object's `style`). */
Render.VBRUSHES = [
  ["pressure", "Pressure"], ["uniform", "Monoline"], ["taper", "Ink taper"], ["calligraphy", "Calligraphy"], ["rough", "Rough edge"],
  ["bristle", "Dry bristle"], ["dotted", "Dotted"], ["dashed", "Dashed"], ["spray", "Spray"], ["splatter", "Splatter"],
  ["wavy", "Wavy"], ["zigzag", "Zigzag"], ["outline", "Hollow outline"], ["beads", "Beads"], ["chain", "Chain rings"], ["hatch", "Hatch marks"]
];
/**
 * Extra settings each vector style understands (stored on the stroke):
 * sp = spacing, amt = the style's main amount, ang = angle in degrees, freq = wavelength.
 */
Render.VBRUSH_PARAMS = {
  taper: { amt: "Taper length" }, calligraphy: { amt: "Nib contrast", ang: "Nib angle" }, rough: { amt: "Roughness" },
  bristle: { amt: "Dryness", sp: "Bristles" }, dotted: { sp: "Spacing" }, dashed: { sp: "Dash length", amt: "Gap" },
  spray: { amt: "Spread", sp: "Density" }, splatter: { amt: "Splatter" }, wavy: { amt: "Wave height", freq: "Wavelength" },
  zigzag: { amt: "Zigzag height", freq: "Wavelength" }, outline: { amt: "Line thickness" }, beads: { sp: "Spacing", amt: "Size contrast" },
  chain: { sp: "Spacing", amt: "Ring thickness" }, hatch: { sp: "Spacing", ang: "Hatch angle", amt: "Mark length" }
};
/** Repeatable pseudo-random number in 0..1 (brush textures must look the same on every redraw). */
function hash(n) { n = Math.sin(n * 127.1 + 311.7) * 43758.5453; return n - Math.floor(n); }

/** A filled ribbon through points a..b of xs/ys with half-widths rs, round at both ends. */
function ribbon(p, xs, ys, rs, a, b) {
  const n = b - a + 1;
  if (n < 1) return;
  if (n === 1) { p.moveTo(xs[a] + rs[a], ys[a]); p.arc(xs[a], ys[a], rs[a], 0, Math.PI * 2); return; }
  const lx = [], ly = [], rx = [], ry = [];
  for (let k = 0; k < n; k++) {
    const i = a + k, i0 = Math.max(a, i - 1), i1 = Math.min(b, i + 1);
    let tx = xs[i1] - xs[i0], ty = ys[i1] - ys[i0];
    const len = Math.hypot(tx, ty) || 1; tx /= len; ty /= len;
    const r = rs[i];
    lx[k] = xs[i] - ty * r; ly[k] = ys[i] + tx * r;
    rx[k] = xs[i] + ty * r; ry[k] = ys[i] - tx * r;
  }
  const e = n - 1;
  p.moveTo(lx[0], ly[0]);
  for (let k = 1; k < n - 1; k++) p.quadraticCurveTo(lx[k], ly[k], (lx[k] + lx[k + 1]) / 2, (ly[k] + ly[k + 1]) / 2);
  p.lineTo(lx[e], ly[e]);
  const ae = Math.atan2(ly[e] - ys[b], lx[e] - xs[b]);
  p.arc(xs[b], ys[b], rs[b], ae, ae - Math.PI, true);
  for (let k = n - 2; k >= 1; k--) p.quadraticCurveTo(rx[k], ry[k], (rx[k] + rx[k - 1]) / 2, (ry[k] + ry[k - 1]) / 2);
  p.lineTo(rx[0], ry[0]);
  const a0 = Math.atan2(ry[0] - ys[a], rx[0] - xs[a]);
  p.arc(xs[a], ys[a], rs[a], a0, a0 - Math.PI, true);
  p.closePath();
}
const dot = (p, x, y, r) => { p.moveTo(x + r, y); p.arc(x, y, r, 0, Math.PI * 2); };

/** Outline of a vector brush stroke (pts = x,y,pressure triplets) in the stroke's style. */
function brushOutline(o, p) {
  const pts = o.pts, n = pts.length / 3;
  if (!n) return;
  const st = o.style || "pressure", seed = o.seed || 1, size = o.size;
  const sp = o.sp ?? 1, amt = o.amt ?? 0.5, ang = ((o.ang ?? 45) * Math.PI) / 180, freq = o.freq ?? 1;
  const xs = new Array(n), ys = new Array(n), rs = new Array(n);
  for (let i = 0; i < n; i++) { xs[i] = pts[i * 3]; ys[i] = pts[i * 3 + 1]; }
  const tapered = (o.taper || st === "taper") && n > 6;
  const tk = st === "taper" ? Math.max(3, Math.round(n * (0.08 + 0.5 * amt))) : 4, tmin = st === "taper" ? 0.06 : 0.35;
  const tan = (X, Y, i, N) => { const i0 = Math.max(0, i - 1), i1 = Math.min(N - 1, i + 1); let tx = X[i1] - X[i0], ty = Y[i1] - Y[i0]; const l = Math.hypot(tx, ty) || 1; return [tx / l, ty / l]; };
  for (let i = 0; i < n; i++) {
    let r = st === "uniform" || st === "outline" || st === "wavy" || st === "zigzag" ? size / 2 : (size * ((1 - o.pressure) + o.pressure * pts[i * 3 + 2])) / 2;
    if (tapered && st !== "uniform") { if (i < tk) r *= tmin + ((1 - tmin) * i) / tk; if (i > n - 1 - tk) r *= tmin + ((1 - tmin) * (n - 1 - i)) / tk; }
    if (st === "calligraphy") {
      // a flat nib held at an angle: thick across it, thin along it
      const [tx, ty] = tan(xs, ys, i, n), c = 0.3 + 0.7 * amt;
      r *= 1 - c + c * Math.abs(tx * Math.sin(ang) - ty * Math.cos(ang));
    }
    if (st === "rough") r *= 1 - amt * 0.8 + amt * 1.6 * hash(seed + i * 1.37);
    rs[i] = Math.max(0.3, r);
  }
  // running length along the stroke
  const L = new Float64Array(n);
  for (let i = 1; i < n; i++) L[i] = L[i - 1] + Math.hypot(xs[i] - xs[i - 1], ys[i] - ys[i - 1]);
  const total = L[n - 1];
  const at = (d) => { // point, half-width and direction at distance d
    let i = 1; while (i < n - 1 && L[i] < d) i++;
    if (n === 1) return [xs[0], ys[0], rs[0], 1, 0];
    const seg = L[i] - L[i - 1] || 1, t = Math.min(1, Math.max(0, (d - L[i - 1]) / seg));
    const dx = xs[i] - xs[i - 1], dy = ys[i] - ys[i - 1], l = Math.hypot(dx, dy) || 1;
    return [xs[i - 1] + dx * t, ys[i - 1] + dy * t, rs[i - 1] + (rs[i] - rs[i - 1]) * t, dx / l, dy / l];
  };
  switch (st) {
    case "dotted": {
      const gap = Math.max(1, size * 1.6 * sp);
      for (let d = 0; d <= total + 1e-6; d += gap) { const [x, y, r] = at(d); dot(p, x, y, r * 0.8); }
      break;
    }
    case "beads": {
      const gap = Math.max(1, size * 1.1 * sp); let k = 0;
      for (let d = 0; d <= total + 1e-6; d += gap, k++) { const [x, y, r] = at(d); dot(p, x, y, r * (k % 2 ? 1 - 0.8 * amt : 0.95)); }
      break;
    }
    case "chain": {
      // hollow rings (the stroke is filled even-odd, so the inner circle cuts the hole)
      const gap = Math.max(1, size * 1.15 * sp);
      for (let d = 0; d <= total + 1e-6; d += gap) { const [x, y, r] = at(d); dot(p, x, y, r * 0.95); dot(p, x, y, Math.max(0.2, r * 0.95 * (0.85 - 0.7 * amt))); }
      break;
    }
    case "hatch": {
      const gap = Math.max(1, size * 0.55 * sp), len = size * (0.4 + 1.6 * amt), ca = Math.cos(ang), sa = Math.sin(ang);
      for (let d = 0; d <= total + 1e-6; d += gap) {
        const [x, y, r] = at(d), w = Math.max(0.35, r * 0.22), hl = len * (0.5 + r / size) / 2;
        const ax = ca * hl, ay = sa * hl, nx = -sa * w, ny = ca * w;
        p.moveTo(x - ax + nx, y - ay + ny); p.lineTo(x + ax + nx, y + ay + ny); p.lineTo(x + ax - nx, y + ay - ny); p.lineTo(x - ax - nx, y - ay - ny); p.closePath();
      }
      break;
    }
    case "dashed": {
      const dash = Math.max(2, size * 2.4 * sp), period = dash + Math.max(1, size * (0.3 + 2.2 * amt));
      let a = -1;
      for (let i = 0; i <= n; i++) {
        const on = i < n && L[i] % period < dash;
        if (on && a < 0) a = i;
        if (!on && a >= 0) { ribbon(p, xs, ys, rs, a, i - 1); a = -1; }
      }
      break;
    }
    case "spray": {
      const count = Math.max(1, Math.min(40, Math.round((size / 3) * sp))), spread = 0.6 + amt * 2.4;
      for (let i = 0; i < n; i++) for (let k = 0; k < count; k++) {
        const h = seed * 13 + i * 31 + k * 7, a = hash(h) * Math.PI * 2, dist = Math.sqrt(hash(h + 1)) * rs[i] * spread;
        dot(p, xs[i] + Math.cos(a) * dist, ys[i] + Math.sin(a) * dist, Math.max(0.5, size * 0.06 * (0.5 + hash(h + 2))));
      }
      break;
    }
    case "splatter": {
      for (let i = 0; i < n; i++) rs[i] *= 0.7;
      ribbon(p, xs, ys, rs, 0, n - 1);
      for (let i = 0; i < n; i++) {
        const h = seed * 17 + i * 23;
        if (hash(h) > amt * 0.8) continue;
        const a = hash(h + 1) * Math.PI * 2, dist = size * (0.6 + hash(h + 2) * 1.3);
        dot(p, xs[i] + Math.cos(a) * dist, ys[i] + Math.sin(a) * dist, Math.max(0.5, size * (0.05 + 0.14 * hash(h + 3))));
      }
      break;
    }
    case "bristle": {
      const K = Math.max(3, Math.min(16, Math.round(7 * sp)));
      for (let k = 0; k < K; k++) {
        const off = (k / (K - 1) - 0.5) * 1.8, thick = (14 / K) * (0.07 + 0.13 * hash(seed + k * 5.3));
        const bx = new Array(n), by = new Array(n), br = new Array(n);
        for (let i = 0; i < n; i++) { const [tx, ty] = tan(xs, ys, i, n); bx[i] = xs[i] - ty * off * rs[i]; by[i] = ys[i] + tx * off * rs[i]; br[i] = Math.max(0.3, rs[i] * thick); }
        // dry-brush gaps: each bristle skips a few stretches
        let a = -1;
        for (let i = 0; i <= n; i++) {
          const on = i < n && hash(seed * 3 + k * 41 + Math.floor(i / 4)) > 0.03 + amt * 0.5;
          if (on && a < 0) a = i;
          if (!on && a >= 0) { ribbon(p, bx, by, br, a, i - 1); a = -1; }
        }
      }
      break;
    }
    case "wavy": case "zigzag": {
      // resample the stroke evenly, then push it sideways along a sine / triangle wave
      const wl = Math.max(4, size * 4 * freq), amp = size * 1.6 * amt, step = st === "zigzag" ? wl / 2 : wl / 16;
      const X = [], Y = [], R = [];
      for (let d = 0, k = 0; d <= total + 1e-6; d += step, k++) {
        const [x, y, r, tx, ty] = at(d);
        const w = st === "zigzag" ? (k % 2 ? 1 : -1) : Math.sin((d / wl) * Math.PI * 2);
        X.push(x - ty * w * amp); Y.push(y + tx * w * amp); R.push(r * 0.6);
      }
      if (st === "zigzag") { for (let k = 1; k < X.length; k++) { const mx = [X[k - 1], X[k]], my = [Y[k - 1], Y[k]]; ribbon(p, mx, my, [R[k - 1], R[k]], 0, 1); } }
      else ribbon(p, X, Y, R, 0, X.length - 1);
      break;
    }
    case "outline": {
      // two ribbons filled even-odd leave a hollow line
      ribbon(p, xs, ys, rs, 0, n - 1);
      const inner = rs.map((r) => Math.max(0.2, r * (0.85 - 0.7 * amt)));
      ribbon(p, xs, ys, inner, 0, n - 1);
      break;
    }
    default: ribbon(p, xs, ys, rs, 0, n - 1);
  }
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
      const r = o.style === "spray" ? o.size * (0.4 + 1.3 * (o.amt ?? 0.5)) : o.style === "splatter" ? o.size * 1.9 : o.style === "wavy" || o.style === "zigzag" ? o.size * (0.5 + 1.7 * (o.amt ?? 0.5)) : o.style === "hatch" ? o.size * (0.5 + 1.6 * (o.amt ?? 0.5)) : o.size / 2;
      return { x: x0 - r, y: y0 - r, w: x1 - x0 + 2 * r, h: y1 - y0 + 2 * r };
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
    if (im instanceof HTMLCanvasElement || (im.complete && im.naturalWidth)) ctx.drawImage(im, 0, 0, o.w, o.h);
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
Render.fillRule = (o) => (o.type === "rings" || Render.isCompound(o) || (o.type === "brush" && (o.style === "outline" || o.style === "chain")) ? "evenodd" : "nonzero");

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
// scratch canvases for layer groups (clipping masks) and masked layers
const bufPool = [];
function scratch(i) {
  const d = App.doc; let c = bufPool[i];
  if (!c) c = bufPool[i] = document.createElement("canvas");
  if (c.width !== d.width || c.height !== d.height) { c.width = d.width; c.height = d.height; }
  const g = c.getContext("2d"); g.setTransform(1, 0, 0, 1, 0, 0); g.globalAlpha = 1; g.globalCompositeOperation = "source-over"; g.clearRect(0, 0, c.width, c.height);
  return g;
}
const hasMask = (L) => !!(L.mask && L.mask.canvas && L.mask.on !== false);
/** Draws a layer's own content (no opacity / blend), with its layer mask applied. */
function layerContent(g, L, i, f, opts) {
  if (opts.live && i === opts.liveIndex) opts.live(g);
  else if (L.kind === "draw") drawDrawLayer(g, L, f);
  else if (L.kind === "media") Media.draw(g, L, f);
  if (hasMask(L)) { g.save(); g.setTransform(1, 0, 0, 1, 0, 0); g.globalAlpha = 1; g.globalCompositeOperation = "destination-in"; g.drawImage(L.mask.canvas, 0, 0); g.restore(); }
}
Render.layerContent = layerContent;
/** The layer a clipped layer is clipped to: the nearest unclipped layer below it. */
Render.clipBase = function (i) { const Ls = App.doc.layers; let b = i; while (b >= 0 && Ls[b].clip) b--; return b; };

/**
 * Draws frame f of the document into ctx (document coordinates).
 * opts: { background, skipLayer, only, from, to, liveIndex + live(g): draw that layer's content yourself }
 * Clipped layers (L.clip) only show where the layer below them has paint; layer masks (L.mask) hide parts of a layer.
 */
Render.frame = function (ctx, f, opts = {}) {
  const d = App.doc, Ls = d.layers;
  if (opts.background !== false && !d.transparent) { ctx.fillStyle = d.background; ctx.fillRect(0, 0, d.width, d.height); }
  const from = opts.from ?? 0, to = Math.min(Ls.length - 1, opts.to ?? Ls.length - 1);
  const shown = (L, i) => L.visible && i !== opts.skipLayer && (opts.only === undefined || i === opts.only);
  let i = from;
  while (i <= to) {
    const L = Ls[i];
    // a group: an unclipped base plus the clipped layers stacked right above it
    const base = L.clip ? Render.clipBase(i) : i;
    let j = i + (L.clip ? 0 : 1);
    while (j <= to && Ls[j].clip) j++;
    const members = []; for (let k = L.clip ? i : i + 1; k < j; k++) if (shown(Ls[k], k)) members.push(k);
    const baseShown = base >= 0 && Ls[base].visible;
    if (base === i && !members.length) {
      if (shown(L, i)) {
        const simple = L.opacity >= 1 && (L.blend || "source-over") === "source-over" && !hasMask(L);
        if (simple) layerContent(ctx, L, i, f, opts);
        else { const g = scratch(0); layerContent(g, L, i, f, opts); ctx.save(); ctx.globalAlpha = L.opacity; ctx.globalCompositeOperation = L.blend || "source-over"; ctx.drawImage(g.canvas, 0, 0); ctx.restore(); }
      }
    } else if (base < 0) {
      // clipped layers with nothing underneath to clip to: draw them as normal layers
      for (const k of members) { const M = Ls[k], g = scratch(0); layerContent(g, M, k, f, opts); ctx.save(); ctx.globalAlpha = M.opacity; ctx.globalCompositeOperation = M.blend || "source-over"; ctx.drawImage(g.canvas, 0, 0); ctx.restore(); }
    } else if (baseShown) {
      const A = scratch(0); layerContent(A, Ls[base], base, f, opts);            // the base's shape
      const G = scratch(1);
      const drawBase = base >= from && shown(Ls[base], base);
      if (drawBase) { G.globalAlpha = Ls[base].opacity; G.drawImage(A.canvas, 0, 0); G.globalAlpha = 1; }
      for (const k of members) {
        const M = Ls[k], C = scratch(2); layerContent(C, M, k, f, opts);
        C.globalCompositeOperation = "destination-in"; C.drawImage(A.canvas, 0, 0);
        G.save(); G.globalAlpha = M.opacity; G.globalCompositeOperation = M.blend || "source-over"; G.drawImage(C.canvas, 0, 0); G.restore();
      }
      ctx.save(); ctx.globalCompositeOperation = drawBase ? Ls[base].blend || "source-over" : "source-over"; ctx.drawImage(G.canvas, 0, 0); ctx.restore();
    }
    i = Math.max(j, i + 1);
  }
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
