package com.coverstudio.app.ui

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast

/** Square icon button with an optional "active" highlight, used for tools and actions. */
@SuppressLint("ViewConstructor")
class IconButton(
    context: Context,
    var icon: String,
    val label: String,
    private val onClick: (IconButton) -> Unit
) : View(context) {
    var active = false
        set(v) { field = v; invalidate() }
    var accent = Theme.ACCENT
    var iconColor = Theme.TEXT
    private val bg = Paint(Paint.ANTI_ALIAS_FLAG)
    private val rect = RectF()

    init {
        isClickable = true
        contentDescription = label
        setOnClickListener { onClick(this) }
        setOnLongClickListener {
            Toast.makeText(context, label, Toast.LENGTH_SHORT).show(); true
        }
        val size = dp(44)
        layoutParams = ViewGroup.MarginLayoutParams(size, size)
    }

    override fun onDraw(canvas: Canvas) {
        val pad = dp(3f)
        rect.set(pad, pad, width - pad, height - pad)
        if (active) {
            bg.color = accent
            canvas.drawRoundRect(rect, dp(8f), dp(8f), bg)
        } else if (isPressed) {
            bg.color = Theme.PANEL_2
            canvas.drawRoundRect(rect, dp(8f), dp(8f), bg)
        }
        val c = if (!isEnabled) Theme.TEXT_DIM else if (active) Color.WHITE else iconColor
        Icons.draw(canvas, icon, width / 2f, height / 2f, dp(22f), c)
    }

    override fun drawableStateChanged() {
        super.drawableStateChanged(); invalidate()
    }
}

/** Colour chip; the checkerboard underneath shows transparency. "Hollow" chips mean "stroke". */
@SuppressLint("ViewConstructor")
class ColorChip(context: Context, var hollow: Boolean = false, private val onClick: (ColorChip) -> Unit) : View(context) {
    var color: Int? = Color.BLACK
        set(v) { field = v; invalidate() }
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val rect = RectF()

    init {
        isClickable = true
        setOnClickListener { onClick(this) }
        layoutParams = ViewGroup.MarginLayoutParams(dp(40), dp(40))
    }

    override fun onDraw(canvas: Canvas) {
        val pad = dp(6f)
        rect.set(pad, pad, width - pad, height - pad)
        val rad = dp(6f)
        val c = color
        if (hollow) {
            val w = dp(7f)
            p.style = Paint.Style.STROKE
            p.strokeWidth = w
            rect.inset(w / 2, w / 2)
            if (c == null) {
                p.color = Color.WHITE; canvas.drawRoundRect(rect, rad, rad, p)
            } else {
                p.shader = checkerShader(context); canvas.drawRoundRect(rect, rad, rad, p); p.shader = null
                p.color = c; canvas.drawRoundRect(rect, rad, rad, p)
            }
            p.style = Paint.Style.FILL
        } else {
            if (c == null) {
                p.color = Color.WHITE; canvas.drawRoundRect(rect, rad, rad, p)
            } else {
                p.shader = checkerShader(context); canvas.drawRoundRect(rect, rad, rad, p); p.shader = null
                p.color = c; canvas.drawRoundRect(rect, rad, rad, p)
            }
        }
        if (c == null) {
            p.color = Theme.DANGER; p.strokeWidth = dp(2f); p.style = Paint.Style.STROKE
            canvas.drawLine(rect.left, rect.bottom, rect.right, rect.top, p)
            p.style = Paint.Style.FILL
        }
        p.style = Paint.Style.STROKE; p.strokeWidth = dp(1f); p.color = Theme.DIVIDER
        rect.set(pad, pad, width - pad, height - pad)
        canvas.drawRoundRect(rect, rad, rad, p)
        p.style = Paint.Style.FILL
    }
}

private val checkers = HashMap<Int, Bitmap>()
fun checkerShader(context: Context, cell: Int = context.dp(6)): BitmapShader {
    val bmp = checkers.getOrPut(cell) {
        Bitmap.createBitmap(cell * 2, cell * 2, Bitmap.Config.ARGB_8888).also {
            val c = Canvas(it)
            c.drawColor(0xFFFFFFFF.toInt())
            val p = Paint().apply { color = 0xFFCCCCCC.toInt() }
            c.drawRect(0f, 0f, cell.toFloat(), cell.toFloat(), p)
            c.drawRect(cell.toFloat(), cell.toFloat(), cell * 2f, cell * 2f, p)
        }
    }
    return BitmapShader(bmp, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT)
}

