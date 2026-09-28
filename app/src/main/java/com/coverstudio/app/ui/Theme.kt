package com.coverstudio.app.ui

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.View

object Theme {
    const val BG = 0xFF17181B.toInt()
    const val PASTEBOARD = 0xFF222328.toInt()
    const val PANEL = 0xFF26282D.toInt()
    const val PANEL_2 = 0xFF30333A.toInt()
    const val DIVIDER = 0xFF3A3D45.toInt()
    const val TEXT = 0xFFE8E9EC.toInt()
    const val TEXT_DIM = 0xFF9A9DA6.toInt()
    const val ACCENT = 0xFF4F9DFF.toInt()
    const val ACCENT_DIM = 0x334F9DFF
    const val PHOTO_ACCENT = 0xFFFF8A4F.toInt()
    const val DANGER = 0xFFFF5A5A.toInt()
    const val SELECTION = 0xFF2F8CFF.toInt()

    /** A palette of colours that tend to be handy for album artwork. */
    val SWATCHES = intArrayOf(
        Color.BLACK, Color.WHITE, 0xFF7F7F7F.toInt(), 0xFFE63946.toInt(), 0xFFF4A261.toInt(),
        0xFFE9C46A.toInt(), 0xFF2A9D8F.toInt(), 0xFF264653.toInt(), 0xFF3A86FF.toInt(),
        0xFF8338EC.toInt(), 0xFFFF006E.toInt(), 0xFFFB5607.toInt(), 0xFFFFBE0B.toInt(),
        0xFF06D6A0.toInt(), 0xFF118AB2.toInt(), 0xFF073B4C.toInt(), 0xFFEDE0D4.toInt(),
        0xFF9C6644.toInt(), 0xFF1D3557.toInt(), 0xFFA8DADC.toInt()
    )
}

fun Context.dp(v: Float): Float = v * resources.displayMetrics.density
fun Context.dp(v: Int): Int = (v * resources.displayMetrics.density + 0.5f).toInt()
fun View.dp(v: Float): Float = context.dp(v)
fun View.dp(v: Int): Int = context.dp(v)

fun roundRect(color: Int, radius: Float, strokeColor: Int = 0, strokeWidth: Int = 0): GradientDrawable =
    GradientDrawable().apply {
        setColor(color)
        cornerRadius = radius
        if (strokeWidth > 0) setStroke(strokeWidth, strokeColor)
    }

fun colorToHex(c: Int): String =
    if (Color.alpha(c) == 255) String.format("#%06X", c and 0xFFFFFF)
    else String.format("#%08X", c)

fun parseHex(s: String): Int? {
    var t = s.trim().removePrefix("#")
    if (t.length == 3) t = t.map { "$it$it" }.joinToString("")
    return try {
        when (t.length) {
            6 -> (0xFF000000.toInt()) or t.toLong(16).toInt()
            8 -> t.toLong(16).toInt()
            else -> null
        }
    } catch (e: NumberFormatException) {
        null
    }
}
