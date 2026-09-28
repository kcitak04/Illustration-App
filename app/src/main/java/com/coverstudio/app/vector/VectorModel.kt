package com.coverstudio.app.vector

import android.graphics.BlendMode
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Region
import android.graphics.Shader
import android.graphics.Typeface
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt

// ------------------------------------------------------------------ paint styles

class Fill(
    var type: Int = SOLID,
    var color: Int = Color.BLACK,
    var color2: Int = Color.WHITE,
    var angle: Float = 90f
) {
    fun copy() = Fill(type, color, color2, angle)
    fun toJson() = JSONObject().put("t", type).put("c", color).put("c2", color2).put("a", angle.toDouble())

    companion object {
        const val NONE = 0
        const val SOLID = 1
        const val LINEAR = 2
        const val RADIAL = 3
        fun fromJson(o: JSONObject) = Fill(o.optInt("t", SOLID), o.optInt("c"), o.optInt("c2"), o.optDouble("a", 90.0).toFloat())
    }

    /** Configures [p] to paint this fill over [bounds] (document space). Returns false for NONE. */
    fun apply(p: Paint, bounds: RectF): Boolean {
        p.shader = null
        when (type) {
            NONE -> return false
            SOLID -> p.color = color
            LINEAR -> {
                val a = Math.toRadians(angle.toDouble())
                val cx = bounds.centerX(); val cy = bounds.centerY()
                val half = (abs(bounds.width() * cos(a)) + abs(bounds.height() * sin(a))).toFloat() / 2f
                val dx = (cos(a) * half).toFloat(); val dy = (sin(a) * half).toFloat()
                p.color = Color.BLACK
                p.shader = LinearGradient(cx - dx, cy - dy, cx + dx, cy + dy, color, color2, Shader.TileMode.CLAMP)
            }
            RADIAL -> {
                p.color = Color.BLACK
                val r = max(1f, max(bounds.width(), bounds.height()) / 2f)
                p.shader = RadialGradient(bounds.centerX(), bounds.centerY(), r, color, color2, Shader.TileMode.CLAMP)
            }
        }
        return true
    }
}

class StrokeStyle(var enabled: Boolean = false, var color: Int = Color.BLACK, var width: Float = 4f, var round: Boolean = true) {
    fun copy() = StrokeStyle(enabled, color, width, round)
    fun toJson() = JSONObject().put("e", enabled).put("c", color).put("w", width.toDouble()).put("r", round)
    companion object {
        fun fromJson(o: JSONObject) = StrokeStyle(o.optBoolean("e"), o.optInt("c"), o.optDouble("w", 4.0).toFloat(), o.optBoolean("r", true))
    }
}

val BLEND_NAMES = listOf(
    "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "Color Dodge", "Color Burn",
    "Hard Light", "Soft Light", "Difference", "Exclusion", "Hue", "Saturation", "Color", "Luminosity", "Add"
)

fun blendModeOf(i: Int): BlendMode? = when (i) {
    1 -> BlendMode.MULTIPLY
    2 -> BlendMode.SCREEN
    3 -> BlendMode.OVERLAY
    4 -> BlendMode.DARKEN
    5 -> BlendMode.LIGHTEN
    6 -> BlendMode.COLOR_DODGE
    7 -> BlendMode.COLOR_BURN
    8 -> BlendMode.HARD_LIGHT
    9 -> BlendMode.SOFT_LIGHT
    10 -> BlendMode.DIFFERENCE
    11 -> BlendMode.EXCLUSION
    12 -> BlendMode.HUE
    13 -> BlendMode.SATURATION
    14 -> BlendMode.COLOR
    15 -> BlendMode.LUMINOSITY
    16 -> BlendMode.PLUS
    else -> null
}

val SVG_BLEND = listOf(
    "normal", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge", "color-burn",
    "hard-light", "soft-light", "difference", "exclusion", "hue", "saturation", "color", "luminosity", "plus-lighter"
)

// ------------------------------------------------------------------ shapes

private var idCounter = System.currentTimeMillis()
fun newId(): Long = ++idCounter

abstract class Shape {
    var id: Long = newId()
    val matrix = Matrix()
    var fill = Fill()
    var stroke = StrokeStyle()
    var opacity = 1f
    var blend = 0

    private var cachedPath: Path? = null
    private var cachedDocPath: Path? = null
    private val tmpVals = FloatArray(9)

