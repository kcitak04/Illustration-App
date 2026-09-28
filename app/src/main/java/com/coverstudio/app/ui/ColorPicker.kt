package com.coverstudio.app.ui

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ComposeShader
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.RectF
import android.graphics.Shader
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.EditText
import android.widget.GridLayout
import android.widget.LinearLayout
import android.widget.ScrollView

object RecentColors {
    val list = ArrayList<Int>()
    fun push(c: Int) {
        list.remove(c); list.add(0, c)
        while (list.size > 10) list.removeAt(list.size - 1)
    }
}

/** HSV + alpha colour picker used by both studios. */
@SuppressLint("ViewConstructor")
class ColorPickerView(context: Context, initial: Int, private val onChange: (Int) -> Unit) : View(context) {
    private val hsv = FloatArray(3)
    var alpha = Color.alpha(initial)
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val sv = RectF()
    private val hue = RectF()
    private val alp = RectF()
    private var dragging = 0

    init {
        Color.colorToHSV(initial, hsv)
    }

    val color: Int get() = Color.HSVToColor(alpha, hsv)

    fun set(c: Int) {
        Color.colorToHSV(c, hsv); alpha = Color.alpha(c); invalidate()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        setMeasuredDimension(w, (w * 0.62f).toInt() + dp(80))
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        val pad = dp(8f)
        sv.set(pad, pad, w - pad, pad + w * 0.62f)
        hue.set(pad, sv.bottom + dp(12f), w - pad, sv.bottom + dp(38f))
        alp.set(pad, hue.bottom + dp(10f), w - pad, hue.bottom + dp(36f))
    }

    override fun onDraw(canvas: Canvas) {
        val pure = Color.HSVToColor(floatArrayOf(hsv[0], 1f, 1f))
        val sat = LinearGradient(sv.left, 0f, sv.right, 0f, Color.WHITE, pure, Shader.TileMode.CLAMP)
        val valg = LinearGradient(0f, sv.top, 0f, sv.bottom, Color.WHITE, Color.BLACK, Shader.TileMode.CLAMP)
        p.shader = ComposeShader(valg, sat, PorterDuff.Mode.MULTIPLY)
        canvas.drawRoundRect(sv, dp(6f), dp(6f), p)
        val hues = IntArray(7) { Color.HSVToColor(floatArrayOf(it * 60f, 1f, 1f)) }
        p.shader = LinearGradient(hue.left, 0f, hue.right, 0f, hues, null, Shader.TileMode.CLAMP)
        canvas.drawRoundRect(hue, dp(6f), dp(6f), p)
        p.shader = checkerShader(context)
        canvas.drawRoundRect(alp, dp(6f), dp(6f), p)
        val opaque = Color.HSVToColor(hsv)
        p.shader = LinearGradient(alp.left, 0f, alp.right, 0f, opaque and 0x00FFFFFF, opaque, Shader.TileMode.CLAMP)
        canvas.drawRoundRect(alp, dp(6f), dp(6f), p)
        p.shader = null

        // markers
        p.style = Paint.Style.STROKE
        p.strokeWidth = dp(2.5f)
        val mx = sv.left + hsv[1] * sv.width()
        val my = sv.top + (1 - hsv[2]) * sv.height()
        p.color = Color.WHITE; canvas.drawCircle(mx, my, dp(9f), p)
        p.color = Color.BLACK; p.strokeWidth = dp(1f); canvas.drawCircle(mx, my, dp(10.5f), p)
        drawBarMarker(canvas, hue, hsv[0] / 360f)
        drawBarMarker(canvas, alp, alpha / 255f)
        p.style = Paint.Style.FILL
    }

    private fun drawBarMarker(canvas: Canvas, r: RectF, t: Float) {
        val x = r.left + t * r.width()
        p.color = Color.WHITE; p.strokeWidth = dp(3f)
        canvas.drawRoundRect(x - dp(4f), r.top - dp(3f), x + dp(4f), r.bottom + dp(3f), dp(3f), dp(3f), p)
        p.color = Color.BLACK; p.strokeWidth = dp(1f)
        canvas.drawRoundRect(x - dp(5.5f), r.top - dp(4.5f), x + dp(5.5f), r.bottom + dp(4.5f), dp(3f), dp(3f), p)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val x = e.x; val y = e.y
        if (e.actionMasked == MotionEvent.ACTION_DOWN) {
            dragging = when {
                y <= sv.bottom + dp(4f) -> 1
                y <= hue.bottom + dp(5f) -> 2
                else -> 3
            }
            parent?.requestDisallowInterceptTouchEvent(true)
        }
        when (dragging) {
            1 -> {
                hsv[1] = ((x - sv.left) / sv.width()).coerceIn(0f, 1f)
                hsv[2] = 1f - ((y - sv.top) / sv.height()).coerceIn(0f, 1f)
            }
            2 -> hsv[0] = ((x - hue.left) / hue.width()).coerceIn(0f, 0.9999f) * 360f
            3 -> alpha = (((x - alp.left) / alp.width()).coerceIn(0f, 1f) * 255).toInt()
        }
        invalidate()
        onChange(color)
        return true
    }
}

/**
 * Opens the colour picker. [allowNone] adds a "None" button (for fill / stroke).
 * [onPick] receives null when "None" was chosen.
 */
fun showColorPicker(context: Context, title: String, initial: Int?, allowNone: Boolean = false, onPick: (Int?) -> Unit) {
    val root = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(context.dp(12), context.dp(4), context.dp(12), 0)
    }
    val preview = ColorChip(context) {}
    val hex = EditText(context).apply {
        setSingleLine(); textSize = 15f; minEms = 6
    }
    var updatingHex = false
    val picker = ColorPickerView(context, initial ?: Color.BLACK) { c ->
        preview.color = c
        updatingHex = true; hex.setText(colorToHex(c)); updatingHex = false
    }
    preview.color = initial ?: Color.BLACK
    hex.setText(colorToHex(initial ?: Color.BLACK))
    hex.addTextChangedListener(object : TextWatcher {
        override fun afterTextChanged(s: Editable?) {
            if (updatingHex) return
            parseHex(s.toString())?.let { picker.set(it); preview.color = it }
        }
        override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
        override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
    })
    root.addView(picker)
    val hexRow = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
        addView(preview); addView(label(context, "Hex")); addView(hex)
    }
    root.addView(hexRow)

    fun swatchGrid(colors: List<Int>): GridLayout = GridLayout(context).apply {
        columnCount = 10
        for (c in colors) addView(ColorChip(context) { picker.set(c); preview.color = c; hex.setText(colorToHex(c)) }.apply {
            color = c
            layoutParams = GridLayout.LayoutParams().apply { width = context.dp(30); height = context.dp(30) }
        })
    }
    if (RecentColors.list.isNotEmpty()) {
        root.addView(label(context, "Recent", 12f))
        root.addView(swatchGrid(RecentColors.list))
    }
    root.addView(label(context, "Swatches", 12f))
    root.addView(swatchGrid(Theme.SWATCHES.toList()))

    val scroll = ScrollView(context).apply { addView(root) }
    val b = AlertDialog.Builder(context)
        .setTitle(title)
        .setView(scroll)
        .setPositiveButton("OK") { _, _ -> RecentColors.push(picker.color); onPick(picker.color) }
        .setNegativeButton("Cancel", null)
    if (allowNone) b.setNeutralButton("None") { _, _ -> onPick(null) }
    b.show()
}
