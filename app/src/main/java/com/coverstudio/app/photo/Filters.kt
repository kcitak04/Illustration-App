package com.coverstudio.app.photo

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorMatrix
import android.graphics.ColorMatrixColorFilter
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffColorFilter
import android.graphics.RadialGradient
import android.graphics.Shader
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

class Param(
    val name: String,
    val min: Float,
    val max: Float,
    val def: Float,
    /** Spatial params are in document pixels and get scaled for low-res previews. */
    val spatial: Boolean = false,
    val fmt: (Float) -> String = { it.roundToInt().toString() }
)

abstract class ImageOp(val name: String, val params: List<Param>, val colorNames: List<String> = emptyList(), val defColors: List<Int> = emptyList()) {
    /** Returns a new bitmap. [scale] = preview size / document size. */
    abstract fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap
}

// ------------------------------------------------------------------ helpers

fun pixelsOf(b: Bitmap): IntArray = IntArray(b.width * b.height).also { b.getPixels(it, 0, b.width, 0, 0, b.width, b.height) }

fun bitmapOf(px: IntArray, w: Int, h: Int): Bitmap =
    Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).also { it.setPixels(px, 0, w, 0, 0, w, h) }

private fun withMatrix(src: Bitmap, cm: ColorMatrix): Bitmap {
    val out = Bitmap.createBitmap(src.width, src.height, Bitmap.Config.ARGB_8888)
    Canvas(out).drawBitmap(src, 0f, 0f, Paint().apply { colorFilter = ColorMatrixColorFilter(cm) })
    return out
}

private fun lutApply(src: Bitmap, lr: IntArray, lg: IntArray = lr, lb: IntArray = lr): Bitmap {
    val px = pixelsOf(src)
    for (i in px.indices) {
        val c = px[i]
        px[i] = (c and 0xFF000000.toInt()) or (lr[(c shr 16) and 255] shl 16) or (lg[(c shr 8) and 255] shl 8) or lb[c and 255]
    }
    return bitmapOf(px, src.width, src.height)
}

private fun clamp255(v: Int) = if (v < 0) 0 else if (v > 255) 255 else v
private fun lum(c: Int) = (((c shr 16) and 255) * 54 + ((c shr 8) and 255) * 183 + (c and 255) * 19) shr 8

/** Fast xorshift random for noise-type filters. */
class XorShift(seed: Long) {
    private var s = if (seed == 0L) 0x9E3779B97F4A7C15uL.toLong() else seed
    fun next(): Int { s = s xor (s shl 13); s = s xor (s ushr 7); s = s xor (s shl 17); return (s ushr 33).toInt() }
    fun nextFloat(): Float = (next() and 0xFFFFFF) / 16777216f
}

// ------------------------------------------------------------------ blur core (premultiplied box blur, 3 passes ≈ gaussian)

private fun premultiply(px: IntArray) {
    for (i in px.indices) {
        val c = px[i]; val a = c ushr 24
        if (a == 255) continue
        if (a == 0) { px[i] = 0; continue }
        val r = ((c shr 16) and 255) * a / 255; val g = ((c shr 8) and 255) * a / 255; val b = (c and 255) * a / 255
        px[i] = (a shl 24) or (r shl 16) or (g shl 8) or b
    }
}

private fun unpremultiply(px: IntArray) {
    for (i in px.indices) {
        val c = px[i]; val a = c ushr 24
        if (a == 255 || a == 0) continue
        val r = min(255, ((c shr 16) and 255) * 255 / a); val g = min(255, ((c shr 8) and 255) * 255 / a); val b = min(255, (c and 255) * 255 / a)
        px[i] = (a shl 24) or (r shl 16) or (g shl 8) or b
    }
}