    abstract val kind: String
    protected abstract fun buildLocalPath(): Path
    abstract fun copyGeometry(): Shape
    protected abstract fun geometryJson(o: JSONObject)

    /** Call after mutating geometry. */
    fun invalidate() { cachedPath = null; cachedDocPath = null }

    /** Call after mutating [matrix] only. */
    fun invalidateTransform() { cachedDocPath = null }

    fun localPath(): Path = cachedPath ?: buildLocalPath().also { cachedPath = it }

    fun docPath(): Path = cachedDocPath ?: Path().also { localPath().transform(matrix, it); cachedDocPath = it }

    /** Uniform scale factor of the transform, used to scale stroke widths without distortion. */
    fun scaleFactor(): Float {
        matrix.getValues(tmpVals)
        val det = tmpVals[0] * tmpVals[4] - tmpVals[1] * tmpVals[3]
        return sqrt(abs(det)).coerceAtLeast(0.0001f)
    }

    fun rotationDeg(): Float {
        matrix.getValues(tmpVals)
        return Math.toDegrees(atan2(tmpVals[3].toDouble(), tmpVals[0].toDouble())).toFloat()
    }

    open fun docBounds(): RectF {
        val r = RectF(); docPath().computeBounds(r, true)
        if (stroke.enabled) { val h = stroke.width * scaleFactor() / 2; r.inset(-h, -h) }
        return r
    }

    open fun draw(c: Canvas, p: Paint) {
        val path = docPath()
        val b = RectF(); path.computeBounds(b, true)
        p.reset(); p.isAntiAlias = true
        p.blendMode = blendModeOf(blend)
        p.style = Paint.Style.FILL
        if (fill.apply(p, b)) {
            p.alpha = (Color.alpha(if (fill.type == Fill.SOLID) fill.color else 255) * opacity).toInt()
            c.drawPath(path, p)
        }
        if (stroke.enabled) {
            p.shader = null
            p.style = Paint.Style.STROKE
            p.color = stroke.color
            p.alpha = (Color.alpha(stroke.color) * opacity).toInt()
            p.strokeWidth = stroke.width * scaleFactor()
            p.strokeCap = if (stroke.round) Paint.Cap.ROUND else Paint.Cap.BUTT
            p.strokeJoin = if (stroke.round) Paint.Join.ROUND else Paint.Join.MITER
            c.drawPath(path, p)
        }
    }

    /** Hit test in document space with tolerance [tol]. */
    open fun hit(x: Float, y: Float, tol: Float): Boolean {
        val b = docBounds()
        b.inset(-tol, -tol)
        if (!b.contains(x, y)) return false
        val path = docPath()
        if (fill.type != Fill.NONE && regionContains(path, x, y)) return true
        val p = Paint().apply {
            style = Paint.Style.STROKE
            strokeWidth = (if (stroke.enabled) stroke.width * scaleFactor() else 0f) + tol * 2
            strokeCap = Paint.Cap.ROUND; strokeJoin = Paint.Join.ROUND
        }
        val outline = Path()
        @Suppress("DEPRECATION")
        p.getFillPath(path, outline)
        return regionContains(outline, x, y)
    }

    fun copy(): Shape = copyGeometry().also {
        it.matrix.set(matrix); it.fill = fill.copy(); it.stroke = stroke.copy()
        it.opacity = opacity; it.blend = blend
    }

    fun toJson(): JSONObject {
        val o = JSONObject()
        o.put("kind", kind)
        matrix.getValues(tmpVals)
        o.put("m", JSONArray().also { a -> tmpVals.forEach { a.put(it.toDouble()) } })
        o.put("fill", fill.toJson()); o.put("stroke", stroke.toJson())
        o.put("op", opacity.toDouble()); o.put("blend", blend)
        geometryJson(o)
        return o
    }

