// ============================================================================
// geometry.js — flattening vector objects to polygons and cutting them with the
// eraser (uses the polygon-clipping library for shape differences)
// ============================================================================
const Geo = (App.Geo = {});

/** Records the Path2D calls Render.localPath makes and turns curves into short line runs. */
class PolyRecorder {
  constructor(k) { this.subs = []; this.cur = null; this.k = k || 1; }
  steps(len, min = 2) { return clamp(Math.ceil((len * this.k) / 2.5), min, 96); }
  last() { const p = this.cur.pts; return p[p.length - 1]; }
  moveTo(x, y) { this.cur = { pts: [[x, y]], closed: false }; this.subs.push(this.cur); }
  lineTo(x, y) { if (!this.cur) this.moveTo(x, y); else this.cur.pts.push([x, y]); }
  quadraticCurveTo(a, b, x, y) {
    if (!this.cur) this.moveTo(a, b);
    const [x0, y0] = this.last(), n = this.steps(Math.hypot(a - x0, b - y0) + Math.hypot(x - a, y - b));
    for (let i = 1; i <= n; i++) { const t = i / n, u = 1 - t; this.cur.pts.push([u * u * x0 + 2 * u * t * a + t * t * x, u * u * y0 + 2 * u * t * b + t * t * y]); }
  }
  bezierCurveTo(a, b, c, d, x, y) {
    if (!this.cur) this.moveTo(a, b);
    const [x0, y0] = this.last(), n = this.steps(Math.hypot(a - x0, b - y0) + Math.hypot(c - a, d - b) + Math.hypot(x - c, y - d));
    for (let i = 1; i <= n; i++) {
      const t = i / n, u = 1 - t, A = u * u * u, B = 3 * u * u * t, C = 3 * u * t * t, D = t * t * t;
      this.cur.pts.push([A * x0 + B * a + C * c + D * x, A * y0 + B * b + C * d + D * y]);
    }
  }
  closePath() { if (this.cur) { this.cur.closed = true; this.cur = null; } }
  rect(x, y, w, h) { this.moveTo(x, y); this.lineTo(x + w, y); this.lineTo(x + w, y + h); this.lineTo(x, y + h); this.closePath(); }
  roundRect(x, y, w, h, r) {
    r = Math.min(Math.abs(+r || 0), Math.abs(w) / 2, Math.abs(h) / 2);
    const P = Math.PI;
    this.moveTo(x + r, y); this.lineTo(x + w - r, y); this.arc(x + w - r, y + r, r, -P / 2, 0);
    this.lineTo(x + w, y + h - r); this.arc(x + w - r, y + h - r, r, 0, P / 2);
    this.lineTo(x + r, y + h); this.arc(x + r, y + h - r, r, P / 2, P);
    this.lineTo(x, y + r); this.arc(x + r, y + r, r, P, P * 1.5); this.closePath();
  }
  ellipse(cx, cy, rx, ry) {
    const n = this.steps(Math.PI * 2 * Math.max(rx, ry), 24);
    this.moveTo(cx + rx, cy);
    for (let i = 1; i < n; i++) { const a = (Math.PI * 2 * i) / n; this.cur.pts.push([cx + Math.cos(a) * rx, cy + Math.sin(a) * ry]); }
    this.closePath();
  }
  arc(cx, cy, r, a0, a1, ccw) {
    const T = Math.PI * 2;
    let sweep = ccw ? a0 - a1 : a1 - a0;
    sweep = sweep >= T ? T : ((sweep % T) + T) % T;
    if (ccw) sweep = -sweep;
    const sx = cx + r * Math.cos(a0), sy = cy + r * Math.sin(a0);
    this.lineTo(sx, sy);
    const n = this.steps(Math.abs(sweep) * r, 4);
    for (let i = 1; i <= n; i++) { const a = a0 + (sweep * i) / n; this.cur.pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]); }
  }
}

/** Subpaths of an object in document coordinates: [{ pts: [[x, y], …], closed }]. */
Geo.flatten = function (o) {
  const rec = new PolyRecorder(M.scaleFactor(o.m));
  Render.localPath(o, rec);
  for (const s of rec.subs) s.pts = s.pts.map(([x, y]) => M.apply(o.m, x, y));
  return rec.subs.filter((s) => s.pts.length > 1);
};