private fun boxPass(src: IntArray, dst: IntArray, w: Int, h: Int, r: Int, horizontal: Boolean) {
    val len = if (horizontal) w else h
    val lines = if (horizontal) h else w
    val step = if (horizontal) 1 else w
    val div = 2 * r + 1
    for (line in 0 until lines) {
        val base = if (horizontal) line * w else line
        var sa = 0; var sr = 0; var sg = 0; var sb = 0
        for (i in -r..r) {
            val c = src[base + i.coerceIn(0, len - 1) * step]
            sa += c ushr 24; sr += (c shr 16) and 255; sg += (c shr 8) and 255; sb += c and 255
        }
        for (x in 0 until len) {
            dst[base + x * step] = ((sa / div) shl 24) or ((sr / div) shl 16) or ((sg / div) shl 8) or (sb / div)
            val o = src[base + (x - r).coerceIn(0, len - 1) * step]
            val n = src[base + (x + r + 1).coerceIn(0, len - 1) * step]
            sa += (n ushr 24) - (o ushr 24)
            sr += ((n shr 16) and 255) - ((o shr 16) and 255)
            sg += ((n shr 8) and 255) - ((o shr 8) and 255)
            sb += (n and 255) - (o and 255)
        }
    }
}

/** Blurs unpremultiplied pixels in place. */
fun blurPixels(px: IntArray, w: Int, h: Int, radius: Float) {
    val r = radius.roundToInt()
    if (r < 1) return
    premultiply(px)
    val tmp = IntArray(px.size)
    repeat(3) {
        boxPass(px, tmp, w, h, r, true)
        boxPass(tmp, px, w, h, r, false)
    }
    unpremultiply(px)
}

// ------------------------------------------------------------------ adjustments