    companion object {
        fun fromJson(o: JSONObject): Shape? {
            val s: Shape = when (o.optString("kind")) {
                "brush" -> BrushShape.parse(o)
                "path" -> PathShape.parse(o)
                "rect" -> RectShape(o.optDouble("w").toFloat(), o.optDouble("h").toFloat(), o.optDouble("r", 0.0).toFloat())
                "ellipse" -> EllipseShape(o.optDouble("w").toFloat(), o.optDouble("h").toFloat())
                "poly" -> PolyShape(o.optDouble("w").toFloat(), o.optDouble("h").toFloat(), o.optInt("n", 5), o.optBoolean("star"), o.optDouble("ir", 0.5).toFloat())
                "text" -> TextShape.parse(o)
                else -> return null
            }
            o.optJSONArray("m")?.let { a -> val v = FloatArray(9) { a.optDouble(it).toFloat() }; s.matrix.setValues(v) }
            o.optJSONObject("fill")?.let { s.fill = Fill.fromJson(it) }
            o.optJSONObject("stroke")?.let { s.stroke = StrokeStyle.fromJson(it) }
            s.opacity = o.optDouble("op", 1.0).toFloat()
            s.blend = o.optInt("blend")
            return s
        }
    }
}

fun regionContains(path: Path, x: Float, y: Float): Boolean {
    val b = RectF(); path.computeBounds(b, true)
    // regions are integer based; scale small shapes up for precision
    val scale = if (max(b.width(), b.height()) < 200f) 200f / max(1f, max(b.width(), b.height())) else 1f
    val p = Path(path)
    val m = Matrix(); m.setScale(scale, scale); p.transform(m)
    val clip = Region((b.left * scale).toInt() - 2, (b.top * scale).toInt() - 2, (b.right * scale).toInt() + 2, (b.bottom * scale).toInt() + 2)
    val r = Region(); r.setPath(p, clip)
    return r.contains((x * scale).toInt(), (y * scale).toInt())
}

/** Pressure-sensitive brush stroke, stored as centre-line samples (x, y, pressure). */
class BrushShape(val pts: FloatArray, val size: Float, val minRatio: Float, val taper: Boolean) : Shape() {
    override val kind = "brush"

    override fun buildLocalPath(): Path = buildOutline(pts, size, minRatio, taper)

    override fun copyGeometry() = BrushShape(pts.copyOf(), size, minRatio, taper)

    override fun geometryJson(o: JSONObject) {
        o.put("size", size.toDouble()); o.put("min", minRatio.toDouble()); o.put("taper", taper)
        o.put("pts", JSONArray().also { a -> pts.forEach { a.put(((it * 100).toInt() / 100.0)) } })
    }

    companion object {
        fun parse(o: JSONObject): BrushShape {
            val a = o.optJSONArray("pts") ?: JSONArray()
            return BrushShape(FloatArray(a.length()) { a.optDouble(it).toFloat() }, o.optDouble("size", 10.0).toFloat(),
                o.optDouble("min", 0.2).toFloat(), o.optBoolean("taper", true))
        }

        /** Builds a filled outline polygon for a variable-width stroke. */
        fun buildOutline(pts: FloatArray, size: Float, minRatio: Float, taper: Boolean): Path {
            val path = Path()
            path.fillType = Path.FillType.WINDING
            outline(pts, size, minRatio, taper, AndroidSink(path))
            return path
        }

        fun outline(pts: FloatArray, size: Float, minRatio: Float, taper: Boolean, sink: PathSink) {
            val n = pts.size / 3
            if (n == 0) return
            fun w(i: Int): Float {
                var r = size * (minRatio + (1 - minRatio) * pts[i * 3 + 2]) / 2f
                if (taper && n > 6) {
                    val k = 4
                    if (i < k) r *= 0.35f + 0.65f * i / k
                    if (i > n - 1 - k) r *= 0.35f + 0.65f * (n - 1 - i) / k
                }
                return max(0.3f, r)
            }
            if (n == 1) {
                val r = w(0)
                sink.moveTo(pts[0] - r, pts[1]); sink.arc(r, pts[0] + r, pts[1], false); sink.arc(r, pts[0] - r, pts[1], false)
                sink.close(); return
            }
            val lx = FloatArray(n); val ly = FloatArray(n); val rx = FloatArray(n); val ry = FloatArray(n)
            for (i in 0 until n) {
                val a = max(0, i - 1); val b = minOf(n - 1, i + 1)
                var tx = pts[b * 3] - pts[a * 3]; var ty = pts[b * 3 + 1] - pts[a * 3 + 1]
                val len = hypot(tx, ty)
                if (len < 1e-4f) { tx = 1f; ty = 0f } else { tx /= len; ty /= len }
                val r = w(i)
                lx[i] = pts[i * 3] - ty * r; ly[i] = pts[i * 3 + 1] + tx * r
                rx[i] = pts[i * 3] + ty * r; ry[i] = pts[i * 3 + 1] - tx * r
            }
            // left side (smoothed with quadratic midpoints)
            sink.moveTo(lx[0], ly[0])
            for (i in 1 until n - 1) sink.quadTo(lx[i], ly[i], (lx[i] + lx[i + 1]) / 2, (ly[i] + ly[i + 1]) / 2)
            sink.lineTo(lx[n - 1], ly[n - 1])
            // round end cap
            sink.arc(w(n - 1), rx[n - 1], ry[n - 1], false)
            // right side back
            for (i in n - 2 downTo 1) sink.quadTo(rx[i], ry[i], (rx[i] + rx[i - 1]) / 2, (ry[i] + ry[i - 1]) / 2)
            sink.lineTo(rx[0], ry[0])
            sink.arc(w(0), lx[0], ly[0], false)
            sink.close()
        }
    }
}

