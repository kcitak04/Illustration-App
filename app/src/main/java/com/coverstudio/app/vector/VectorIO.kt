package com.coverstudio.app.vector

import android.graphics.Color
import android.graphics.Matrix
import android.graphics.RectF
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

private fun f(v: Float): String {
    val r = Math.round(v * 100) / 100.0
    return if (r == Math.floor(r)) r.toLong().toString() else r.toString()
}

class SvgSink : PathSink {
    val sb = StringBuilder()
    override fun moveTo(x: Float, y: Float) { sb.append("M").append(f(x)).append(' ').append(f(y)) }
    override fun lineTo(x: Float, y: Float) { sb.append("L").append(f(x)).append(' ').append(f(y)) }
    override fun quadTo(x1: Float, y1: Float, x: Float, y: Float) {
        sb.append("Q").append(f(x1)).append(' ').append(f(y1)).append(' ').append(f(x)).append(' ').append(f(y))
    }
    override fun cubicTo(x1: Float, y1: Float, x2: Float, y2: Float, x: Float, y: Float) {
        sb.append("C").append(f(x1)).append(' ').append(f(y1)).append(' ').append(f(x2)).append(' ').append(f(y2))
            .append(' ').append(f(x)).append(' ').append(f(y))
    }
    override fun arc(r: Float, x: Float, y: Float, clockwise: Boolean) {
        sb.append("A").append(f(r)).append(' ').append(f(r)).append(" 0 0 ").append(if (clockwise) 1 else 0)
            .append(' ').append(f(x)).append(' ').append(f(y))
    }
    override fun close() { sb.append("Z") }
}

/** Converts rectangles, ellipses and polygons into editable Bézier paths. */
fun toPathShape(s: Shape): PathShape? {
    val nodes = ArrayList<Node>()
    when (s) {
        is PathShape -> return s
        is RectShape -> {
            nodes.add(Node(0f, 0f)); nodes.add(Node(s.w, 0f)); nodes.add(Node(s.w, s.h)); nodes.add(Node(0f, s.h))
        }
        is EllipseShape -> {
            val k = 0.5523f
            val rx = s.w / 2; val ry = s.h / 2; val cx = rx; val cy = ry
            nodes.add(Node(cx, 0f, cx - rx * k, 0f, cx + rx * k, 0f, true))
            nodes.add(Node(s.w, cy, s.w, cy - ry * k, s.w, cy + ry * k, true))
            nodes.add(Node(cx, s.h, cx + rx * k, s.h, cx - rx * k, s.h, true))
            nodes.add(Node(0f, cy, 0f, cy + ry * k, 0f, cy - ry * k, true))
        }
        is PolyShape -> {
            val n = if (s.star) s.sides * 2 else s.sides
            for (i in 0 until n) {
                val r = if (s.star && i % 2 == 1) s.innerRatio else 1f
                val a = 2 * PI * i / n - PI / 2
                nodes.add(Node(s.w / 2 + (cos(a) * r * s.w / 2).toFloat(), s.h / 2 + (sin(a) * r * s.h / 2).toFloat()))
            }
        }
        else -> return null
    }
    val p = PathShape(nodes, true)
    p.matrix.set(s.matrix); p.fill = s.fill.copy(); p.stroke = s.stroke.copy(); p.opacity = s.opacity; p.blend = s.blend
    p.id = s.id
    return p
}

private fun svgColor(c: Int) = String.format("#%06X", c and 0xFFFFFF)
private fun esc(t: String) = t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;")

private fun svgMatrix(m: Matrix): String {
    val v = FloatArray(9); m.getValues(v)
    return "matrix(${f(v[0])} ${f(v[3])} ${f(v[1])} ${f(v[4])} ${f(v[2])} ${f(v[5])})"
}

