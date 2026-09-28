package com.coverstudio.app.ui

import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF

/**
 * Hand-drawn line icons on a 24x24 grid. Keeping them in code means the app has
 * no icon resources to maintain and every glyph matches the same stroke style.
 */
object Icons {
    private val path = Path()
    private val r = RectF()

    fun draw(c: Canvas, name: String, cx: Float, cy: Float, size: Float, color: Int) {
        val s = size / 24f
        val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = 1.8f
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
            this.color = color
        }
        val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; this.color = color }
        c.save()
        c.translate(cx - 12f * s, cy - 12f * s)
        c.scale(s, s)
        path.reset()
        when (name) {
            "select" -> {
                path.moveTo(6f, 3f); path.lineTo(6f, 19f); path.lineTo(10f, 15f); path.lineTo(13f, 21f)
                path.lineTo(15.5f, 20f); path.lineTo(12.5f, 14f); path.lineTo(18f, 14f); path.close()
                c.drawPath(path, fill)
            }
            "direct" -> {
                path.moveTo(6f, 3f); path.lineTo(6f, 19f); path.lineTo(10f, 15f); path.lineTo(13f, 21f)
                path.lineTo(15.5f, 20f); path.lineTo(12.5f, 14f); path.lineTo(18f, 14f); path.close()
                c.drawPath(path, stroke)
            }
            "pen" -> {
                path.moveTo(12f, 3f); path.lineTo(18f, 13f); path.lineTo(15f, 20f); path.lineTo(9f, 20f)
                path.lineTo(6f, 13f); path.close()
                c.drawPath(path, stroke)
                c.drawLine(12f, 3f, 12f, 11f, stroke)
                c.drawCircle(12f, 12.5f, 1.5f, fill)
                c.drawLine(8f, 22f, 16f, 22f, stroke)
            }
            "pencil" -> {
                path.moveTo(4f, 20f); path.lineTo(5f, 15f); path.lineTo(16f, 4f); path.lineTo(20f, 8f)
                path.lineTo(9f, 19f); path.close()
                c.drawPath(path, stroke)
                c.drawLine(14f, 6f, 18f, 10f, stroke)
            }
            "brush" -> {
                path.moveTo(20f, 3f); path.lineTo(11f, 13f)
                c.drawPath(path, stroke)
                path.reset()
                path.moveTo(10f, 13f); path.cubicTo(6f, 13f, 7f, 18f, 4f, 20f)
                path.cubicTo(9f, 21f, 13f, 19f, 12.5f, 15f); path.close()
                c.drawPath(path, fill)
            }
            "rect" -> { r.set(4f, 6f, 20f, 18f); c.drawRect(r, stroke) }
            "ellipse" -> { r.set(3.5f, 5.5f, 20.5f, 18.5f); c.drawOval(r, stroke) }
            "star" -> { starPath(path, 12f, 12.5f, 9f, 4f, 5); c.drawPath(path, stroke) }
            "line" -> c.drawLine(5f, 19f, 19f, 5f, stroke)
            "text" -> {
                c.drawLine(5f, 5f, 19f, 5f, stroke); c.drawLine(12f, 5f, 12f, 20f, stroke)
                c.drawLine(9f, 20f, 15f, 20f, stroke)
            }
            "eraser" -> {
                path.moveTo(8f, 20f); path.lineTo(3.5f, 15.5f); path.lineTo(14f, 5f); path.lineTo(20.5f, 11.5f)
                path.lineTo(12f, 20f); path.close()
                c.drawPath(path, stroke)
                c.drawLine(8.5f, 10.5f, 15f, 17f, stroke)
                c.drawLine(8f, 20f, 20f, 20f, stroke)
            }
            "eyedropper" -> {
                c.drawLine(4f, 20f, 13f, 11f, stroke)
                path.moveTo(11f, 9f); path.lineTo(15f, 13f)
                c.drawPath(path, stroke)
                r.set(14f, 3f, 21f, 10f); c.drawOval(r, fill)
                c.drawLine(11f, 9f, 15.5f, 8.5f, stroke)
            }
            "hand" -> {
                path.moveTo(8f, 13f); path.lineTo(8f, 5.5f); path.moveTo(11f, 11f); path.lineTo(11f, 4f)
                path.moveTo(14f, 11f); path.lineTo(14f, 5f); path.moveTo(17f, 12f); path.lineTo(17f, 7f)
                c.drawPath(path, stroke)
                path.reset()
                path.moveTo(17f, 12f); path.lineTo(17f, 15f); path.cubicTo(17f, 19f, 14f, 21f, 11f, 21f)
                path.cubicTo(8f, 21f, 6f, 18f, 4.5f, 14f); path.lineTo(8f, 13f)
                c.drawPath(path, stroke)
            }
            "undo" -> {
                path.moveTo(9f, 5f); path.lineTo(4f, 10f); path.lineTo(9f, 15f)
                path.moveTo(4f, 10f); path.lineTo(14f, 10f); path.cubicTo(18f, 10f, 20f, 13f, 20f, 15.5f)
                path.cubicTo(20f, 18f, 18f, 20f, 14f, 20f); path.lineTo(10f, 20f)
                c.drawPath(path, stroke)
            }
            "redo" -> {
                c.scale(-1f, 1f, 12f, 12f)
                draw(c, "undo", 12f, 12f, 24f, color)
            }
            "layers" -> {
                path.moveTo(12f, 4f); path.lineTo(21f, 9f); path.lineTo(12f, 14f); path.lineTo(3f, 9f); path.close()
                c.drawPath(path, stroke)
                path.reset(); path.moveTo(3f, 13f); path.lineTo(12f, 18f); path.lineTo(21f, 13f)
                c.drawPath(path, stroke)
                path.reset(); path.moveTo(3f, 17f); path.lineTo(12f, 22f); path.lineTo(21f, 17f)
                c.drawPath(path, stroke)
            }
            "menu" -> { c.drawCircle(12f, 5f, 1.8f, fill); c.drawCircle(12f, 12f, 1.8f, fill); c.drawCircle(12f, 19f, 1.8f, fill) }
            "back" -> {
                path.moveTo(15f, 5f); path.lineTo(8f, 12f); path.lineTo(15f, 19f)
                c.drawPath(path, stroke)
            }
            "plus" -> { c.drawLine(12f, 5f, 12f, 19f, stroke); c.drawLine(5f, 12f, 19f, 12f, stroke) }
            "trash" -> {
                c.drawLine(4f, 6f, 20f, 6f, stroke)
                path.moveTo(6f, 6f); path.lineTo(7f, 21f); path.lineTo(17f, 21f); path.lineTo(18f, 6f)
                path.moveTo(9f, 6f); path.lineTo(9f, 3f); path.lineTo(15f, 3f); path.lineTo(15f, 6f)
                c.drawPath(path, stroke)
                c.drawLine(10f, 10f, 10f, 17f, stroke); c.drawLine(14f, 10f, 14f, 17f, stroke)
            }
            "eye" -> {
                path.moveTo(2f, 12f); path.cubicTo(6f, 5f, 18f, 5f, 22f, 12f); path.cubicTo(18f, 19f, 6f, 19f, 2f, 12f)
                c.drawPath(path, stroke)
                c.drawCircle(12f, 12f, 3f, fill)
            }
            "eye_off" -> {
                path.moveTo(2f, 12f); path.cubicTo(6f, 5f, 18f, 5f, 22f, 12f); path.cubicTo(18f, 19f, 6f, 19f, 2f, 12f)
                c.drawPath(path, stroke)
                c.drawLine(4f, 4f, 20f, 20f, stroke)
            }
            "lock" -> {
                r.set(5f, 11f, 19f, 21f); c.drawRoundRect(r, 2f, 2f, stroke)
                path.moveTo(8f, 11f); path.lineTo(8f, 8f); path.cubicTo(8f, 2.5f, 16f, 2.5f, 16f, 8f); path.lineTo(16f, 11f)
                c.drawPath(path, stroke)
            }
            "unlock" -> {
                r.set(5f, 11f, 19f, 21f); c.drawRoundRect(r, 2f, 2f, stroke)
                path.moveTo(8f, 11f); path.lineTo(8f, 8f); path.cubicTo(8f, 2.5f, 16f, 2.5f, 16f, 6f)
                c.drawPath(path, stroke)
            }
            "up" -> {
                path.moveTo(6f, 14f); path.lineTo(12f, 8f); path.lineTo(18f, 14f)
                c.drawPath(path, stroke)
            }
            "down" -> {
                path.moveTo(6f, 10f); path.lineTo(12f, 16f); path.lineTo(18f, 10f)
                c.drawPath(path, stroke)
            }
            "copy" -> {
                r.set(8f, 8f, 20f, 20f); c.drawRoundRect(r, 2f, 2f, stroke)
                path.moveTo(16f, 5f); path.lineTo(16f, 4f); path.lineTo(4f, 4f); path.lineTo(4f, 16f); path.lineTo(5f, 16f)
                c.drawPath(path, stroke)
            }
            "move" -> {
                c.drawLine(12f, 3f, 12f, 21f, stroke); c.drawLine(3f, 12f, 21f, 12f, stroke)
                path.moveTo(9f, 6f); path.lineTo(12f, 3f); path.lineTo(15f, 6f)
                path.moveTo(9f, 18f); path.lineTo(12f, 21f); path.lineTo(15f, 18f)
                path.moveTo(6f, 9f); path.lineTo(3f, 12f); path.lineTo(6f, 15f)
                path.moveTo(18f, 9f); path.lineTo(21f, 12f); path.lineTo(18f, 15f)
                c.drawPath(path, stroke)
            }
            "smudge" -> {
                path.moveTo(9f, 21f); path.lineTo(9f, 10f); path.cubicTo(9f, 8f, 12f, 8f, 12f, 10f); path.lineTo(12f, 3.5f)
                path.cubicTo(12f, 1.8f, 15f, 1.8f, 15f, 3.5f); path.lineTo(15f, 12f)
                path.cubicTo(17f, 11f, 19f, 12f, 18f, 15f); path.lineTo(16f, 21f)
                c.drawPath(path, stroke)
            }
            "clone" -> {
                r.set(6f, 12f, 18f, 17f); c.drawRect(r, stroke)
                c.drawLine(4f, 20f, 20f, 20f, stroke)
                path.moveTo(10f, 12f); path.lineTo(10f, 8f); path.moveTo(14f, 12f); path.lineTo(14f, 8f)
                c.drawPath(path, stroke)
                c.drawCircle(12f, 5.5f, 3f, stroke)
            }
            "lasso" -> {
                r.set(3f, 3f, 21f, 15f); c.drawOval(r, stroke)
                path.moveTo(7f, 13.5f); path.cubicTo(4f, 17f, 8f, 20f, 10f, 17f); path.cubicTo(11f, 20f, 9f, 21f, 8f, 22f)
                c.drawPath(path, stroke)
            }
            "marquee" -> {
                stroke.pathEffect = android.graphics.DashPathEffect(floatArrayOf(2.5f, 2.5f), 0f)
                r.set(4f, 5f, 20f, 19f); c.drawRect(r, stroke)
            }
            "gradient" -> {
                r.set(3f, 5f, 21f, 19f)
                val g = Paint(Paint.ANTI_ALIAS_FLAG)
                g.shader = android.graphics.LinearGradient(3f, 0f, 21f, 0f, color, color and 0x00FFFFFF, android.graphics.Shader.TileMode.CLAMP)
                c.drawRect(r, g); c.drawRect(r, stroke)
            }
            "bucket" -> {
                path.moveTo(11f, 3f); path.lineTo(19f, 11f); path.lineTo(11f, 19f); path.lineTo(3f, 11f); path.close()
                c.drawPath(path, stroke)
                c.drawLine(3f, 11f, 19f, 11f, stroke)
                path.reset(); path.moveTo(20f, 14f); path.cubicTo(22f, 17f, 22f, 19f, 20f, 19f); path.cubicTo(18f, 19f, 18f, 17f, 20f, 14f)
                c.drawPath(path, fill)
            }
            "adjust" -> {
                c.drawCircle(12f, 12f, 9f, stroke)
                path.moveTo(12f, 3f); path.lineTo(12f, 21f)
                r.set(3f, 3f, 21f, 21f); path.arcTo(r, 90f, 180f)
                c.drawPath(path, fill)
            }
            "filter" -> {
                c.drawCircle(9f, 9f, 5.5f, stroke); c.drawCircle(15f, 9f, 5.5f, stroke); c.drawCircle(12f, 15f, 5.5f, stroke)
            }
            "export" -> {
                c.drawLine(12f, 3f, 12f, 15f, stroke)
                path.moveTo(7f, 8f); path.lineTo(12f, 3f); path.lineTo(17f, 8f)
                path.moveTo(4f, 14f); path.lineTo(4f, 20f); path.lineTo(20f, 20f); path.lineTo(20f, 14f)
                c.drawPath(path, stroke)
            }
            "image" -> {
                r.set(3f, 4f, 21f, 20f); c.drawRoundRect(r, 2f, 2f, stroke)
                c.drawCircle(8.5f, 9f, 1.8f, fill)
                path.moveTo(3.5f, 18f); path.lineTo(9f, 13f); path.lineTo(13f, 16.5f); path.lineTo(16f, 13.5f); path.lineTo(20.5f, 18f)
                c.drawPath(path, stroke)
            }
            "wand" -> {
                c.drawLine(4f, 20f, 15f, 9f, stroke)
                path.moveTo(17f, 2f); path.lineTo(17f, 6f); path.moveTo(15f, 4f); path.lineTo(19f, 4f)
                path.moveTo(21f, 8f); path.lineTo(21f, 11f); path.moveTo(19.5f, 9.5f); path.lineTo(22.5f, 9.5f)
                path.moveTo(11f, 3f); path.lineTo(11f, 5f); path.moveTo(10f, 4f); path.lineTo(12f, 4f)
                c.drawPath(path, stroke)
            }
            "check" -> {
                path.moveTo(4.5f, 12.5f); path.lineTo(9.5f, 17.5f); path.lineTo(19.5f, 6.5f)
                c.drawPath(path, stroke)
            }
            "close" -> { c.drawLine(6f, 6f, 18f, 18f, stroke); c.drawLine(18f, 6f, 6f, 18f, stroke) }
            "merge" -> {
                path.moveTo(6f, 3f); path.lineTo(6f, 9f); path.cubicTo(6f, 13f, 12f, 12f, 12f, 16f); path.lineTo(12f, 21f)
                path.moveTo(18f, 3f); path.lineTo(18f, 9f); path.cubicTo(18f, 13f, 12f, 12f, 12f, 16f)
                path.moveTo(9f, 18f); path.lineTo(12f, 21f); path.lineTo(15f, 18f)
                c.drawPath(path, stroke)
            }
            "mask" -> {
                r.set(3f, 4f, 21f, 20f); c.drawRect(r, stroke)
                c.drawCircle(12f, 12f, 5f, fill)
            }
            "flip_h" -> {
                c.drawLine(12f, 3f, 12f, 21f, stroke)
                path.moveTo(9f, 6f); path.lineTo(3f, 18f); path.lineTo(9f, 18f); path.close()
                c.drawPath(path, fill)
                path.reset(); path.moveTo(15f, 6f); path.lineTo(21f, 18f); path.lineTo(15f, 18f); path.close()
                c.drawPath(path, stroke)
            }
            "flip_v" -> {
                c.rotate(90f, 12f, 12f)
                draw(c, "flip_h", 12f, 12f, 24f, color)
            }
            "rotate" -> {
                r.set(4f, 4f, 20f, 20f)
                path.addArc(r, -60f, 300f)
                c.drawPath(path, stroke)
                path.reset(); path.moveTo(16f, 2.5f); path.lineTo(16.5f, 6.5f); path.lineTo(12.5f, 7f)
                c.drawPath(path, stroke)
            }
            "vector" -> {
                path.moveTo(4f, 19f); path.cubicTo(8f, 4f, 16f, 20f, 20f, 5f)
                c.drawPath(path, stroke)
                r.set(2f, 17f, 6f, 21f); c.drawRect(r, fill); r.set(18f, 3f, 22f, 7f); c.drawRect(r, fill)
            }
            "photo" -> draw(c, "image", 12f, 12f, 24f, color)
            "send" -> {
                path.moveTo(3f, 12f); path.lineTo(20f, 12f); path.moveTo(14f, 6f); path.lineTo(20f, 12f); path.lineTo(14f, 18f)
                c.drawPath(path, stroke)
            }
            "arrange" -> {
                r.set(3f, 3f, 14f, 14f); c.drawRect(r, stroke)
                r.set(10f, 10f, 21f, 21f); c.drawRect(r, fill)
            }
            "align" -> {
                c.drawLine(12f, 2f, 12f, 22f, stroke)
                r.set(6f, 5f, 18f, 10f); c.drawRect(r, stroke)
                r.set(4f, 14f, 20f, 19f); c.drawRect(r, stroke)
            }
            "crop" -> {
                path.moveTo(6f, 2f); path.lineTo(6f, 18f); path.lineTo(22f, 18f)
                path.moveTo(2f, 6f); path.lineTo(18f, 6f); path.lineTo(18f, 22f)
                c.drawPath(path, stroke)
            }
            "palette" -> {
                path.moveTo(12f, 3f); path.cubicTo(4f, 3f, 2f, 10f, 3.5f, 14f); path.cubicTo(5f, 18f, 9f, 16f, 10f, 18f)
                path.cubicTo(11f, 21f, 13f, 21f, 15f, 20.5f); path.cubicTo(20f, 19f, 22f, 14f, 21f, 10f); path.cubicTo(20f, 6f, 17f, 3f, 12f, 3f)
                c.drawPath(path, stroke)
                c.drawCircle(8f, 9f, 1.5f, fill); c.drawCircle(13f, 7f, 1.5f, fill); c.drawCircle(17f, 11f, 1.5f, fill)
            }
            "settings" -> {
                c.drawCircle(12f, 12f, 3f, stroke)
                for (i in 0 until 8) {
                    c.save(); c.rotate(i * 45f, 12f, 12f); c.drawLine(12f, 3f, 12f, 6f, stroke); c.restore()
                }
                c.drawCircle(12f, 12f, 6.5f, stroke)
            }
            "save" -> {
                path.moveTo(4f, 4f); path.lineTo(17f, 4f); path.lineTo(20f, 7f); path.lineTo(20f, 20f); path.lineTo(4f, 20f); path.close()
                c.drawPath(path, stroke)
                r.set(8f, 4f, 15f, 9f); c.drawRect(r, stroke)
                r.set(7f, 13f, 17f, 20f); c.drawRect(r, stroke)
            }
            "node" -> {
                path.moveTo(3f, 18f); path.cubicTo(7f, 6f, 17f, 6f, 21f, 18f)
                c.drawPath(path, stroke)
                r.set(9.5f, 6.5f, 14.5f, 11.5f); c.drawRect(r, fill)
                c.drawLine(5f, 9f, 19f, 9f, stroke)
            }
            else -> c.drawCircle(12f, 12f, 6f, stroke)
        }
        c.restore()
    }

    fun starPath(p: Path, cx: Float, cy: Float, outer: Float, inner: Float, points: Int) {
        p.reset()
        val n = points * 2
        for (i in 0 until n) {
            val rad = if (i % 2 == 0) outer else inner
            val a = Math.PI * 2 * i / n - Math.PI / 2
            val x = cx + (rad * Math.cos(a)).toFloat()
            val y = cy + (rad * Math.sin(a)).toFloat()
            if (i == 0) p.moveTo(x, y) else p.lineTo(x, y)
        }
        p.close()
    }
}