/** Minimal path builder abstraction so outlines can be emitted to both Android and SVG. */
interface PathSink {
    fun moveTo(x: Float, y: Float)
    fun lineTo(x: Float, y: Float)
    fun quadTo(x1: Float, y1: Float, x: Float, y: Float)
    fun cubicTo(x1: Float, y1: Float, x2: Float, y2: Float, x: Float, y: Float)
    /** Half-circle arc of radius [r] from the current point to (x, y). [clockwise] in screen space. */
    fun arc(r: Float, x: Float, y: Float, clockwise: Boolean)
    fun close()
}

class AndroidSink(val p: Path) : PathSink {
    private var cx = 0f
    private var cy = 0f
    override fun moveTo(x: Float, y: Float) { p.moveTo(x, y); cx = x; cy = y }
    override fun lineTo(x: Float, y: Float) { p.lineTo(x, y); cx = x; cy = y }
    override fun quadTo(x1: Float, y1: Float, x: Float, y: Float) { p.quadTo(x1, y1, x, y); cx = x; cy = y }
    override fun cubicTo(x1: Float, y1: Float, x2: Float, y2: Float, x: Float, y: Float) { p.cubicTo(x1, y1, x2, y2, x, y); cx = x; cy = y }
    override fun arc(r: Float, x: Float, y: Float, clockwise: Boolean) {
        val mx = (cx + x) / 2; val my = (cy + y) / 2
        val rr = max(0.01f, hypot(x - cx, y - cy) / 2)
        val start = Math.toDegrees(atan2((cy - my).toDouble(), (cx - mx).toDouble())).toFloat()
        p.arcTo(RectF(mx - rr, my - rr, mx + rr, my + rr), start, if (clockwise) 180f else -180f)
        cx = x; cy = y
    }
    override fun close() { p.close() }
}

/** A Bézier anchor with in/out control handles (absolute local coordinates). */
class Node(var x: Float, var y: Float, var inX: Float = x, var inY: Float = y, var outX: Float = x, var outY: Float = y, var smooth: Boolean = false) {
    fun copy() = Node(x, y, inX, inY, outX, outY, smooth)
    fun translate(dx: Float, dy: Float) { x += dx; y += dy; inX += dx; inY += dy; outX += dx; outY += dy }
    fun hasIn() = inX != x || inY != y
    fun hasOut() = outX != x || outY != y
}

class PathShape(val nodes: MutableList<Node>, var closed: Boolean) : Shape() {
    override val kind = "path"

    override fun buildLocalPath(): Path {
        val p = Path()
        if (nodes.isEmpty()) return p
        p.moveTo(nodes[0].x, nodes[0].y)
        for (i in 1 until nodes.size) seg(p, nodes[i - 1], nodes[i])
        if (closed && nodes.size > 1) { seg(p, nodes.last(), nodes[0]); p.close() }
        return p
    }

    private fun seg(p: Path, a: Node, b: Node) {
        if (!a.hasOut() && !b.hasIn()) p.lineTo(b.x, b.y)
        else p.cubicTo(a.outX, a.outY, b.inX, b.inY, b.x, b.y)
    }

    override fun copyGeometry() = PathShape(nodes.map { it.copy() }.toMutableList(), closed)