/** The filled area of an object as a polygon-clipping MultiPolygon (document coordinates). */
function fillGeom(o) {
  if (o.type === "rings") {
    return o.polys.map((P) => P.map((r) => { const ring = []; for (let i = 0; i < r.length; i += 2) ring.push(M.apply(o.m, r[i], r[i + 1])); return ring; }));
  }
  const polys = Geo.flatten(o).filter((s) => s.pts.length >= 3).map((s) => [s.pts]);
  if (!polys.length) return [];
  if (Render.fillRule(o) === "evenodd") return polys.length === 1 ? polygonClipping.union(polys[0]) : polygonClipping.xor(...polys);
  return polys.length === 1 ? polygonClipping.union(polys[0]) : polygonClipping.union(...polys);
}

// ---------------------------------------------------------------- the eraser's footprint
function circlePts(x, y, r, n = 20) { const out = []; for (let i = 0; i < n; i++) { const a = (Math.PI * 2 * i) / n; out.push([x + Math.cos(a) * r, y + Math.sin(a) * r]); } return out; }
function hull(pts) {
  pts = pts.slice().sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const cross = (o, a, b) => (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  const lo = [], up = [];
  for (const p of pts) { while (lo.length >= 2 && cross(lo[lo.length - 2], lo[lo.length - 1], p) <= 0) lo.pop(); lo.push(p); }
  for (let i = pts.length - 1; i >= 0; i--) { const p = pts[i]; while (up.length >= 2 && cross(up[up.length - 2], up[up.length - 1], p) <= 0) up.pop(); up.push(p); }
  up.pop(); lo.pop();
  return lo.concat(up);
}
/** Turns eraser trails ([[x, y, r], …] per trail) into one MultiPolygon. */
Geo.eraserRegion = function (trails) {
  const pieces = [];
  for (const trail of trails) {
    if (!trail.length) continue;
    // thin the dabs: consecutive kept points overlap by at least half a radius
    const kept = [trail[0]];
    for (let i = 1; i < trail.length; i++) {
      const a = kept[kept.length - 1], b = trail[i];
      if (Math.hypot(b[0] - a[0], b[1] - a[1]) >= Math.max(0.5, Math.min(a[2], b[2]) * 0.6) || i === trail.length - 1) kept.push(b);
    }
    if (kept.length === 1) { const [x, y, r] = kept[0]; pieces.push([circlePts(x, y, r)]); continue; }
    for (let i = 1; i < kept.length; i++) {
      const a = kept[i - 1], b = kept[i];
      pieces.push([hull(circlePts(a[0], a[1], a[2]).concat(circlePts(b[0], b[1], b[2])))]);
    }
  }
  if (!pieces.length) return [];
  // union in batches keeps each sweep small
  let acc = [];
  for (let i = 0; i < pieces.length; i += 60) acc = polygonClipping.union(acc, ...pieces.slice(i, i + 60));
  return acc;
};

function regionBounds(region) {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const P of region) for (const [x, y] of P[0]) { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x); y1 = Math.max(y1, y); }
  return { x0, y0, x1, y1 };
}
Geo.inside = (region, x, y) => insideRegion(region, x, y);
function insideRegion(region, x, y) {
  let c = false;
  for (const P of region) for (const r of P) {
    for (let i = 0, j = r.length - 1; i < r.length; j = i++) {
      const [xi, yi] = r[i], [xj, yj] = r[j];
      if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) c = !c;
    }
  }
  return c;
}

// ---------------------------------------------------------------- cutting
const baseProps = (o, id) => ({ id, group: o.group, m: M.id(), fill: o.fill ? cloneObj(o.fill) : null, stroke: o.stroke || null, sw: o.sw || 0, opacity: o.opacity ?? 1, blend: o.blend || "source-over", cap: o.cap, join: o.join });