fun exportSvg(doc: VectorDoc): String {
    val defs = StringBuilder()
    val body = StringBuilder()
    var gid = 0
    fun paintAttr(fill: Fill, bounds: RectF, alphaScale: Float): String {
        return when (fill.type) {
            Fill.NONE -> "fill=\"none\""
            Fill.SOLID -> "fill=\"${svgColor(fill.color)}\"" +
                (if (Color.alpha(fill.color) < 255) " fill-opacity=\"${f(Color.alpha(fill.color) / 255f)}\"" else "")
            else -> {
                val id = "g${gid++}"
                val c1 = svgColor(fill.color); val o1 = f(Color.alpha(fill.color) / 255f)
                val c2 = svgColor(fill.color2); val o2 = f(Color.alpha(fill.color2) / 255f)
                if (fill.type == Fill.LINEAR) {
                    val a = Math.toRadians(fill.angle.toDouble())
                    val cx = bounds.centerX(); val cy = bounds.centerY()
                    val half = (Math.abs(bounds.width() * cos(a)) + Math.abs(bounds.height() * sin(a))).toFloat() / 2f
                    val dx = (cos(a) * half).toFloat(); val dy = (sin(a) * half).toFloat()
                    defs.append("<linearGradient id=\"$id\" gradientUnits=\"userSpaceOnUse\" x1=\"${f(cx - dx)}\" y1=\"${f(cy - dy)}\" x2=\"${f(cx + dx)}\" y2=\"${f(cy + dy)}\">")
                    defs.append("<stop offset=\"0\" stop-color=\"$c1\" stop-opacity=\"$o1\"/><stop offset=\"1\" stop-color=\"$c2\" stop-opacity=\"$o2\"/></linearGradient>\n")
                } else {
                    val r = maxOf(1f, maxOf(bounds.width(), bounds.height()) / 2f)
                    defs.append("<radialGradient id=\"$id\" gradientUnits=\"userSpaceOnUse\" cx=\"${f(bounds.centerX())}\" cy=\"${f(bounds.centerY())}\" r=\"${f(r)}\">")
                    defs.append("<stop offset=\"0\" stop-color=\"$c1\" stop-opacity=\"$o1\"/><stop offset=\"1\" stop-color=\"$c2\" stop-opacity=\"$o2\"/></radialGradient>\n")
                }
                "fill=\"url(#$id)\""
            }
        }
    }

    for (layer in doc.layers) {
        if (!layer.visible) continue
        body.append("<g id=\"${esc(layer.name)}\"").append(if (layer.opacity < 1f) " opacity=\"${f(layer.opacity)}\"" else "").append(">\n")
        for (s in layer.shapes) {
            val common = StringBuilder()
            if (s.opacity < 1f) common.append(" opacity=\"${f(s.opacity)}\"")
            if (s.blend != 0) common.append(" style=\"mix-blend-mode:${SVG_BLEND[s.blend]}\"")
            val stroke = if (s.stroke.enabled)
                " stroke=\"${svgColor(s.stroke.color)}\" stroke-width=\"${f(s.stroke.width)}\"" +
                    (if (Color.alpha(s.stroke.color) < 255) " stroke-opacity=\"${f(Color.alpha(s.stroke.color) / 255f)}\"" else "") +
                    (if (s.stroke.round) " stroke-linecap=\"round\" stroke-linejoin=\"round\"" else "")
            else ""
            when (s) {
                is TextShape -> {
                    val box = s.localBox()
                    val anchor = when (s.align) { 1 -> "middle"; 2 -> "end"; else -> "start" }
                    val x = when (s.align) { 1 -> box.width() / 2; 2 -> box.width(); else -> 0f }
                    val fm = s.paint().fontMetrics
                    val fam = if (s.font.startsWith("/")) Fonts.displayName(s.font) else s.font
                    body.append("<text transform=\"${svgMatrix(s.matrix)}\" font-family=\"${esc(fam)}\" font-size=\"${f(s.size)}\"")
                    if (s.bold) body.append(" font-weight=\"bold\"")
                    if (s.italic) body.append(" font-style=\"italic\"")
                    if (s.letterSpacing != 0f) body.append(" letter-spacing=\"${f(s.letterSpacing * s.size)}\"")
                    body.append(" text-anchor=\"$anchor\" ").append(paintAttr(s.fill, box, 1f)).append(stroke).append(common).append(">")
                    s.lines().forEachIndexed { i, line ->
                        body.append("<tspan x=\"${f(x)}\" y=\"${f(-fm.ascent + i * s.size * s.lineHeight)}\">${esc(line)}</tspan>")
                    }
                    body.append("</text>\n")
                }
                else -> {
                    val sink = SvgSink()
                    when (s) {
                        is BrushShape -> BrushShape.outline(s.pts, s.size, s.minRatio, s.taper, sink)
                        else -> {
                            val p = toPathShape(s) ?: continue
                            if (p.nodes.isEmpty()) continue
                            sink.moveTo(p.nodes[0].x, p.nodes[0].y)
                            fun seg(a: Node, b: Node) {
                                if (!a.hasOut() && !b.hasIn()) sink.lineTo(b.x, b.y)
                                else sink.cubicTo(a.outX, a.outY, b.inX, b.inY, b.x, b.y)
                            }
                            for (i in 1 until p.nodes.size) seg(p.nodes[i - 1], p.nodes[i])
                            if (p.closed) { seg(p.nodes.last(), p.nodes[0]); sink.close() }
                        }
                    }
                    // gradients are defined in document space, so resolve them against doc bounds
                    val local = RectF(); s.localPath().computeBounds(local, true)
                    body.append("<path transform=\"${svgMatrix(s.matrix)}\" d=\"${sink.sb}\" ")
                        .append(paintAttr(s.fill, local, 1f)).append(stroke)
                        .append(common).append("/>\n")
                }
            }
        }
        body.append("</g>\n")
    }
    val sb = StringBuilder()
    sb.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
    sb.append("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"${doc.width}\" height=\"${doc.height}\" viewBox=\"0 0 ${doc.width} ${doc.height}\">\n")
    if (defs.isNotEmpty()) sb.append("<defs>\n").append(defs).append("</defs>\n")
    if (Color.alpha(doc.background) > 0) sb.append("<rect width=\"100%\" height=\"100%\" fill=\"${svgColor(doc.background)}\"/>\n")
    sb.append(body)
    sb.append("</svg>\n")
    return sb.toString()
}