    override fun geometryJson(o: JSONObject) {
        o.put("closed", closed)
        val a = JSONArray()
        for (n in nodes) {
            a.put(n.x.toDouble()); a.put(n.y.toDouble()); a.put(n.inX.toDouble()); a.put(n.inY.toDouble())
            a.put(n.outX.toDouble()); a.put(n.outY.toDouble()); a.put(if (n.smooth) 1 else 0)
        }
        o.put("nodes", a)
    }

    /** Inserts a node halfway along the segment starting at [i] (de Casteljau split). */
    fun subdivide(i: Int): Int {
        if (nodes.size < 2) return -1
        val j = if (i + 1 < nodes.size) i + 1 else if (closed) 0 else return -1
        val a = nodes[i]; val b = nodes[j]
        val p0x = a.x; val p0y = a.y; val p1x = a.outX; val p1y = a.outY
        val p2x = b.inX; val p2y = b.inY; val p3x = b.x; val p3y = b.y
        fun mid(u: Float, v: Float) = (u + v) / 2
        val q0x = mid(p0x, p1x); val q0y = mid(p0y, p1y)
        val q1x = mid(p1x, p2x); val q1y = mid(p1y, p2y)
        val q2x = mid(p2x, p3x); val q2y = mid(p2y, p3y)
        val r0x = mid(q0x, q1x); val r0y = mid(q0y, q1y)
        val r1x = mid(q1x, q2x); val r1y = mid(q1y, q2y)
        val sx = mid(r0x, r1x); val sy = mid(r0y, r1y)
        val curved = a.hasOut() || b.hasIn()
        val n = if (curved) Node(sx, sy, r0x, r0y, r1x, r1y, true) else Node(sx, sy)
        if (curved) { a.outX = q0x; a.outY = q0y; b.inX = q2x; b.inY = q2y }
        val at = if (j == 0) nodes.size else j
        nodes.add(at, n)
        invalidate()
        return at
    }

    companion object {
        fun parse(o: JSONObject): PathShape {
            val a = o.optJSONArray("nodes") ?: JSONArray()
            val list = ArrayList<Node>()
            var i = 0
            while (i + 6 < a.length()) {
                list.add(Node(a.optDouble(i).toFloat(), a.optDouble(i + 1).toFloat(), a.optDouble(i + 2).toFloat(),
                    a.optDouble(i + 3).toFloat(), a.optDouble(i + 4).toFloat(), a.optDouble(i + 5).toFloat(), a.optInt(i + 6) == 1))
                i += 7
            }
            return PathShape(list, o.optBoolean("closed"))
        }
    }
}

class RectShape(var w: Float, var h: Float, var radius: Float = 0f) : Shape() {
    override val kind = "rect"
    override fun buildLocalPath() = Path().apply {
        if (radius > 0) addRoundRect(RectF(0f, 0f, w, h), radius, radius, Path.Direction.CW)
        else addRect(RectF(0f, 0f, w, h), Path.Direction.CW)
    }
    override fun copyGeometry() = RectShape(w, h, radius)
    override fun geometryJson(o: JSONObject) { o.put("w", w.toDouble()); o.put("h", h.toDouble()); o.put("r", radius.toDouble()) }
}

class EllipseShape(var w: Float, var h: Float) : Shape() {
    override val kind = "ellipse"
    override fun buildLocalPath() = Path().apply { addOval(RectF(0f, 0f, w, h), Path.Direction.CW) }
    override fun copyGeometry() = EllipseShape(w, h)
    override fun geometryJson(o: JSONObject) { o.put("w", w.toDouble()); o.put("h", h.toDouble()) }
}

class PolyShape(var w: Float, var h: Float, var sides: Int, var star: Boolean, var innerRatio: Float) : Shape() {
    override val kind = "poly"
    override fun buildLocalPath(): Path {
        val p = Path()
        val cx = w / 2; val cy = h / 2
        val n = if (star) sides * 2 else sides
        for (i in 0 until n) {
            val r = if (star && i % 2 == 1) innerRatio else 1f
            val a = 2 * PI * i / n - PI / 2
            val x = cx + (cos(a) * r * w / 2).toFloat()
            val y = cy + (sin(a) * r * h / 2).toFloat()
            if (i == 0) p.moveTo(x, y) else p.lineTo(x, y)
        }
        p.close()
        return p
    }
    override fun copyGeometry() = PolyShape(w, h, sides, star, innerRatio)
    override fun geometryJson(o: JSONObject) {
        o.put("w", w.toDouble()); o.put("h", h.toDouble()); o.put("n", sides); o.put("star", star); o.put("ir", innerRatio.toDouble())
    }
}