object BrightnessContrast : ImageOp("Brightness / Contrast", listOf(Param("Brightness", -100f, 100f, 0f), Param("Contrast", -100f, 100f, 0f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val f = Math.tan((v[1] / 100f + 1) * PI / 4).toFloat().coerceIn(0f, 20f)
        val t = (1 - f) * 128f + v[0] * 1.5f
        return withMatrix(src, ColorMatrix(floatArrayOf(f, 0f, 0f, 0f, t, 0f, f, 0f, 0f, t, 0f, 0f, f, 0f, t, 0f, 0f, 0f, 1f, 0f)))
    }
}

object HueSaturation : ImageOp("Hue / Saturation", listOf(Param("Hue", -180f, 180f, 0f), Param("Saturation", -100f, 100f, 0f), Param("Lightness", -100f, 100f, 0f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val a = Math.toRadians(v[0].toDouble()); val c = cos(a).toFloat(); val s = sin(a).toFloat()
        val hue = ColorMatrix(floatArrayOf(
            0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f, 0f, 0f,
            0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f, 0f, 0f,
            0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f, 0f, 0f,
            0f, 0f, 0f, 1f, 0f))
        val sat = ColorMatrix(); sat.setSaturation(1f + v[1] / 100f)
        hue.postConcat(sat)
        val l = v[2] / 100f
        val light = if (l >= 0) ColorMatrix(floatArrayOf(1 - l, 0f, 0f, 0f, 255 * l, 0f, 1 - l, 0f, 0f, 255 * l, 0f, 0f, 1 - l, 0f, 255 * l, 0f, 0f, 0f, 1f, 0f))
        else ColorMatrix(floatArrayOf(1 + l, 0f, 0f, 0f, 0f, 0f, 1 + l, 0f, 0f, 0f, 0f, 0f, 1 + l, 0f, 0f, 0f, 0f, 0f, 1f, 0f))
        hue.postConcat(light)
        return withMatrix(src, hue)
    }
}

object ExposureTemp : ImageOp("Exposure / Temperature", listOf(
    Param("Exposure", -3f, 3f, 0f, fmt = { String.format("%.2f", it) }), Param("Temperature", -100f, 100f, 0f), Param("Tint", -100f, 100f, 0f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val e = 2f.pow(v[0]); val t = v[1] / 100f * 0.35f; val ti = v[2] / 100f * 0.3f
        return withMatrix(src, ColorMatrix(floatArrayOf(
            e * (1 + t), 0f, 0f, 0f, 0f, 0f, e * (1 - ti), 0f, 0f, 0f, 0f, 0f, e * (1 - t), 0f, 0f, 0f, 0f, 0f, 1f, 0f)))
    }
}

object Levels : ImageOp("Levels", listOf(
    Param("Input black", 0f, 250f, 0f), Param("Input white", 5f, 255f, 255f), Param("Gamma", 0.1f, 3f, 1f, fmt = { String.format("%.2f", it) }),
    Param("Output black", 0f, 255f, 0f), Param("Output white", 0f, 255f, 255f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val lut = IntArray(256)
        val ib = v[0]; val iw = max(ib + 1, v[1])
        for (i in 0 until 256) {
            val x = ((i - ib) / (iw - ib)).coerceIn(0f, 1f).pow(1f / v[2])
            lut[i] = clamp255((v[3] + x * (v[4] - v[3])).roundToInt())
        }
        return lutApply(src, lut)
    }
}

object Vibrance : ImageOp("Vibrance", listOf(Param("Vibrance", -100f, 100f, 30f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val amt = v[0] / 100f
        val px = pixelsOf(src)
        for (i in px.indices) {
            val c = px[i]; val r = (c shr 16) and 255; val g = (c shr 8) and 255; val b = c and 255
            val mx = max(r, max(g, b)); val avg = (r + g + b) / 3f
            val k = amt * (1f - (mx - avg) / 128f).coerceIn(0f, 1f) * 1.5f
            px[i] = (c and 0xFF000000.toInt()) or (clamp255((r + (r - avg) * k).roundToInt()) shl 16) or
                (clamp255((g + (g - avg) * k).roundToInt()) shl 8) or clamp255((b + (b - avg) * k).roundToInt())
        }
        return bitmapOf(px, src.width, src.height)
    }
}

object Invert : ImageOp("Invert", emptyList()) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float) =
        withMatrix(src, ColorMatrix(floatArrayOf(-1f, 0f, 0f, 0f, 255f, 0f, -1f, 0f, 0f, 255f, 0f, 0f, -1f, 0f, 255f, 0f, 0f, 0f, 1f, 0f)))
}

object BlackWhite : ImageOp("Black & White", listOf(Param("Contrast", -100f, 100f, 0f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val cm = ColorMatrix(); cm.setSaturation(0f)
        val f = Math.tan((v[0] / 100f + 1) * PI / 4).toFloat(); val t = (1 - f) * 128f
        cm.postConcat(ColorMatrix(floatArrayOf(f, 0f, 0f, 0f, t, 0f, f, 0f, 0f, t, 0f, 0f, f, 0f, t, 0f, 0f, 0f, 1f, 0f)))
        return withMatrix(src, cm)
    }
}

object Sepia : ImageOp("Sepia", listOf(Param("Amount", 0f, 100f, 100f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val a = v[0] / 100f
        fun m(s: Float, i: Float) = i + (s - i) * a
        return withMatrix(src, ColorMatrix(floatArrayOf(
            m(0.393f, 1f), m(0.769f, 0f), m(0.189f, 0f), 0f, 0f,
            m(0.349f, 0f), m(0.686f, 1f), m(0.168f, 0f), 0f, 0f,
            m(0.272f, 0f), m(0.534f, 0f), m(0.131f, 1f), 0f, 0f,
            0f, 0f, 0f, 1f, 0f)))
    }
}

object Posterize : ImageOp("Posterize", listOf(Param("Levels", 2f, 16f, 4f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val n = v[0].roundToInt().coerceAtLeast(2)
        val lut = IntArray(256) { ((it * n / 256) * 255 / (n - 1)).coerceIn(0, 255) }
        return lutApply(src, lut)
    }
}

object Threshold : ImageOp("Threshold", listOf(Param("Level", 1f, 255f, 128f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val t = v[0].roundToInt(); val px = pixelsOf(src)
        for (i in px.indices) { val c = px[i]; px[i] = (c and 0xFF000000.toInt()) or (if (lum(c) >= t) 0xFFFFFF else 0) }
        return bitmapOf(px, src.width, src.height)
    }
}

object GradientMap : ImageOp("Gradient Map / Duotone", listOf(Param("Mix", 0f, 100f, 100f), Param("Midpoint", 10f, 90f, 50f)),
    listOf("Shadows", "Highlights"), listOf(0xFF1D1045.toInt(), 0xFFFF7A59.toInt())) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val mix = v[0] / 100f; val mid = v[1] / 100f
        val g = 1f / (Math.log(mid.toDouble()) / Math.log(0.5)).toFloat()
        val lut = IntArray(256) {
            val t = (it / 255f).pow(1f / g)
            val a = colors[0]; val b = colors[1]
            Color.rgb(((Color.red(a) + (Color.red(b) - Color.red(a)) * t)).roundToInt(),
                ((Color.green(a) + (Color.green(b) - Color.green(a)) * t)).roundToInt(),
                ((Color.blue(a) + (Color.blue(b) - Color.blue(a)) * t)).roundToInt())
        }
        val px = pixelsOf(src)
        for (i in px.indices) {
            val c = px[i]; val m = lut[lum(c)]
            val r = (((c shr 16) and 255) + ((((m shr 16) and 255) - ((c shr 16) and 255)) * mix)).roundToInt()
            val gg = (((c shr 8) and 255) + ((((m shr 8) and 255) - ((c shr 8) and 255)) * mix)).roundToInt()
            val b = ((c and 255) + (((m and 255) - (c and 255)) * mix)).roundToInt()
            px[i] = (c and 0xFF000000.toInt()) or (r shl 16) or (gg shl 8) or b
        }
        return bitmapOf(px, src.width, src.height)
    }
}

object ColorOverlay : ImageOp("Color Fill Overlay", listOf(Param("Opacity", 0f, 100f, 50f)), listOf("Color"), listOf(0xFFFF006E.toInt())) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val out = src.copy(Bitmap.Config.ARGB_8888, true)
        val c = Canvas(out)
        val col = colors[0]
        c.drawBitmap(src, 0f, 0f, Paint().apply {
            colorFilter = PorterDuffColorFilter(col, PorterDuff.Mode.SRC_IN)
            alpha = (v[0] / 100f * 255).toInt()
        })
        return out
    }
}

// ------------------------------------------------------------------ filters

object GaussianBlur : ImageOp("Gaussian Blur", listOf(Param("Radius", 0f, 200f, 12f, spatial = true))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val px = pixelsOf(src); blurPixels(px, src.width, src.height, v[0] * scale); return bitmapOf(px, src.width, src.height)
    }
}

object MotionBlur : ImageOp("Motion Blur", listOf(Param("Angle", -180f, 180f, 0f), Param("Distance", 0f, 600f, 60f, spatial = true))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val d = v[1] * scale; val a = Math.toRadians(v[0].toDouble())
        val out = Bitmap.createBitmap(src.width, src.height, Bitmap.Config.ARGB_8888)
        val c = Canvas(out)
        val n = d.roundToInt().coerceIn(2, 48)
        val p = Paint(Paint.FILTER_BITMAP_FLAG)
        for (i in 0 until n) {
            val t = -0.5f + i.toFloat() / (n - 1)
            p.alpha = (255f / (i + 1)).roundToInt().coerceAtLeast(1)
            c.drawBitmap(src, (cos(a) * d * t).toFloat(), (sin(a) * d * t).toFloat(), p)
        }
        return out
    }
}

object ZoomBlur : ImageOp("Zoom Blur", listOf(Param("Strength", 0f, 100f, 25f), Param("Center X %", 0f, 100f, 50f), Param("Center Y %", 0f, 100f, 50f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val out = Bitmap.createBitmap(src.width, src.height, Bitmap.Config.ARGB_8888)
        val c = Canvas(out)
        val cx = src.width * v[1] / 100f; val cy = src.height * v[2] / 100f
        val s = v[0] / 100f * 0.5f
        val n = 32
        val p = Paint(Paint.FILTER_BITMAP_FLAG)
        for (i in 0 until n) {
            val k = 1f + s * i / (n - 1)
            p.alpha = (255f / (i + 1)).roundToInt().coerceAtLeast(1)
            c.save(); c.scale(k, k, cx, cy); c.drawBitmap(src, 0f, 0f, p); c.restore()
        }
        return out
    }
}

object Sharpen : ImageOp("Sharpen (Unsharp Mask)", listOf(Param("Amount", 0f, 300f, 80f), Param("Radius", 0.5f, 20f, 2f, spatial = true, fmt = { String.format("%.1f", it) }))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val px = pixelsOf(src); val bl = px.copyOf()
        blurPixels(bl, src.width, src.height, max(1f, v[1] * scale))
        val k = v[0] / 100f
        for (i in px.indices) {
            val c = px[i]; val b = bl[i]
            val r = clamp255((((c shr 16) and 255) + (((c shr 16) and 255) - ((b shr 16) and 255)) * k).roundToInt())
            val g = clamp255((((c shr 8) and 255) + (((c shr 8) and 255) - ((b shr 8) and 255)) * k).roundToInt())
            val bb = clamp255(((c and 255) + ((c and 255) - (b and 255)) * k).roundToInt())
            px[i] = (c and 0xFF000000.toInt()) or (r shl 16) or (g shl 8) or bb
        }
        return bitmapOf(px, src.width, src.height)
    }
}

object Noise : ImageOp("Film Grain / Noise", listOf(Param("Amount", 0f, 100f, 20f), Param("Monochrome (0/1)", 0f, 1f, 1f), Param("Seed", 0f, 100f, 7f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val px = pixelsOf(src); val rnd = XorShift(v[2].toLong() * 7919 + 17)
        val amt = v[0] * 1.6f; val mono = v[1] >= 0.5f
        for (i in px.indices) {
            val c = px[i]
            val n0 = ((rnd.nextFloat() + rnd.nextFloat() + rnd.nextFloat()) / 1.5f - 1f) * amt
            val n1 = if (mono) n0 else ((rnd.nextFloat() + rnd.nextFloat() + rnd.nextFloat()) / 1.5f - 1f) * amt
            val n2 = if (mono) n0 else ((rnd.nextFloat() + rnd.nextFloat() + rnd.nextFloat()) / 1.5f - 1f) * amt
            px[i] = (c and 0xFF000000.toInt()) or (clamp255((((c shr 16) and 255) + n0).toInt()) shl 16) or
                (clamp255((((c shr 8) and 255) + n1).toInt()) shl 8) or clamp255(((c and 255) + n2).toInt())
        }
        return bitmapOf(px, src.width, src.height)
    }
}

object Pixelate : ImageOp("Pixelate", listOf(Param("Cell size", 2f, 200f, 24f, spatial = true))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val cell = max(1f, v[0] * scale)
        val sw = max(1, (src.width / cell).roundToInt()); val sh = max(1, (src.height / cell).roundToInt())
        val small = Bitmap.createScaledBitmap(src, sw, sh, true)
        val out = Bitmap.createBitmap(src.width, src.height, Bitmap.Config.ARGB_8888)
        Canvas(out).drawBitmap(small, null, android.graphics.Rect(0, 0, src.width, src.height), Paint())
        return out
    }
}

object Vignette : ImageOp("Vignette", listOf(Param("Amount", 0f, 100f, 60f), Param("Size", 0f, 100f, 50f), Param("Softness", 1f, 100f, 60f)), listOf("Color"), listOf(Color.BLACK)) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val out = src.copy(Bitmap.Config.ARGB_8888, true)
        val c = Canvas(out)
        val w = src.width.toFloat(); val h = src.height.toFloat()
        val r = hypot(w, h) / 2f
        val inner = (0.2f + v[1] / 100f * 0.6f).coerceIn(0f, 0.95f)
        val outer = (inner + v[2] / 100f * (1f - inner)).coerceIn(inner + 0.01f, 1f)
        val col = colors[0] and 0xFFFFFF
        val a = (v[0] / 100f * 255).toInt()
        val p = Paint(Paint.ANTI_ALIAS_FLAG)
        p.shader = RadialGradient(w / 2, h / 2, r, intArrayOf(col, col, (a shl 24) or col), floatArrayOf(0f, inner, outer), Shader.TileMode.CLAMP)
        p.xfermode = android.graphics.PorterDuffXfermode(PorterDuff.Mode.SRC_ATOP)
        c.drawRect(0f, 0f, w, h, p)
        return out
    }
}

