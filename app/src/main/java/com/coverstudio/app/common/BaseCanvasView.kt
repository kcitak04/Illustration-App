package com.coverstudio.app.common

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.PointF
import android.graphics.RectF
import android.os.SystemClock
import android.view.MotionEvent
import android.view.View
import com.coverstudio.app.ui.Theme
import com.coverstudio.app.ui.checkerShader
import com.coverstudio.app.ui.dp
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

/** One input sample already converted to document coordinates. */
class Sample(val x: Float, val y: Float, val pressure: Float, val time: Long)

/** Information about the pointer that started a tool gesture. */
class PointerInfo(val isStylus: Boolean, val isEraserEnd: Boolean, val sideButton: Boolean)

enum class InputMode { AUTO, STYLUS_ONLY, ANY }

object InputSettings {
    /** AUTO: fingers draw until a stylus is seen, then fingers only navigate (palm rejection). */
    var mode = InputMode.AUTO
    var stylusSeen = false
    /** Pressure curve exponent. <1 = softer (easier to get thick lines), >1 = firmer. */
    var pressureGamma = 1.0f
    var twoFingerTapUndo = true
}

/**
 * Shared canvas behaviour for both studios: zoom / pan with two fingers,
 * stylus detection + palm rejection, pressure curve, historical sample delivery,
 * two-finger-tap undo / three-finger-tap redo, and stylus hover cursor.
 */
abstract class BaseCanvasView(context: Context) : View(context) {
    var docW = 1000
        protected set
    var docH = 1000
        protected set

    val viewMatrix = Matrix()
    private val inverse = Matrix()
    private var fitted = false

    var onUndoGesture: (() -> Unit)? = null
    var onRedoGesture: (() -> Unit)? = null
    var onViewChanged: (() -> Unit)? = null

    /** Last stylus hover position in document coordinates (null if not hovering). */
    var hover: PointF? = null
        private set

    private enum class Mode { NONE, TOOL, NAV }
    private var mode = Mode.NONE
    private var toolPointerId = -1
    private var toolStartTime = 0L
    private var toolMoved = 0f
    private var toolStartX = 0f
    private var toolStartY = 0f

    private var navCx = 0f
    private var navCy = 0f
    private var navSpan = 0f
    private var tapStart = 0L
    private var tapMaxPointers = 0
    private var tapTravel = 0f
    private var lastCx = 0f
    private var lastCy = 0f