/**
 * Helps sliders that preview live: records one undo step on the first change of a drag,
 * then lets the value change freely until the finger lifts.
 */
class LiveEdit(private val record: () -> Unit) {
    private var started = false
    fun change(done: Boolean, apply: () -> Unit) {
        if (!started) { record(); started = true }
        apply()
        if (done) started = false
    }
}

fun label(context: Context, text: String, size: Float = 13f, color: Int = Theme.TEXT_DIM): TextView =
    TextView(context).apply {
        this.text = text
        textSize = size
        setTextColor(color)
        gravity = Gravity.CENTER_VERTICAL
        setPadding(dp(6), 0, dp(6), 0)
    }

fun textButton(context: Context, text: String, primary: Boolean = false, onClick: () -> Unit): TextView =
    TextView(context).apply {
        this.text = text
        textSize = 14f
        setTextColor(if (primary) Color.WHITE else Theme.TEXT)
        gravity = Gravity.CENTER
        setPadding(dp(14), dp(8), dp(14), dp(8))
        background = roundRect(if (primary) Theme.ACCENT else Theme.PANEL_2, dp(8f))
        isClickable = true
        setOnClickListener { onClick() }
        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            setMargins(dp(4), dp(4), dp(4), dp(4))
        }
    }

/** Compact labelled slider: "Size [=====o---] 24". */
class SliderRow(
    context: Context,
    title: String,
    val min: Float,
    val max: Float,
    initial: Float,
    private val format: (Float) -> String = { it.toInt().toString() },
    widthDp: Int = 150,
    private val onChange: (Float, Boolean) -> Unit
) : LinearLayout(context) {
    private val seek = SeekBar(context)
    private val valueText = label(context, "", 12f, Theme.TEXT)
    private var suppress = false
    val value: Float get() = min + (max - min) * seek.progress / 1000f

    init {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        addView(label(context, title, 12f))
        seek.max = 1000
        seek.progressTintList = ColorStateList.valueOf(Theme.ACCENT)
        seek.thumbTintList = ColorStateList.valueOf(Theme.ACCENT)
        seek.progressBackgroundTintList = ColorStateList.valueOf(Theme.DIVIDER)
        addView(seek, LayoutParams(dp(widthDp), dp(40)))
        valueText.minWidth = dp(40)
        addView(valueText)
        setValue(initial)
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) {
                valueText.text = format(value)
                if (!suppress && fromUser) onChange(value, false)
            }
            override fun onStartTrackingTouch(s: SeekBar) {}
            override fun onStopTrackingTouch(s: SeekBar) { onChange(value, true) }
        })
    }

    fun setValue(v: Float) {
        suppress = true
        seek.progress = (((v - min) / (max - min)).coerceIn(0f, 1f) * 1000).toInt()
        valueText.text = format(v)
        suppress = false
    }
}

/** Horizontally scrolling strip for tool options. */
class OptionsBar(context: Context) : HorizontalScrollView(context) {
    val row = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setPadding(dp(6), 0, dp(6), 0)
    }

    init {
        isHorizontalScrollBarEnabled = false
        setBackgroundColor(Theme.PANEL)
        addView(row, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(52)))
    }

    fun clear() = row.removeAllViews()
    fun add(v: View) = row.addView(v)
    fun divider() {
        row.addView(View(context).apply { setBackgroundColor(Theme.DIVIDER) },
            LinearLayout.LayoutParams(dp(1), dp(28)).apply { setMargins(dp(6), 0, dp(6), 0) })
    }
}

fun promptText(context: Context, title: String, initial: String, multiline: Boolean = false, onOk: (String) -> Unit) {
    val input = EditText(context).apply {
        setText(initial)
        setSelection(initial.length)
        inputType = if (multiline) InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
        else InputType.TYPE_CLASS_TEXT
        if (multiline) minLines = 2
    }
    val wrap = FrameLayout(context).apply { setPadding(context.dp(20), context.dp(8), context.dp(20), 0); addView(input) }
    AlertDialog.Builder(context)
        .setTitle(title)
        .setView(wrap)
        .setPositiveButton("OK") { _, _ -> onOk(input.text.toString()) }
        .setNegativeButton("Cancel", null)
        .show()
}

fun choose(context: Context, title: String, items: List<String>, onPick: (Int) -> Unit) {
    AlertDialog.Builder(context)
        .setTitle(title)
        .setItems(items.toTypedArray()) { _, i -> onPick(i) }
        .show()
}

fun toast(context: Context, msg: String) = Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()