object ChromaticAberration : ImageOp("Chromatic Aberration", listOf(Param("Amount", 0f, 80f, 12f, spatial = true), Param("Radial (0/1)", 0f, 1f, 1f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val w = src.width; val h = src.height
        val px = pixelsOf(src); val out = IntArray(px.size)
        val amt = v[0] * scale; val radial = v[1] >= 0.5f
        val cx = w / 2f; val cy = h / 2f; val maxR = hypot(cx, cy)
        for (y in 0 until h) for (x in 0 until w) {
            val i = y * w + x
            val rx: Int; val ry: Int; val bx: Int; val by: Int
            if (radial) {
                val dx = x - cx; val dy = y - cy
                val k = amt / maxR
                rx = (cx + dx * (1 + k)).toInt().coerceIn(0, w - 1); ry = (cy + dy * (1 + k)).toInt().coerceIn(0, h - 1)
                bx = (cx + dx * (1 - k)).toInt().coerceIn(0, w - 1); by = (cy + dy * (1 - k)).toInt().coerceIn(0, h - 1)
            } else {
                rx = (x + amt).toInt().coerceIn(0, w - 1); ry = y; bx = (x - amt).toInt().coerceIn(0, w - 1); by = y
            }
            val rc = px[ry * w + rx]; val bc = px[by * w + bx]; val c = px[i]
            val a = max(c ushr 24, max(rc ushr 24, bc ushr 24))
            out[i] = (a shl 24) or (rc and 0xFF0000) or (c and 0xFF00) or (bc and 0xFF)
        }
        return bitmapOf(out, w, h)
    }
}

object Glitch : ImageOp("Glitch", listOf(Param("Amount", 0f, 100f, 40f), Param("Slice height", 2f, 200f, 30f, spatial = true), Param("RGB split", 0f, 60f, 10f, spatial = true), Param("Seed", 0f, 100f, 3f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val w = src.width; val h = src.height
        val px = pixelsOf(src); val out = IntArray(px.size)
        val rnd = XorShift(v[3].toLong() * 104729 + 3)
        val slice = max(1, (v[1] * scale).roundToInt())
        val split = (v[2] * scale).roundToInt()
        val amt = v[0] / 100f
        var y = 0
        while (y < h) {
            val bandH = max(1, (slice * (0.3f + rnd.nextFloat() * 1.4f)).roundToInt())
            val shift = if (rnd.nextFloat() < amt * 0.6f) ((rnd.nextFloat() * 2 - 1) * amt * w * 0.15f).roundToInt() else 0
            for (yy in y until min(h, y + bandH)) {
                val row = yy * w
                for (x in 0 until w) {
                    val sx = ((x - shift) % w + w) % w
                    val c = px[row + sx]
                    val r = px[row + ((sx + split) % w + w) % w]
                    val b = px[row + ((sx - split) % w + w) % w]
                    out[row + x] = (c and 0xFF00FF00.toInt()) or (r and 0xFF0000) or (b and 0xFF)
                }
            }
            y += bandH
        }
        return bitmapOf(out, w, h)
    }
}

object Halftone : ImageOp("Halftone", listOf(Param("Dot size", 3f, 120f, 18f, spatial = true), Param("Color dots (0/1)", 0f, 1f, 0f), Param("Angle", 0f, 90f, 45f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val w = src.width; val h = src.height
        val cell = max(2f, v[0] * scale)
        val color = v[1] >= 0.5f
        val out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val c = Canvas(out)
        c.drawColor(if (color) Color.BLACK else Color.WHITE)
        val p = Paint(Paint.ANTI_ALIAS_FLAG)
        val px = pixelsOf(src)
        val ang = Math.toRadians(v[2].toDouble()); val ca = cos(ang).toFloat(); val sa = sin(ang).toFloat()
        val diag = hypot(w.toFloat(), h.toFloat())
        val n = (diag / cell).toInt() + 2
        for (j in -n..n) for (i in -n..n) {
            // grid in rotated space, centred on the image
            val gx = i * cell; val gy = j * cell
            val x = w / 2f + gx * ca - gy * sa; val y = h / 2f + gx * sa + gy * ca
            if (x < -cell || y < -cell || x > w + cell || y > h + cell) continue
            val sxp = x.toInt().coerceIn(0, w - 1); val syp = y.toInt().coerceIn(0, h - 1)
            val col = px[syp * w + sxp]
            if ((col ushr 24) < 10) continue
            val l = lum(col) / 255f
            val rad = cell * 0.5f * 1.41f * sqrt(if (color) l.coerceAtLeast(0.15f) else 1 - l)
            p.color = if (color) col or 0xFF000000.toInt() else Color.BLACK
            c.drawCircle(x, y, rad, p)
        }
        return out
    }
}

object ShadowGlow : ImageOp("Drop Shadow / Glow", listOf(
    Param("Offset X", -200f, 200f, 20f, spatial = true), Param("Offset Y", -200f, 200f, 20f, spatial = true),
    Param("Blur", 0f, 150f, 25f, spatial = true), Param("Opacity", 0f, 100f, 70f)), listOf("Color"), listOf(Color.BLACK)) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val w = src.width; val h = src.height
        val sh = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        Canvas(sh).drawBitmap(src, v[0] * scale, v[1] * scale, Paint().apply {
            colorFilter = PorterDuffColorFilter(colors[0] or 0xFF000000.toInt(), PorterDuff.Mode.SRC_IN)
        })
        val spx = pixelsOf(sh); blurPixels(spx, w, h, v[2] * scale)
        val blurred = bitmapOf(spx, w, h)
        val out = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        val c = Canvas(out)
        c.drawBitmap(blurred, 0f, 0f, Paint().apply { alpha = (v[3] / 100f * 255).toInt() })
        c.drawBitmap(src, 0f, 0f, null)
        return out
    }
}

object Emboss : ImageOp("Emboss", listOf(Param("Strength", 0f, 100f, 60f), Param("Angle", -180f, 180f, 135f))) {
    override fun apply(src: Bitmap, v: FloatArray, colors: IntArray, scale: Float): Bitmap {
        val w = src.width; val h = src.height; val px = pixelsOf(src); val out = IntArray(px.size)
        val a = Math.toRadians(v[1].toDouble()); val dx = cos(a).roundToInt(); val dy = sin(a).roundToInt()
        val k = v[0] / 100f * 3f
        for (y in 0 until h) for (x in 0 until w) {
            val i = y * w + x
            val p1 = px[(y + dy).coerceIn(0, h - 1) * w + (x + dx).coerceIn(0, w - 1)]
            val p2 = px[(y - dy).coerceIn(0, h - 1) * w + (x - dx).coerceIn(0, w - 1)]
            val g = clamp255(128 + ((lum(p1) - lum(p2)) * k).roundToInt())
            out[i] = (px[i] and 0xFF000000.toInt()) or (g shl 16) or (g shl 8) or g
        }
        return bitmapOf(out, w, h)
    }
}

val ADJUSTMENTS: List<ImageOp> = listOf(
    BrightnessContrast, Levels, HueSaturation, Vibrance, ExposureTemp, BlackWhite, Sepia, GradientMap, ColorOverlay,
    Invert, Posterize, Threshold
)

val FILTERS: List<ImageOp> = listOf(
    GaussianBlur, MotionBlur, ZoomBlur, Sharpen, Noise, ChromaticAberration, Glitch, Halftone, Pixelate, Vignette, ShadowGlow, Emboss
)