object Fonts {
    val families = listOf(
        "sans-serif", "sans-serif-medium", "sans-serif-black", "sans-serif-light", "sans-serif-thin",
        "sans-serif-condensed", "serif", "monospace", "serif-monospace", "casual", "cursive", "sans-serif-smallcaps"
    )
    private val cache = HashMap<String, Typeface>()

    /** [font] is a family name or an absolute path to an imported .ttf/.otf file. */
    fun typeface(font: String, bold: Boolean, italic: Boolean): Typeface {
        val key = "$font|$bold|$italic"
        return cache.getOrPut(key) {
            val style = when {
                bold && italic -> Typeface.BOLD_ITALIC
                bold -> Typeface.BOLD
                italic -> Typeface.ITALIC
                else -> Typeface.NORMAL
            }
            val base = if (font.startsWith("/")) {
                try { Typeface.createFromFile(File(font)) } catch (e: Exception) { Typeface.DEFAULT }
            } else Typeface.create(font, Typeface.NORMAL)
            Typeface.create(base, style)
        }
    }

    fun displayName(font: String) = if (font.startsWith("/")) File(font).nameWithoutExtension else font
}

class TextShape(
    var text: String,
    var size: Float,
    var font: String = "sans-serif",
    var bold: Boolean = false,
    var italic: Boolean = false,
    var align: Int = 0,
    var letterSpacing: Float = 0f,
    var lineHeight: Float = 1.15f
) : Shape() {
    override val kind = "text"

    fun paint(): Paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Fonts.typeface(font, bold, italic)
        textSize = size
        letterSpacing = this@TextShape.letterSpacing
        textAlign = when (align) { 1 -> Paint.Align.CENTER; 2 -> Paint.Align.RIGHT; else -> Paint.Align.LEFT }
    }

    fun lines() = text.split("\n")

    /** Local box: (0,0) top-left, width = widest line. */
    fun localBox(): RectF {
        val p = paint()
        val w = lines().maxOfOrNull { p.measureText(it) } ?: 0f
        val h = size * lineHeight * lines().size
        return RectF(0f, 0f, max(w, 1f), max(h, 1f))
    }

    private fun lineX(box: RectF) = when (align) { 1 -> box.width() / 2; 2 -> box.width(); else -> 0f }

    override fun buildLocalPath(): Path {
        // outline path is used for hit-testing, bounds and SVG/outline conversion
        val p = paint()
        val box = localBox()
        val out = Path()
        val fm = p.fontMetrics
        lines().forEachIndexed { i, line ->
            val lp = Path()
            p.getTextPath(line, 0, line.length, lineX(box), -fm.ascent + i * size * lineHeight, lp)
            out.addPath(lp)
        }
        return out
    }

    override fun docBounds(): RectF {
        val r = localBox(); matrix.mapRect(r)
        if (stroke.enabled) { val h = stroke.width * scaleFactor() / 2; r.inset(-h, -h) }
        return r
    }

    override fun hit(x: Float, y: Float, tol: Float): Boolean {
        val inv = Matrix(); if (!matrix.invert(inv)) return false
        val pt = floatArrayOf(x, y); inv.mapPoints(pt)
        val box = localBox(); val t = tol / scaleFactor(); box.inset(-t, -t)
        return box.contains(pt[0], pt[1])
    }

    override fun draw(c: Canvas, p: Paint) {
        val tp = paint()
        tp.blendMode = blendModeOf(blend)
        val box = localBox()
        val fm = tp.fontMetrics
        c.save()
        c.concat(matrix)
        fun drawLines() {
            lines().forEachIndexed { i, line -> c.drawText(line, lineX(box), -fm.ascent + i * size * lineHeight, tp) }
        }
        if (fill.type != Fill.NONE) {
            tp.style = Paint.Style.FILL
            fill.apply(tp, box)
            tp.alpha = (Color.alpha(if (fill.type == Fill.SOLID) fill.color else 255) * opacity).toInt()
            drawLines()
        }
        if (stroke.enabled) {
            tp.shader = null
            tp.style = Paint.Style.STROKE
            tp.color = stroke.color
            tp.alpha = (Color.alpha(stroke.color) * opacity).toInt()
            tp.strokeWidth = stroke.width
            tp.strokeJoin = Paint.Join.ROUND
            drawLines()
        }
        c.restore()
    }

    override fun copyGeometry() = TextShape(text, size, font, bold, italic, align, letterSpacing, lineHeight)

    override fun geometryJson(o: JSONObject) {
        o.put("text", text); o.put("size", size.toDouble()); o.put("font", font); o.put("bold", bold); o.put("italic", italic)
        o.put("align", align); o.put("ls", letterSpacing.toDouble()); o.put("lh", lineHeight.toDouble())
    }

    companion object {
        fun parse(o: JSONObject) = TextShape(
            o.optString("text"), o.optDouble("size", 100.0).toFloat(), o.optString("font", "sans-serif"),
            o.optBoolean("bold"), o.optBoolean("italic"), o.optInt("align"), o.optDouble("ls", 0.0).toFloat(), o.optDouble("lh", 1.15).toFloat()
        )
    }
}