/** Splits the outline of a stroke-only object into the runs the eraser did not touch. */
function cutStroke(o, region) {
  const k = M.scaleFactor(o.m), subs = Geo.flatten(o);
  let touched = false;
  const runs = [];
  for (const s of subs) {
    const src = s.closed ? s.pts.concat([s.pts[0]]) : s.pts;
    // resample so short eraser swipes across a long straight segment are noticed
    const pts = [src[0]];
    for (let i = 1; i < src.length; i++) {
      const [ax, ay] = src[i - 1], [bx, by] = src[i], n = Math.max(1, Math.ceil(Math.hypot(bx - ax, by - ay) / 1.5));
      for (let j = 1; j <= n; j++) pts.push([ax + ((bx - ax) * j) / n, ay + ((by - ay) * j) / n]);
    }
    const inside = pts.map(([x, y]) => insideRegion(region, x, y));
    if (!inside.some(Boolean)) { runs.push({ pts: s.pts, closed: s.closed }); continue; }
    touched = true;
    let local = [], cur = null;
    pts.forEach((p, i) => { if (inside[i]) { cur = null; } else { if (!cur) { cur = []; local.push(cur); } cur.push(p); } });
    // a closed outline cut once: join the piece that wraps past the start
    if (s.closed && local.length > 1 && !inside[0] && !inside[inside.length - 1]) { const first = local.shift(); local[local.length - 1] = local[local.length - 1].concat(first.slice(1)); }
    for (const r of local) {
      let len = 0; for (let i = 1; i < r.length; i++) len += Math.hypot(r[i][0] - r[i - 1][0], r[i][1] - r[i - 1][1]);
      if (r.length > 1 && len > 0.75) runs.push({ pts: rdp(r, 0.3), closed: false });
    }
  }
  if (!touched) return null;
  return runs.map((r, i) => ({
    ...baseProps(o, i === 0 ? o.id : uid()), type: "path", fill: null, sw: (o.sw || 0) * k, closed: r.closed,
    nodes: r.pts.map(([x, y]) => ({ x, y, ix: x, iy: y, ox: x, oy: y }))
  }));
}

/**
 * Cuts one object with the eraser region.
 * Returns null when the object is untouched, [] when it is erased entirely, or the pieces left.
 */
Geo.cutObject = function (o, region, rb) {
  if (o.type === "text" || o.type === "image") return null;
  const b = Render.docBounds(o);
  if (b.x > rb.x1 || b.y > rb.y1 || b.x + b.w < rb.x0 || b.y + b.h < rb.y0) return null;
  const hasStroke = !!(o.stroke && o.sw > 0 && o.type !== "brush");
  if (o.fill) {
    const geom = fillGeom(o);
    if (!geom.length) return null;
    if (!polygonClipping.intersection(geom, region).length) return null;
    const rest = polygonClipping.difference(geom, region);
    if (!rest.length) return [];
    return [{ ...baseProps(o, o.id), type: "rings", polys: rest.map((P) => P.map((r) => r.flat())), stroke: hasStroke ? o.stroke : null, sw: hasStroke ? o.sw * M.scaleFactor(o.m) : 0 }];
  }
  if (hasStroke) return cutStroke(o, region);
  return null;
};