    private val pts = FloatArray(2)
    private val shadowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x66000000 }
    private val checkerPaint = Paint()
    private val checkerMatrix = Matrix()

    init {
        isFocusable = true
    }

    val zoom: Float
        get() {
            val v = FloatArray(9); viewMatrix.getValues(v)
            return hypot(v[Matrix.MSCALE_X], v[Matrix.MSKEW_Y])
        }

    fun setDocSize(w: Int, h: Int) {
        docW = w; docH = h; fitted = false
        if (width > 0) fitToScreen()
    }

    fun fitToScreen() {
        if (width == 0 || height == 0) return
        val margin = dp(24f)
        val s = min((width - margin * 2) / docW, (height - margin * 2) / docH)
        viewMatrix.reset()
        viewMatrix.postScale(s, s)
        viewMatrix.postTranslate((width - docW * s) / 2f, (height - docH * s) / 2f)
        fitted = true
        matrixChanged()
    }

    /** Zoom so that one document pixel == one screen pixel, centred. */
    fun actualPixels() {
        val cx = width / 2f; val cy = height / 2f
        val doc = viewToDoc(cx, cy)
        viewMatrix.reset()
        viewMatrix.postTranslate(cx - doc.x, cy - doc.y)
        matrixChanged()
    }

    private fun matrixChanged() {
        viewMatrix.invert(inverse)
        onViewChanged?.invoke()
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (!fitted) fitToScreen() else {
            viewMatrix.postTranslate((w - oldw) / 2f, (h - oldh) / 2f); matrixChanged()
        }
    }

    fun viewToDoc(x: Float, y: Float): PointF {
        pts[0] = x; pts[1] = y; inverse.mapPoints(pts); return PointF(pts[0], pts[1])
    }

    fun docToView(x: Float, y: Float): PointF {
        pts[0] = x; pts[1] = y; viewMatrix.mapPoints(pts); return PointF(pts[0], pts[1])
    }

    /** Converts a length in screen dp into document units at the current zoom. */
    fun docLen(dpLen: Float): Float = dp(dpLen) / zoom

    // ---------------------------------------------------------------- drawing

    final override fun onDraw(canvas: Canvas) {
        canvas.drawColor(Theme.PASTEBOARD)
        canvas.save()
        canvas.concat(viewMatrix)
        val z = zoom
        val sh = dp(3f) / z
        canvas.drawRect(sh, sh, docW + sh, docH + sh, shadowPaint)
        // checkerboard in screen-space-sized cells so transparency is always visible
        val cs = checkerShader(context, max(4, dp(8f).toInt()))
        checkerMatrix.setScale(1f / z, 1f / z); cs.setLocalMatrix(checkerMatrix)
        checkerPaint.shader = cs
        canvas.drawRect(0f, 0f, docW.toFloat(), docH.toFloat(), checkerPaint)
        canvas.clipRect(0f, 0f, docW.toFloat(), docH.toFloat())
        drawDocument(canvas)
        canvas.restore()
        canvas.save()
        canvas.concat(viewMatrix)
        drawDocOverlay(canvas)
        canvas.restore()
        drawScreenOverlay(canvas)
    }

    /** Draw the artwork in document coordinates (already clipped to the artboard). */
    protected abstract fun drawDocument(canvas: Canvas)

    /** Draw guides / handles in document coordinates (not clipped). */
    protected open fun drawDocOverlay(canvas: Canvas) {}

    /** Draw anything in screen coordinates. */
    protected open fun drawScreenOverlay(canvas: Canvas) {}

    // ---------------------------------------------------------------- tool callbacks

    protected abstract fun onToolDown(s: Sample, info: PointerInfo)
    protected abstract fun onToolMove(s: Sample)
    protected abstract fun onToolUp(s: Sample)
    protected abstract fun onToolCancel()

    /** Whether the current tool wants single-finger drags (e.g. hand tool) to pan. */
    protected open fun toolIsPan(): Boolean = false

    // ---------------------------------------------------------------- input

    private fun isStylus(e: MotionEvent, i: Int): Boolean {
        val t = e.getToolType(i)
        return t == MotionEvent.TOOL_TYPE_STYLUS || t == MotionEvent.TOOL_TYPE_ERASER
    }

    private fun canDraw(e: MotionEvent, i: Int): Boolean {
        if (toolIsPan()) return false
        if (isStylus(e, i)) return true
        if (e.getToolType(i) == MotionEvent.TOOL_TYPE_MOUSE) return true
        return when (InputSettings.mode) {
            InputMode.ANY -> true
            InputMode.STYLUS_ONLY -> false
            InputMode.AUTO -> !InputSettings.stylusSeen
        }
    }

    private fun pressureOf(e: MotionEvent, i: Int, hist: Int = -1): Float {
        if (!isStylus(e, i)) return 1f
        val raw = if (hist >= 0) e.getHistoricalPressure(i, hist) else e.getPressure(i)
        return raw.coerceIn(0.01f, 1f).pow(InputSettings.pressureGamma)
    }

    private fun sampleAt(e: MotionEvent, i: Int, hist: Int = -1): Sample {
        val x = if (hist >= 0) e.getHistoricalX(i, hist) else e.getX(i)
        val y = if (hist >= 0) e.getHistoricalY(i, hist) else e.getY(i)
        val t = if (hist >= 0) e.getHistoricalEventTime(hist) else e.eventTime
        pts[0] = x; pts[1] = y; inverse.mapPoints(pts)
        return Sample(pts[0], pts[1], pressureOf(e, i, hist), t)
    }

    private fun startNav(e: MotionEvent) {
        mode = Mode.NAV
        computeCentroid(e, -1)
        navCx = lastCx; navCy = lastCy
        navSpan = spanOf(e, -1)
    }

    private fun computeCentroid(e: MotionEvent, skip: Int) {
        var sx = 0f; var sy = 0f; var n = 0
        for (i in 0 until e.pointerCount) {
            if (i == skip) continue
            if (isStylus(e, i) && e.pointerCount > 1) continue
            sx += e.getX(i); sy += e.getY(i); n++
        }
        if (n == 0) { lastCx = e.getX(0); lastCy = e.getY(0); return }
        lastCx = sx / n; lastCy = sy / n
    }

    private fun spanOf(e: MotionEvent, skip: Int): Float {
        var total = 0f; var n = 0
        for (i in 0 until e.pointerCount) {
            if (i == skip) continue
            total += hypot(e.getX(i) - lastCx, e.getY(i) - lastCy); n++
        }
        return if (n > 1) total / n else 0f
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val idx = e.actionIndex
        if (isStylus(e, idx) && !InputSettings.stylusSeen) InputSettings.stylusSeen = true
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                tapStart = SystemClock.uptimeMillis(); tapMaxPointers = 1; tapTravel = 0f
                if (canDraw(e, 0)) {
                    mode = Mode.TOOL
                    toolPointerId = e.getPointerId(0)
                    toolStartTime = e.eventTime
                    toolMoved = 0f
                    toolStartX = e.x; toolStartY = e.y
                    val info = PointerInfo(
                        isStylus(e, 0),
                        e.getToolType(0) == MotionEvent.TOOL_TYPE_ERASER,
                        (e.buttonState and (MotionEvent.BUTTON_STYLUS_PRIMARY or MotionEvent.BUTTON_SECONDARY)) != 0
                    )
                    onToolDown(sampleAt(e, 0), info)
                } else {
                    startNav(e)
                }
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                tapMaxPointers = max(tapMaxPointers, e.pointerCount)
                if (mode == Mode.TOOL) {
                    val toolIdx = e.findPointerIndex(toolPointerId)
                    val toolIsStylus = toolIdx >= 0 && isStylus(e, toolIdx)
                    if (!toolIsStylus) {
                        // a finger stroke turned into a navigation gesture
                        onToolCancel()
                        startNav(e)
                    } // else: palm / resting fingers while drawing with the stylus -> ignore
                } else if (mode == Mode.NAV) {
                    startNav(e)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (mode == Mode.TOOL) {
                    val i = e.findPointerIndex(toolPointerId)
                    if (i >= 0) {
                        toolMoved = max(toolMoved, hypot(e.getX(i) - toolStartX, e.getY(i) - toolStartY))
                        for (h in 0 until e.historySize) onToolMove(sampleAt(e, i, h))
                        onToolMove(sampleAt(e, i))
                    }
                } else if (mode == Mode.NAV) {
                    computeCentroid(e, -1)
                    val span = spanOf(e, -1)
                    val dx = lastCx - navCx; val dy = lastCy - navCy
                    tapTravel += abs(dx) + abs(dy)
                    viewMatrix.postTranslate(dx, dy)
                    if (navSpan > dp(10f) && span > dp(10f)) {
                        var f = span / navSpan
                        val z = zoom
                        val nz = (z * f).coerceIn(0.02f, 64f)
                        f = nz / z
                        tapTravel += abs(span - navSpan)
                        viewMatrix.postScale(f, f, lastCx, lastCy)
                    }
                    navCx = lastCx; navCy = lastCy; navSpan = span
                    matrixChanged()
                }
            }
            MotionEvent.ACTION_POINTER_UP -> {
                if (mode == Mode.TOOL && e.getPointerId(idx) == toolPointerId) {
                    onToolUp(sampleAt(e, idx)); mode = Mode.NONE; toolPointerId = -1
                } else if (mode == Mode.NAV) {
                    computeCentroid(e, idx)
                    navCx = lastCx; navCy = lastCy; navSpan = spanOf(e, idx)
                }
            }
            MotionEvent.ACTION_UP -> {
                if (mode == Mode.TOOL) {
                    onToolUp(sampleAt(e, 0))
                } else if (mode == Mode.NAV) {
                    val quick = SystemClock.uptimeMillis() - tapStart < 300 && tapTravel < dp(24f)
                    if (quick && InputSettings.twoFingerTapUndo) {
                        if (tapMaxPointers == 2) onUndoGesture?.invoke()
                        else if (tapMaxPointers == 3) onRedoGesture?.invoke()
                    }
                }
                mode = Mode.NONE; toolPointerId = -1
            }
            MotionEvent.ACTION_CANCEL -> {
                if (mode == Mode.TOOL) onToolCancel()
                mode = Mode.NONE; toolPointerId = -1
            }
        }
        return true
    }

    override fun onHoverEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> {
                if (isStylus(e, 0)) { hover = viewToDoc(e.x, e.y); invalidate() }
            }
            MotionEvent.ACTION_HOVER_EXIT -> { hover = null; invalidate() }
        }
        return true
    }

    override fun onGenericMotionEvent(e: MotionEvent): Boolean {
        // mouse wheel / trackpad zoom for Chromebooks & DeX
        if (e.actionMasked == MotionEvent.ACTION_SCROLL) {
            val v = e.getAxisValue(MotionEvent.AXIS_VSCROLL)
            if (v != 0f) {
                val f = if (v > 0) 1.15f else 1 / 1.15f
                viewMatrix.postScale(f, f, e.x, e.y); matrixChanged(); return true
            }
        }
        return super.onGenericMotionEvent(e)
    }

    /** Helper for views: current visible document rectangle. */
    fun visibleDocRect(): RectF {
        val r = RectF(0f, 0f, width.toFloat(), height.toFloat())
        inverse.mapRect(r); return r
    }
}