// ------------------------------------------------------------------ document

class VectorLayer(var name: String) {
    var visible = true
    var locked = false
    var opacity = 1f
    val shapes = ArrayList<Shape>()

    fun copy(): VectorLayer = VectorLayer(name).also {
        it.visible = visible; it.locked = locked; it.opacity = opacity
        shapes.forEach { s -> it.shapes.add(s.copy().also { c -> c.id = s.id }) }
    }

    fun toJson(): JSONObject = JSONObject().put("name", name).put("visible", visible).put("locked", locked)
        .put("opacity", opacity.toDouble()).put("shapes", JSONArray().also { a -> shapes.forEach { a.put(it.toJson()) } })

    companion object {
        fun fromJson(o: JSONObject) = VectorLayer(o.optString("name", "Layer")).also {
            it.visible = o.optBoolean("visible", true); it.locked = o.optBoolean("locked")
            it.opacity = o.optDouble("opacity", 1.0).toFloat()
            val a = o.optJSONArray("shapes") ?: JSONArray()
            for (i in 0 until a.length()) Shape.fromJson(a.getJSONObject(i))?.let { s -> it.shapes.add(s) }
        }
    }
}

class VectorDoc(var width: Int, var height: Int) {
    var background: Int = Color.WHITE
    val layers = ArrayList<VectorLayer>().apply { add(VectorLayer("Layer 1")) }
    var active = 0

    val activeLayer: VectorLayer get() = layers[active.coerceIn(0, layers.size - 1)]

    fun render(c: Canvas, drawBackground: Boolean = true) {
        if (drawBackground && Color.alpha(background) > 0) c.drawColor(background)
        val p = Paint()
        for (l in layers) {
            if (!l.visible) continue
            val needLayer = l.opacity < 1f
            if (needLayer) c.saveLayerAlpha(null, (l.opacity * 255).toInt())
            for (s in l.shapes) s.draw(c, p)
            if (needLayer) c.restore()
        }
    }

    fun snapshot(): VectorDoc = VectorDoc(width, height).also { d ->
        d.background = background; d.layers.clear(); layers.forEach { d.layers.add(it.copy()) }; d.active = active
    }

    fun restoreFrom(o: VectorDoc) {
        width = o.width; height = o.height; background = o.background
        layers.clear(); o.layers.forEach { layers.add(it.copy()) }; active = o.active
    }

    fun findLayerOf(s: Shape): VectorLayer? = layers.firstOrNull { it.shapes.contains(s) }

    fun toJson(): JSONObject = JSONObject().put("w", width).put("h", height).put("bg", background).put("active", active)
        .put("layers", JSONArray().also { a -> layers.forEach { a.put(it.toJson()) } })

    companion object {
        fun fromJson(o: JSONObject): VectorDoc {
            val d = VectorDoc(o.optInt("w", 3000), o.optInt("h", 3000))
            d.background = o.optInt("bg", Color.WHITE)
            val a = o.optJSONArray("layers")
            if (a != null && a.length() > 0) {
                d.layers.clear()
                for (i in 0 until a.length()) d.layers.add(VectorLayer.fromJson(a.getJSONObject(i)))
            }
            d.active = o.optInt("active").coerceIn(0, d.layers.size - 1)
            return d
        }
    }
}