// ---------------------------------------------------------------- back to editable paths
/** Turns closed polygon rings into path nodes: straight runs become corners, gentle bends smooth curves. */
function ringsToNodes(rings, eps) {
  const nodes = [];
  for (const flat of rings) {
    let pts = [];
    for (let i = 0; i < flat.length; i += 2) pts.push([flat[i], flat[i + 1]]);
    const a = pts[0], z = pts[pts.length - 1];
    if (pts.length > 1 && Math.hypot(a[0] - z[0], a[1] - z[1]) < 1e-6) pts.pop();
    if (pts.length < 3) continue;
    // simplify a closed ring in two halves split at the point farthest from the start
    let far = 1, fd = -1;
    pts.forEach((q, i) => { const dd = Math.hypot(q[0] - a[0], q[1] - a[1]); if (dd > fd) { fd = dd; far = i; } });
    pts = rdp(pts.slice(0, far + 1), eps).slice(0, -1).concat(rdp(pts.slice(far).concat([pts[0]]), eps).slice(0, -1));
    if (pts.length < 3) continue;
    const n = pts.length;
    pts.forEach((q, i) => {
      const prev = pts[(i - 1 + n) % n], next = pts[(i + 1) % n];
      const a1 = Math.atan2(q[1] - prev[1], q[0] - prev[0]), a2 = Math.atan2(next[1] - q[1], next[0] - q[0]);
      let turn = Math.abs(a2 - a1); if (turn > Math.PI) turn = Math.PI * 2 - turn;
      const node = { x: q[0], y: q[1], ix: q[0], iy: q[1], ox: q[0], oy: q[1], smooth: false };
      if (turn < Math.PI / 6 && n > 4) {
        const tx = (next[0] - prev[0]) / 6, ty = (next[1] - prev[1]) / 6;
        Object.assign(node, { ix: q[0] - tx, iy: q[1] - ty, ox: q[0] + tx, oy: q[1] + ty, smooth: true });
      }
      if (i === 0 && nodes.length) node.start = true;
      nodes.push(node);
    });
  }
  return nodes;
}
/** Editable path version of a merged / cut shape or a vector brush stroke (null if it can't be converted). */
Geo.toPath = function (o) {
  let polys, m;
  if (o.type === "rings") { polys = o.polys; m = o.m.slice(); }
  else if (o.type === "brush") { polys = fillGeom(o).map((P) => P.map((r) => r.flat())); m = M.id(); }
  else return null;
  const eps = 0.4 / Math.max(1e-6, M.scaleFactor(m));
  const nodes = ringsToNodes(polys.flat(), eps);
  if (nodes.length < 3) return null;
  return { ...baseProps(o, o.id), m, type: "path", closed: true, nodes, stroke: o.type === "brush" ? null : o.stroke || null, sw: o.type === "brush" ? 0 : o.sw || 0 };
};

// ---------------------------------------------------------------- merging (pathfinder)
/** The outline of a stroke as a filled area (round caps and joins). */
function strokeGeom(o) {
  const r = ((o.sw || 0) * M.scaleFactor(o.m)) / 2;
  if (r <= 0) return [];
  const trails = Geo.flatten(o).map((s) => (s.closed ? s.pts.concat([s.pts[0]]) : s.pts).map(([x, y]) => [x, y, r]));
  return Geo.eraserRegion(trails);
}
/** Everything an object covers (fill + stroke) as a MultiPolygon, or null for text / images. */
Geo.areaOf = function (o) {
  if (o.type === "text" || o.type === "image") return null;
  const parts = [];
  if (o.fill) parts.push(fillGeom(o));
  if (o.stroke && o.sw > 0 && o.type !== "brush") parts.push(strokeGeom(o));
  const nonEmpty = parts.filter((g) => g.length);
  if (!nonEmpty.length) return [];
  return nonEmpty.length === 1 ? nonEmpty[0] : polygonClipping.union(...nonEmpty);
};
/**
 * Combines objects (bottom-to-top order) into one filled shape.
 * op: unite | subtract (the back one minus the rest) | intersect | exclude
 */
Geo.combine = function (objs, op) {
  const areas = objs.map(Geo.areaOf);
  let res;
  if (op === "subtract") res = polygonClipping.difference(areas[0], ...areas.slice(1));
  else if (op === "intersect") res = polygonClipping.intersection(...areas);
  else if (op === "exclude") res = polygonClipping.xor(...areas);
  else res = polygonClipping.union(...areas);
  const src = op === "subtract" ? objs[0] : objs[objs.length - 1];
  const fill = src.fill ? cloneObj(src.fill) : src.stroke || "#111";
  return { ...baseProps(src, uid()), group: undefined, type: "rings", fill, stroke: null, sw: 0, polys: res.map((P) => P.map((r) => r.flat())) };
};

/** Erases the region from every vector object of a cell. Returns the ids that are gone or changed. */
Geo.cutCell = function (cell, region, skip) {
  if (!region.length || typeof polygonClipping === "undefined") return { changed: 0, removed: [] };
  const rb = regionBounds(region), out = [], removed = [];
  let n = 0;
  for (const o of cell.objects) {
    if (skip && skip.has(o.id)) { out.push(o); continue; }
    let r = null;
    try { r = Geo.cutObject(o, region, rb); } catch (e) { r = null; }
    if (r === null) { out.push(o); continue; }
    n++;
    if (!r.length) removed.push(o.id);
    out.push(...r);
  }
  cell.objects = out;
  return { changed: n, removed };
};
