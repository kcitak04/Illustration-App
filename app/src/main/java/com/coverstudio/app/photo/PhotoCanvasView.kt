package com.coverstudio.app.photo

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PointF
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import android.graphics.RadialGradient
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Shader
import android.os.Handler
import android.os.Looper
import com.coverstudio.app.common.BaseCanvasView
import com.coverstudio.app.common.PointerInfo
import com.coverstudio.app.common.Sample
import com.coverstudio.app.ui.Theme
import com.coverstudio.app.vector.FloatArrayList
import com.coverstudio.app.vector.VectorDoc
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

enum class PTool(val icon: String, val label: String) {
    MOVE("move", "Move / free transform layer (V)"),
    MARQUEE("marquee", "Marquee selection (M)"),
    LASSO("lasso", "Lasso selection (L)"),
    WAND("wand", "Magic wand (W)"),
    BRUSH("brush", "Brush (B)"),
    ERASER("eraser", "Eraser (E)"),
    SMUDGE("smudge", "Smudge"),
    CLONE("clone", "Clone stamp (S) – side button / 'Set source' picks the source"),
    GRADIENT("gradient", "Gradient (G)"),
    BUCKET("bucket", "Paint bucket"),
    EYEDROPPER("eyedropper", "Eyedropper (I)"),
    HAND("hand", "Hand / pan (H)")
}

class BrushSettings(
    var size: Float, var hardness: Float, var opacity: Float, var flow: Float,
    var spacing: Float = 0.12f, var pressureSize: Boolean = true, var pressureOpacity: Boolean = false
)

class PhotoCanvasView(context: Context) : BaseCanvasView(context) {
    var doc = RasterDoc(3000, 3000)
        private set

    var tool = PTool.BRUSH
        set(v) {
            if (field == PTool.MOVE && v != PTool.MOVE) commitTransform()
            field = v; invalidate()
        }

    var color = Color.BLACK
    var color2 = Color.WHITE
    val brush = BrushSettings(40f, 0.8f, 1f, 1f)
    val eraser = BrushSettings(80f, 0.6f, 1f, 1f)
    val clone = BrushSettings(80f, 0.5f, 1f, 1f)
    val smudge = BrushSettings(80f, 0.3f, 1f, 1f, 0.1f, false, true)
    var smudgeStrength = 0.6f
    var gradientRadial = false
    var gradientToTransparent = false
    var gradientOpacity = 1f
    var fillTolerance = 32f
    var fillOpacity = 1f
    var contiguous = true
    var sampleAllLayers = false
    var marqueeEllipse = false
    /** 0 = new, 1 = add, 2 = subtract. */
    var selectionMode = 0
    var editMask = false
    var pickCloneSource = false

    var onChanged: (() -> Unit)? = null
    var onLayersChanged: (() -> Unit)? = null
    var onColorPicked: ((Int) -> Unit)? = null
    var onNeedsRasterize: ((RasterLayer, () -> Unit) -> Unit)? = null
    var onBusy: ((Boolean) -> Unit)? = null
    var onTransformStateChanged: (() -> Unit)? = null
    var onMessage: ((String) -> Unit)? = null

    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()

    fun setDocument(d: RasterDoc) {
        doc = d; undo.clear(); redo.clear(); undoBytes = 0
        pendingMatrix = null; filterPreview = null
        setDocSize(d.width, d.height)
        onLayersChanged?.invoke(); invalidate()
    }

    // ------------------------------------------------------------------ undo

    private val undo = ArrayList<DocState>()
    private val redo = ArrayList<DocState>()
    private var undoBytes = 0L
    private val undoBudget = Runtime.getRuntime().maxMemory() / 3
    val canUndo get() = undo.isNotEmpty()
    val canRedo get() = redo.isNotEmpty()

    fun pushUndo(bytes: Long = 0) {
        undo.add(doc.state(bytes)); undoBytes += bytes
        redo.clear()
        while (undo.size > 3 && (undoBytes > undoBudget || undo.size > 60)) {
            undoBytes -= undo.removeAt(0).bytes
        }
    }

    fun undo() {
        commitTransform()
        if (undo.isEmpty()) return
        val s = undo.removeAt(undo.size - 1); undoBytes -= s.bytes
        redo.add(doc.state(s.bytes))
        doc.restore(s)
        afterHistory()
    }

    fun redo() {
        if (redo.isEmpty()) return
        val s = redo.removeAt(redo.size - 1)
        undo.add(doc.state(s.bytes)); undoBytes += s.bytes
        doc.restore(s)
        afterHistory()
    }

    private fun afterHistory() {
        filterPreview = null
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    /** Copy-on-write: the old bitmap goes to the undo stack, the layer gets a fresh editable copy. */
    fun beginPixelEdit(l: RasterLayer, mask: Boolean = false) {
        if (mask) {
            val m = l.mask ?: return
            pushUndo(m.byteCount.toLong())
            l.mask = m.copy(Bitmap.Config.ALPHA_8, true)
        } else {
            pushUndo(l.bitmap.byteCount.toLong())
            l.bitmap = l.bitmap.copy(Bitmap.Config.ARGB_8888, true)
        }
        l.version++
    }

    /** Replaces the layer's pixels with [bmp] (undoable). */
    fun replacePixels(l: RasterLayer, bmp: Bitmap) {
        pushUndo(l.bitmap.byteCount.toLong())
        l.bitmap = bmp; l.version++
        onChanged?.invoke(); onLayersChanged?.invoke(); invalidate()
    }

    // ------------------------------------------------------------------ paints & dab textures

    private val dstIn = Paint(Paint.FILTER_BITMAP_FLAG).apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_IN) }
    private val bmpPaint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val dabPaint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG)
    private val overlay = Paint(Paint.ANTI_ALIAS_FLAG)
    private val dabCache = HashMap<Int, Bitmap>()

    private fun dabTexture(hardness: Float): Bitmap {
        val key = (hardness * 20).roundToInt()
        return dabCache.getOrPut(key) {
            val n = 128
            val bmp = Bitmap.createBitmap(n, n, Bitmap.Config.ALPHA_8)
            val bytes = ByteArray(bmp.rowBytes * n)
            val h = key / 20f
            for (y in 0 until n) for (x in 0 until n) {
                val d = hypot(x + 0.5f - n / 2f, y + 0.5f - n / 2f) / (n / 2f)
                val a = when {
                    d >= 1f -> 0f
                    d <= h -> 1f
                    else -> { val t = (d - h) / (1f - h).coerceAtLeast(0.001f); 1f - t * t * (3 - 2 * t) }
                }
                bytes[y * bmp.rowBytes + x] = (a * 255).roundToInt().toByte()
            }
            bmp.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(bytes))
            bmp
        }
    }

    // ------------------------------------------------------------------ stroke state

    private var strokeActive = false
    private var strokeTool = PTool.BRUSH
    private val dabs = FloatArrayList() // x, y, size  (alpha kept in parallel list)
    private val dabAlpha = FloatArrayList()
    private var lastX = 0f
    private var lastY = 0f
    private var lastP = 1f
    private var leftover = 0f
    private val dirty = RectF()
    private var cloneShader: BitmapShader? = null
    private var cloneOffX = 0f
    private var cloneOffY = 0f
    private var cloneSource: PointF? = null
    private var cloneAligned = false
    private var strokeSettings = brush

    // smudge
    private var smudgeCarry: Bitmap? = null
    private var smudgeTmp: Bitmap? = null

    // generic drag
    private var startX = 0f
    private var startY = 0f
    private var curX = 0f
    private var curY = 0f
    private var dragging = false
    private val lassoPath = Path()
    private var gestureTool = PTool.BRUSH

    override fun toolIsPan() = tool == PTool.HAND

    private fun settingsFor(t: PTool) = when (t) {
        PTool.ERASER -> eraser
        PTool.CLONE -> clone
        PTool.SMUDGE -> smudge
        else -> brush
    }

    private fun needsPixels(l: RasterLayer, then: () -> Unit): Boolean {
        if (l.locked) { onMessage?.invoke("Layer is locked"); return false }
        if (!l.visible) { onMessage?.invoke("Layer is hidden"); return false }
        if (l.smart && !(editMask && l.mask != null)) {
            onNeedsRasterize?.invoke(l, then); return false
        }
        return true
    }

    override fun onToolDown(s: Sample, info: PointerInfo) {
        startX = s.x; startY = s.y; curX = s.x; curY = s.y
        dragging = true
        gestureTool = when {
            info.isEraserEnd -> PTool.ERASER
            info.sideButton && tool == PTool.CLONE -> { setCloneSource(s.x, s.y); dragging = false; return }
            info.sideButton -> PTool.EYEDROPPER
            else -> tool
        }
        if (pickCloneSource && gestureTool == PTool.CLONE) {
            setCloneSource(s.x, s.y); pickCloneSource = false; dragging = false; onTransformStateChanged?.invoke(); return
        }
        val l = doc.activeLayer ?: return
        when (gestureTool) {
            PTool.BRUSH, PTool.ERASER, PTool.CLONE, PTool.SMUDGE -> {
                if (!needsPixels(l) {}) { dragging = false; return }
                if (gestureTool == PTool.CLONE) {
                    val src = cloneSource
                    if (src == null) { onMessage?.invoke("Set a clone source first (stylus button or 'Set source')"); dragging = false; return }
                    if (!cloneAligned) { cloneOffX = src.x - s.x; cloneOffY = src.y - s.y; cloneAligned = true }
                    cloneShader = BitmapShader(l.bitmap, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP).also {
                        val m = Matrix(); m.setTranslate(-cloneOffX, -cloneOffY); it.setLocalMatrix(m)
                    }
                }
                strokeTool = gestureTool
                strokeSettings = settingsFor(gestureTool)
                dabs.clear(); dabAlpha.clear(); leftover = 0f
                dirty.setEmpty()
                lastX = s.x; lastY = s.y; lastP = s.pressure
                strokeActive = true
                if (gestureTool == PTool.SMUDGE) startSmudge(l, s)
                else addDab(s.x, s.y, s.pressure)
            }
            PTool.EYEDROPPER -> pick(s.x, s.y)
            PTool.MOVE -> moveDown(s)
            PTool.LASSO -> { lassoPath.reset(); lassoPath.moveTo(s.x, s.y) }
            PTool.BUCKET -> { if (needsPixels(l) {}) bucketFill(l, s.x, s.y); dragging = false }
            PTool.WAND -> { magicWand(l, s.x, s.y); dragging = false }
            PTool.GRADIENT -> { if (!needsPixels(l) {}) dragging = false }
            else -> {}
        }
        invalidate()
    }

    override fun onToolMove(s: Sample) {
        if (!dragging) return
        curX = s.x; curY = s.y
        when (gestureTool) {
            PTool.BRUSH, PTool.ERASER, PTool.CLONE -> if (strokeActive) strokeTo(s.x, s.y, s.pressure)
            PTool.SMUDGE -> if (strokeActive) smudgeTo(s.x, s.y, s.pressure)
            PTool.EYEDROPPER -> pick(s.x, s.y)
            PTool.MOVE -> moveDrag()
            PTool.LASSO -> lassoPath.lineTo(s.x, s.y)
            else -> {}
        }
        invalidate()
    }

    override fun onToolUp(s: Sample) {
        onToolMove(s)
        if (!dragging) return
        dragging = false
        val l = doc.activeLayer
        when (gestureTool) {
            PTool.BRUSH, PTool.ERASER, PTool.CLONE -> if (strokeActive && l != null) commitStroke(l)
            PTool.SMUDGE -> if (strokeActive) { strokeActive = false; onChanged?.invoke(); onLayersChanged?.invoke() }
            PTool.MARQUEE -> {
                val r = RectF(min(startX, curX), min(startY, curY), max(startX, curX), max(startY, curY))
                if (r.width() < docLen(4f) && r.height() < docLen(4f)) { if (selectionMode == 0) deselect() }
                else {
                    val p = Path(); if (marqueeEllipse) p.addOval(r, Path.Direction.CW) else p.addRect(r, Path.Direction.CW)
                    applySelectionPath(p)
                }
            }
            PTool.LASSO -> {
                lassoPath.close()
                val b = RectF(); lassoPath.computeBounds(b, true)
                if (b.width() > docLen(4f) || b.height() > docLen(4f)) applySelectionPath(Path(lassoPath))
                lassoPath.reset()
            }
            PTool.GRADIENT -> if (l != null && hypot(curX - startX, curY - startY) > docLen(4f)) commitGradient(l)
            PTool.MOVE -> moveUp()
            else -> {}
        }
        invalidate()
    }

    override fun onToolCancel() {
        if (gestureTool == PTool.SMUDGE && strokeActive) { strokeActive = false; undo() }
        strokeActive = false; dragging = false; lassoPath.reset()
        invalidate()
    }

    // ------------------------------------------------------------------ brush stroke

    private fun dabSize(p: Float) = strokeSettings.size * (if (strokeSettings.pressureSize) 0.1f + 0.9f * p else 1f)
    private fun dabFlow(p: Float) = strokeSettings.flow * (if (strokeSettings.pressureOpacity) p else 1f)

    private fun addDab(x: Float, y: Float, p: Float) {
        val sz = max(0.5f, dabSize(p))
        dabs.add(x, y, sz); dabAlpha.add(dabFlow(p), 0f, 0f)
        dirty.union(x - sz, y - sz, x + sz, y + sz)
    }

    private fun strokeTo(x: Float, y: Float, p: Float) {
        val dist = hypot(x - lastX, y - lastY)
        if (dist < 0.01f) return
        var t = leftover
        while (true) {
            val f0 = t / dist
            if (f0 > 1f) break
            val pp = lastP + (p - lastP) * f0
            val step = max(0.5f, dabSize(pp) * strokeSettings.spacing)
            if (t > 0f) addDab(lastX + (x - lastX) * f0, lastY + (y - lastY) * f0, pp)
            t += step
        }
        leftover = t - dist
        lastX = x; lastY = y; lastP = p
    }

    private fun drawDabs(c: Canvas, colorInt: Int, shader: Shader?) {
        val tex = dabTexture(strokeSettings.hardness)
        dabPaint.reset(); dabPaint.isFilterBitmap = true; dabPaint.isAntiAlias = true
        dabPaint.color = colorInt or 0xFF000000.toInt()
        dabPaint.shader = shader
        val r = RectF()
        var i = 0; var j = 0
        while (i < dabs.size) {
            val h = dabs[i + 2] / 2f
            r.set(dabs[i] - h, dabs[i + 1] - h, dabs[i] + h, dabs[i + 1] + h)
            dabPaint.alpha = (dabAlpha[j] * 255).roundToInt().coerceIn(1, 255)
            c.drawBitmap(tex, null, r, dabPaint)
            i += 3; j += 3
        }
    }

    private fun strokeColor() = when (strokeTool) {
        PTool.ERASER -> Color.BLACK
        else -> color
    }

    private fun maskHides(): Boolean = strokeTool == PTool.BRUSH && Color.luminance(color) < 0.5f

    private var scratch: Bitmap? = null
    private fun scratchBitmap(): Bitmap {
        val s = scratch
        if (s != null && s.width == doc.width && s.height == doc.height) return s
        return doc.newBitmap().also { scratch = it }
    }

    private fun commitStroke(l: RasterLayer) {
        strokeActive = false
        val d = Rect(floor(dirty.left).toInt() - 2, floor(dirty.top).toInt() - 2, ceil(dirty.right).toInt() + 2, ceil(dirty.bottom).toInt() + 2)
        if (!d.intersect(0, 0, doc.width, doc.height)) return
        val sel = doc.selection
        val onMask = editMask && l.mask != null
        val sc = scratchBitmap()
        val c = Canvas(sc)
        c.clipRect(d)
        c.drawColor(0, PorterDuff.Mode.CLEAR)
        drawDabs(c, strokeColor(), if (strokeTool == PTool.CLONE) cloneShader else null)
        if (sel != null) c.drawBitmap(sel, 0f, 0f, dstIn)
        if (onMask) {
            beginPixelEdit(l, true)
            val mc = Canvas(l.mask!!)
            val hide = maskHides()
            val p = Paint().apply {
                alpha = (strokeSettings.opacity * 255).roundToInt()
                xfermode = PorterDuffXfermode(if (hide) PorterDuff.Mode.DST_OUT else PorterDuff.Mode.SRC_OVER)
            }
            mc.drawBitmap(sc, d, d, p)
        } else {
            beginPixelEdit(l)
            val lc = Canvas(l.bitmap)
            val p = Paint().apply {
                alpha = (strokeSettings.opacity * 255).roundToInt()
                if (strokeTool == PTool.ERASER) xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_OUT)
            }
            lc.drawBitmap(sc, d, d, p)
        }
        cloneShader = null
        onChanged?.invoke(); onLayersChanged?.invoke()
    }

    fun setCloneSource(x: Float, y: Float) {
        cloneSource = PointF(x, y); cloneAligned = false
        onMessage?.invoke("Clone source set")
        invalidate()
    }

    // ------------------------------------------------------------------ smudge

    private fun startSmudge(l: RasterLayer, s: Sample) {
        beginPixelEdit(l)
        val d = max(4, strokeSettings.size.roundToInt())
        smudgeCarry = Bitmap.createBitmap(d, d, Bitmap.Config.ARGB_8888).also {
            Canvas(it).drawBitmap(l.bitmap, Rect((s.x - d / 2f).toInt(), (s.y - d / 2f).toInt(), (s.x + d / 2f).toInt(), (s.y + d / 2f).toInt()), Rect(0, 0, d, d), null)
        }
        smudgeTmp = Bitmap.createBitmap(d, d, Bitmap.Config.ARGB_8888)
    }

    private fun smudgeTo(x: Float, y: Float, p: Float) {
        val l = doc.activeLayer ?: return
        val carry = smudgeCarry ?: return
        val tmp = smudgeTmp ?: return
        val d = carry.width
        val dist = hypot(x - lastX, y - lastY)
        val step = max(1f, d * strokeSettings.spacing)
        var t = leftover
        val lc = Canvas(l.bitmap)
        val tc = Canvas(tmp)
        val cc = Canvas(carry)
        val tex = dabTexture(strokeSettings.hardness)
        val full = Rect(0, 0, d, d)
        val paint = Paint(Paint.FILTER_BITMAP_FLAG)
        while (t <= dist) {
            val f = if (dist > 0) t / dist else 1f
            val px = lastX + (x - lastX) * f; val py = lastY + (y - lastY) * f
            val pp = lastP + (p - lastP) * f
            val dst = Rect((px - d / 2f).roundToInt(), (py - d / 2f).roundToInt(), (px - d / 2f).roundToInt() + d, (py - d / 2f).roundToInt() + d)
            tc.drawColor(0, PorterDuff.Mode.CLEAR)
            tc.drawBitmap(carry, 0f, 0f, null)
            tc.drawBitmap(tex, null, full, dstIn)
            paint.alpha = (smudgeStrength * (if (strokeSettings.pressureOpacity) pp else 1f) * 255).roundToInt().coerceIn(0, 255)
            lc.drawBitmap(tmp, full, dst, paint)
            // pick up paint from the new location
            paint.alpha = ((1f - smudgeStrength) * 255).roundToInt().coerceIn(0, 255)
            cc.drawBitmap(l.bitmap, dst, full, paint)
            t += step
        }
        leftover = t - dist
        lastX = x; lastY = y; lastP = p
        l.version++
    }

    // ------------------------------------------------------------------ gradient

    private fun gradientShader(): Shader {
        val c2 = if (gradientToTransparent) color and 0x00FFFFFF else color2
        return if (gradientRadial) RadialGradient(startX, startY, max(1f, hypot(curX - startX, curY - startY)), color, c2, Shader.TileMode.CLAMP)
        else LinearGradient(startX, startY, curX, curY, color, c2, Shader.TileMode.CLAMP)
    }

    private fun commitGradient(l: RasterLayer) {
        val sc = scratchBitmap()
        val c = Canvas(sc)
        c.drawColor(0, PorterDuff.Mode.CLEAR)
        c.drawRect(0f, 0f, doc.width.toFloat(), doc.height.toFloat(), Paint().apply { shader = gradientShader() })
        doc.selection?.let { c.drawBitmap(it, 0f, 0f, dstIn) }
        beginPixelEdit(l)
        Canvas(l.bitmap).drawBitmap(sc, 0f, 0f, Paint().apply { alpha = (gradientOpacity * 255).roundToInt() })
        onChanged?.invoke(); onLayersChanged?.invoke()
    }

    // ------------------------------------------------------------------ flood fill / wand

    private fun samplePixels(l: RasterLayer): IntArray =
        if (sampleAllLayers) pixelsOf(doc.flatten()) else pixelsOf(l.bitmap)

    /** Returns a byte mask (255 = filled) of the region matching the pixel at (x, y). */
    private fun floodMask(px: IntArray, w: Int, h: Int, sx: Int, sy: Int, tol: Int, contiguousFill: Boolean): ByteArray {
        val out = ByteArray(w * h)
        val target = px[sy * w + sx]
        fun match(c: Int): Boolean {
            if (c == target) return true
            val da = abs((c ushr 24) - (target ushr 24)); val dr = abs(((c shr 16) and 255) - ((target shr 16) and 255))
            val dg = abs(((c shr 8) and 255) - ((target shr 8) and 255)); val db = abs((c and 255) - (target and 255))
            return max(max(da, dr), max(dg, db)) <= tol
        }
        if (!contiguousFill) {
            for (i in px.indices) if (match(px[i])) out[i] = -1
            return out
        }
        var stack = IntArray(4096)
        var sp = 0
        fun push(v: Int) { if (sp == stack.size) stack = stack.copyOf(sp * 2); stack[sp++] = v }
        push(sy * w + sx)
        while (sp > 0) {
            val idx = stack[--sp]
            val y = idx / w
            var x = idx % w
            while (x >= 0 && out[y * w + x] == 0.toByte() && match(px[y * w + x])) x--
            x++
            var spanUp = false; var spanDown = false
            while (x < w && out[y * w + x] == 0.toByte() && match(px[y * w + x])) {
                out[y * w + x] = -1
                if (y > 0) {
                    val up = out[(y - 1) * w + x] == 0.toByte() && match(px[(y - 1) * w + x])
                    if (!spanUp && up) { push((y - 1) * w + x); spanUp = true } else if (spanUp && !up) spanUp = false
                }
                if (y < h - 1) {
                    val dn = out[(y + 1) * w + x] == 0.toByte() && match(px[(y + 1) * w + x])
                    if (!spanDown && dn) { push((y + 1) * w + x); spanDown = true } else if (spanDown && !dn) spanDown = false
                }
                x++
            }
        }
        return out
    }

    private fun bucketFill(l: RasterLayer, x: Float, y: Float) {
        val ix = x.toInt(); val iy = y.toInt()
        if (ix !in 0 until doc.width || iy !in 0 until doc.height) return
        val w = doc.width; val h = doc.height
        val c = color; val tol = fillTolerance.roundToInt()
        onBusy?.invoke(true)
        worker.execute {
            val px = samplePixels(l)
            val m = floodMask(px, w, h, ix, iy, tol, contiguous)
            val out = IntArray(w * h)
            for (i in out.indices) if (m[i] != 0.toByte()) out[i] = c
            val fillBmp = bitmapOf(out, w, h)
            main.post {
                onBusy?.invoke(false)
                doc.selection?.let { Canvas(fillBmp).drawBitmap(it, 0f, 0f, dstIn) }
                beginPixelEdit(l)
                Canvas(l.bitmap).drawBitmap(fillBmp, 0f, 0f, Paint().apply { alpha = (fillOpacity * 255).roundToInt() })
                onChanged?.invoke(); onLayersChanged?.invoke(); invalidate()
            }
        }
    }

    private fun magicWand(l: RasterLayer, x: Float, y: Float) {
        val ix = x.toInt(); val iy = y.toInt()
        if (ix !in 0 until doc.width || iy !in 0 until doc.height) return
        val w = doc.width; val h = doc.height
        val tol = fillTolerance.roundToInt()
        onBusy?.invoke(true)
        worker.execute {
            val px = samplePixels(l)
            val m = floodMask(px, w, h, ix, iy, tol, contiguous)
            main.post {
                onBusy?.invoke(false)
                val tmp = RasterDoc(w, h); tmp.setSelectionBytes(m)
                combineSelection(tmp.selection!!, null)
            }
        }
    }

    // ------------------------------------------------------------------ selection

    private fun applySelectionPath(p: Path) {
        val m = doc.newMask(0)
        Canvas(m).drawPath(p, Paint(Paint.ANTI_ALIAS_FLAG))
        combineSelection(m, p)
    }

    private fun combineSelection(newMask: Bitmap, path: Path?) {
        val cur = doc.selection
        when {
            selectionMode == 0 || (cur == null && selectionMode == 1) -> { doc.selection = newMask; doc.selectionPath = path }
            cur == null && selectionMode == 2 -> {
                val all = doc.newMask(255)
                Canvas(all).drawBitmap(newMask, 0f, 0f, Paint().apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_OUT) })
                doc.selection = all
                doc.selectionPath = path?.let { val r = Path(); r.addRect(0f, 0f, doc.width.toFloat(), doc.height.toFloat(), Path.Direction.CW); r.op(it, Path.Op.DIFFERENCE); r }
            }
            else -> {
                val c = Canvas(cur!!)
                c.drawBitmap(newMask, 0f, 0f, Paint().apply {
                    xfermode = PorterDuffXfermode(if (selectionMode == 1) PorterDuff.Mode.SRC_OVER else PorterDuff.Mode.DST_OUT)
                })
                val old = doc.selectionPath
                doc.selectionPath = if (old != null && path != null) Path(old).also { it.op(path, if (selectionMode == 1) Path.Op.UNION else Path.Op.DIFFERENCE) } else null
            }
        }
        onChanged?.invoke(); invalidate()
    }

    fun selectAll() {
        doc.selection = doc.newMask(255)
        doc.selectionPath = Path().apply { addRect(0f, 0f, doc.width.toFloat(), doc.height.toFloat(), Path.Direction.CW) }
        invalidate()
    }

    fun deselect() { doc.selection = null; doc.selectionPath = null; invalidate() }

    fun invertSelection() {
        val s = doc.selection ?: return selectAll()
        val all = doc.newMask(255)
        Canvas(all).drawBitmap(s, 0f, 0f, Paint().apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_OUT) })
        doc.selection = all
        doc.selectionPath = doc.selectionPath?.let { p ->
            Path().apply { addRect(0f, 0f, doc.width.toFloat(), doc.height.toFloat(), Path.Direction.CW); op(p, Path.Op.DIFFERENCE) }
        }
        invalidate()
    }

    fun featherSelection(radius: Float) {
        val s = doc.selection ?: return
        onBusy?.invoke(true)
        worker.execute {
            val bytes = doc.selectionBytes() ?: return@execute
            val px = IntArray(bytes.size) { ((bytes[it].toInt() and 255) shl 24) }
            blurPixels(px, doc.width, doc.height, radius)
            val out = ByteArray(px.size) { (px[it] ushr 24).toByte() }
            main.post {
                onBusy?.invoke(false)
                if (doc.selection === s) { val keep = doc.selectionPath; doc.setSelectionBytes(out); doc.selectionPath = keep }
                invalidate()
            }
        }
    }

    fun selectLayerContent() {
        val l = doc.activeLayer ?: return
        doc.selection = l.bitmap.extractAlpha(); doc.selectionPath = null
        invalidate()
    }

    /** Photoshop's "Layer via Copy" / "Layer via Cut". */
    fun layerViaCopy(cut: Boolean) {
        val l = doc.activeLayer ?: return
        val sel = doc.selection ?: run { onMessage?.invoke("Make a selection first"); return }
        if (l.smart) { onNeedsRasterize?.invoke(l) { layerViaCopy(cut) }; return }
        val copy = doc.newBitmap()
        val c = Canvas(copy)
        c.drawBitmap(l.bitmap, 0f, 0f, null)
        c.drawBitmap(sel, 0f, 0f, dstIn)
        if (cut) {
            beginPixelEdit(l)
            Canvas(l.bitmap).drawBitmap(sel, 0f, 0f, Paint().apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_OUT) })
        } else pushUndo()
        doc.addLayer(l.name + if (cut) " cut" else " copy", copy)
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun clearSelectionPixels() {
        val l = doc.activeLayer ?: return
        if (!needsPixels(l) { clearSelectionPixels() }) return
        beginPixelEdit(l)
        val c = Canvas(l.bitmap)
        val sel = doc.selection
        if (sel == null) c.drawColor(0, PorterDuff.Mode.CLEAR)
        else c.drawBitmap(sel, 0f, 0f, Paint().apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_OUT) })
        onChanged?.invoke(); onLayersChanged?.invoke(); invalidate()
    }

    fun fillSelection(col: Int) {
        val l = doc.activeLayer ?: return
        if (!needsPixels(l) { fillSelection(col) }) return
        val sc = scratchBitmap()
        val c = Canvas(sc)
        c.drawColor(col, PorterDuff.Mode.SRC)
        doc.selection?.let { c.drawBitmap(it, 0f, 0f, dstIn) }
        beginPixelEdit(l)
        Canvas(l.bitmap).drawBitmap(sc, 0f, 0f, null)
        onChanged?.invoke(); onLayersChanged?.invoke(); invalidate()
    }

    // ------------------------------------------------------------------ eyedropper

    private fun pick(x: Float, y: Float) {
        if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return
        val bmp = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp); c.translate(-x.toInt().toFloat(), -y.toInt().toFloat())
        doc.drawLayers(c, doc.layers)
        val col = bmp.getPixel(0, 0)
        if (Color.alpha(col) > 0) onColorPicked?.invoke(col or 0xFF000000.toInt())
    }

    // ------------------------------------------------------------------ move / free transform

    /** Accumulated transform that has not been applied to pixels yet. */
    var pendingMatrix: Matrix? = null
        private set
    private var transformBase = RectF()
    private var startMatrix = Matrix()
    private var dragMode = 0 // 1 move, 2 scale, 3 rotate
    private var handle = -1
    var proportional = true

    val transforming get() = pendingMatrix != null

    private fun contentBounds(l: RasterLayer): RectF {
        val small = 256f
        val s = min(1f, small / max(doc.width, doc.height))
        val sw = max(1, (doc.width * s).roundToInt()); val sh = max(1, (doc.height * s).roundToInt())
        val bmp = Bitmap.createScaledBitmap(l.bitmap, sw, sh, true)
        val px = pixelsOf(bmp)
        var x0 = sw; var y0 = sh; var x1 = -1; var y1 = -1
        for (y in 0 until sh) for (x in 0 until sw) if ((px[y * sw + x] ushr 24) > 2) {
            if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y
        }
        if (x1 < 0) return RectF(0f, 0f, doc.width.toFloat(), doc.height.toFloat())
        return RectF(max(0f, (x0 - 1) / s), max(0f, (y0 - 1) / s), min(doc.width.toFloat(), (x1 + 2) / s), min(doc.height.toFloat(), (y1 + 2) / s))
    }

    fun beginTransform(): Boolean {
        val l = doc.activeLayer ?: return false
        if (l.locked) { onMessage?.invoke("Layer is locked"); return false }
        if (pendingMatrix == null) {
            transformBase = contentBounds(l)
            pendingMatrix = Matrix()
            onTransformStateChanged?.invoke()
        }
        return true
    }

    private fun transformedCorners(): FloatArray {
        val b = transformBase
        val pts = floatArrayOf(b.left, b.top, b.right, b.top, b.right, b.bottom, b.left, b.bottom,
            b.centerX(), b.top, b.right, b.centerY(), b.centerX(), b.bottom, b.left, b.centerY(), b.centerX(), b.centerY())
        pendingMatrix?.mapPoints(pts)
        return pts
    }

    private fun rotateHandle(pts: FloatArray): PointF {
        val cx = pts[16]; val cy = pts[17]; val tx = pts[8]; val ty = pts[9]
        val d = hypot(tx - cx, ty - cy).coerceAtLeast(0.01f)
        val off = docLen(30f)
        return PointF(tx + (tx - cx) / d * off, ty + (ty - cy) / d * off)
    }

    private fun moveDown(s: Sample) {
        if (!beginTransform()) { dragging = false; return }
        startMatrix = Matrix(pendingMatrix)
        val pts = transformedCorners()
        val tol = docLen(22f)
        val rh = rotateHandle(pts)
        dragMode = 1; handle = -1
        if (hypot(rh.x - s.x, rh.y - s.y) < tol) { dragMode = 3; return }
        for (i in 0 until 8) if (hypot(pts[i * 2] - s.x, pts[i * 2 + 1] - s.y) < tol) { dragMode = 2; handle = i; return }
    }

    private fun moveDrag() {
        val m = Matrix(startMatrix)
        when (dragMode) {
            1 -> m.postTranslate(curX - startX, curY - startY)
            3 -> {
                val c = floatArrayOf(transformBase.centerX(), transformBase.centerY()); startMatrix.mapPoints(c)
                var deg = Math.toDegrees((atan2(curY - c[1], curX - c[0]) - atan2(startY - c[1], startX - c[0])).toDouble()).toFloat()
                val sn = (deg / 15f).roundToInt() * 15f; if (abs(deg - sn) < 3f) deg = sn
                m.postRotate(deg, c[0], c[1])
            }
            2 -> {
                // scale in the layer's local (untransformed) space so rotation is preserved
                val inv = Matrix(); startMatrix.invert(inv)
                val p = floatArrayOf(curX, curY); inv.mapPoints(p)
                val b = transformBase; val i = handle
                val ax = when (i) { 0, 3, 7 -> b.right; 1, 2, 5 -> b.left; else -> b.centerX() }
                val ay = when (i) { 0, 1, 4 -> b.bottom; 2, 3, 6 -> b.top; else -> b.centerY() }
                val hx = when (i) { 0, 3, 7 -> b.left; 1, 2, 5 -> b.right; else -> b.centerX() }
                val hy = when (i) { 0, 1, 4 -> b.top; 2, 3, 6 -> b.bottom; else -> b.centerY() }
                var sx = if (i == 4 || i == 6) 1f else (p[0] - ax) / (hx - ax).let { if (abs(it) < 0.001f) 0.001f else it }
                var sy = if (i == 5 || i == 7) 1f else (p[1] - ay) / (hy - ay).let { if (abs(it) < 0.001f) 0.001f else it }
                if (i < 4 && proportional) { val k = if (abs(sx) > abs(sy)) sx else sy; sx = k; sy = k }
                if (abs(sx) < 0.01f) sx = 0.01f; if (abs(sy) < 0.01f) sy = 0.01f
                m.set(startMatrix); m.preScale(sx, sy, ax, ay)
            }
        }
        pendingMatrix = m
    }

    private fun moveUp() { onTransformStateChanged?.invoke() }

    fun transformBy(f: (Matrix, RectF) -> Unit) {
        if (!beginTransform()) return
        val c = floatArrayOf(transformBase.centerX(), transformBase.centerY()); pendingMatrix!!.mapPoints(c)
        val b = RectF(transformBase); pendingMatrix!!.mapRect(b)
        f(pendingMatrix!!, b)
        invalidate(); onTransformStateChanged?.invoke()
    }

    /** Scales the layer so its content fills the canvas (cover). */
    fun fitLayerToCanvas(cover: Boolean) {
        val l = doc.activeLayer ?: return
        if (!beginTransform()) return
        val b = transformBase
        val s = if (cover) max(doc.width / b.width(), doc.height / b.height()) else min(doc.width / b.width(), doc.height / b.height())
        pendingMatrix!!.reset()
        pendingMatrix!!.postScale(s, s, b.centerX(), b.centerY())
        pendingMatrix!!.postTranslate(doc.width / 2f - b.centerX(), doc.height / 2f - b.centerY())
        l.version++
        invalidate(); onTransformStateChanged?.invoke()
    }

    fun cancelTransform() { pendingMatrix = null; invalidate(); onTransformStateChanged?.invoke() }

    fun commitTransform() {
        val m = pendingMatrix ?: return
        pendingMatrix = null
        val l = doc.activeLayer ?: return
        if (m.isIdentity) { onTransformStateChanged?.invoke(); invalidate(); return }
        pushUndo(l.bitmap.byteCount.toLong() + (l.mask?.byteCount ?: 0))
        if (l.smart) {
            l.smartMatrix.postConcat(m)
            smartVector?.let { renderSmart(l, it) }
        } else {
            val nb = doc.newBitmap()
            Canvas(nb).drawBitmap(l.bitmap, m, Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG))
            l.bitmap = nb
        }
        l.mask?.let { mk ->
            val nm = doc.newMask(0)
            Canvas(nm).drawBitmap(mk, m, Paint(Paint.FILTER_BITMAP_FLAG))
            l.mask = nm
        }
        l.version++
        onTransformStateChanged?.invoke(); onChanged?.invoke(); onLayersChanged?.invoke(); invalidate()
    }

    // ------------------------------------------------------------------ smart (linked illustration) layers

    var smartVector: VectorDoc? = null

    fun renderSmart(l: RasterLayer, v: VectorDoc) {
        val bmp = doc.newBitmap()
        val c = Canvas(bmp)
        c.concat(l.smartMatrix)
        c.scale(doc.width.toFloat() / v.width, doc.height.toFloat() / v.height)
        v.render(c, l.smartBg)
        l.bitmap = bmp; l.version++
    }

    fun refreshSmartLayers() {
        val v = smartVector ?: return
        var any = false
        for (l in doc.layers) if (l.smart) { renderSmart(l, v); any = true }
        if (any) { onLayersChanged?.invoke(); invalidate() }
    }

    fun rasterize(l: RasterLayer) {
        pushUndo(); l.smart = false; l.version++; onLayersChanged?.invoke()
    }

    // ------------------------------------------------------------------ filter preview

    var filterPreview: Bitmap? = null
        private set
    private var previewScale = 1f
    private var previewSource: Bitmap? = null
    private var previewSel: ByteArray? = null
    @Volatile private var previewSeq = 0

    /** Prepares a low-res proxy of the active layer for fast live previews. */
    fun startPreview(): Boolean {
        val l = doc.activeLayer ?: return false
        val maxSide = 1100
        previewScale = min(1f, maxSide.toFloat() / max(doc.width, doc.height))
        val w = max(1, (doc.width * previewScale).roundToInt()); val h = max(1, (doc.height * previewScale).roundToInt())
        previewSource = if (previewScale < 1f) Bitmap.createScaledBitmap(l.bitmap, w, h, true) else l.bitmap.copy(Bitmap.Config.ARGB_8888, false)
        previewSel = doc.selection?.let { s ->
            val small = Bitmap.createScaledBitmap(s, w, h, true)
            val tmp = RasterDoc(w, h); tmp.selection = small; tmp.selectionBytes()
        }
        filterPreview = previewSource
        invalidate()
        return true
    }

    fun updatePreview(op: ImageOp, v: FloatArray, colors: IntArray) {
        val src = previewSource ?: return
        val seq = ++previewSeq
        val scale = previewScale
        val sel = previewSel
        worker.execute {
            if (seq != previewSeq) return@execute
            val out = try { op.apply(src, v, colors, scale) } catch (e: Throwable) { null } ?: return@execute
            val res = if (sel != null) blendWithMask(src, out, sel) else out
            main.post { if (seq == previewSeq && previewSource === src) { filterPreview = res; invalidate() } }
        }
    }

    fun endPreview() {
        previewSeq++
        filterPreview = null; previewSource = null; previewSel = null; invalidate()
    }

    fun applyOp(op: ImageOp, v: FloatArray, colors: IntArray, done: () -> Unit) {
        val l = doc.activeLayer ?: return
        val src = l.bitmap
        val sel = doc.selectionBytes()
        onBusy?.invoke(true)
        worker.execute {
            val result = try {
                val out = op.apply(src, v, colors, 1f)
                if (sel != null) blendWithMask(src, out, sel) else out
            } catch (e: OutOfMemoryError) { null }
            main.post {
                onBusy?.invoke(false)
                endPreview()
                if (result == null) onMessage?.invoke("Not enough memory for this filter")
                else replacePixels(l, result)
                done()
            }
        }
    }

    private fun blendWithMask(orig: Bitmap, out: Bitmap, sel: ByteArray): Bitmap {
        val a = pixelsOf(orig); val b = pixelsOf(out)
        for (i in a.indices) {
            val m = sel[i].toInt() and 255
            if (m == 255) continue
            if (m == 0) { b[i] = a[i]; continue }
            val x = a[i]; val y = b[i]
            fun ch(s: Int) = (((x shr s) and 255) * (255 - m) + ((y shr s) and 255) * m) / 255
            b[i] = (ch(24) shl 24) or (ch(16) shl 16) or (ch(8) shl 8) or ch(0)
        }
        return bitmapOf(b, orig.width, orig.height)
    }

    // ------------------------------------------------------------------ rendering

    override fun drawDocument(canvas: Canvas) {
        val w = doc.width.toFloat(); val h = doc.height.toFloat()
        canvas.saveLayer(0f, 0f, w, h, null)
        doc.layers.forEachIndexed { i, l ->
            if (!l.visible) return@forEachIndexed
            if (i == doc.active && (strokeActive || filterPreview != null || pendingMatrix != null || (dragging && gestureTool == PTool.GRADIENT))) {
                drawActive(canvas, l)
            } else doc.drawLayers(canvas, listOf(l))
        }
        canvas.restore()
    }

    private fun drawActive(c: Canvas, l: RasterLayer) {
        val w = doc.width.toFloat(); val h = doc.height.toFloat()
        c.saveLayer(0f, 0f, w, h, doc.layerPaint(l))
        val m = pendingMatrix
        val fp = filterPreview
        when {
            fp != null -> c.drawBitmap(fp, null, RectF(0f, 0f, w, h), bmpPaint)
            m != null -> c.drawBitmap(l.bitmap, m, bmpPaint)
            else -> c.drawBitmap(l.bitmap, 0f, 0f, null)
        }
        val onMask = editMask && l.mask != null
        if (strokeActive && !onMask && strokeTool != PTool.SMUDGE) {
            val p = Paint().apply {
                alpha = (strokeSettings.opacity * 255).roundToInt()
                if (strokeTool == PTool.ERASER) xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_OUT)
            }
            c.saveLayer(0f, 0f, w, h, p)
            drawDabs(c, strokeColor(), if (strokeTool == PTool.CLONE) cloneShader else null)
            doc.selection?.let { c.drawBitmap(it, 0f, 0f, dstIn) }
            c.restore()
        }
        if (dragging && gestureTool == PTool.GRADIENT) {
            c.saveLayer(0f, 0f, w, h, Paint().apply { alpha = (gradientOpacity * 255).roundToInt() })
            c.drawRect(0f, 0f, w, h, Paint().apply { shader = gradientShader() })
            doc.selection?.let { c.drawBitmap(it, 0f, 0f, dstIn) }
            c.restore()
        }
        val mask = l.mask
        if (mask != null && l.maskEnabled) {
            c.saveLayer(0f, 0f, w, h, dstIn)
            if (m != null) c.drawBitmap(mask, m, bmpPaint) else c.drawBitmap(mask, 0f, 0f, null)
            if (strokeActive && onMask) {
                val p = Paint().apply {
                    alpha = (strokeSettings.opacity * 255).roundToInt()
                    xfermode = PorterDuffXfermode(if (maskHides()) PorterDuff.Mode.DST_OUT else PorterDuff.Mode.SRC_OVER)
                }
                c.saveLayer(0f, 0f, w, h, p)
                drawDabs(c, Color.BLACK, null)
                doc.selection?.let { c.drawBitmap(it, 0f, 0f, dstIn) }
                c.restore()
            }
            c.restore()
        }
        c.restore()
    }

    private var antsPhase = 0f

    override fun drawDocOverlay(canvas: Canvas) {
        val z = zoom
        val px1 = resources.displayMetrics.density / z
        overlay.reset(); overlay.isAntiAlias = true

        // selection: dim outside + marching ants
        val sel = doc.selection
        if (sel != null) {
            val w = doc.width.toFloat(); val h = doc.height.toFloat()
            canvas.saveLayer(0f, 0f, w, h, null)
            overlay.color = 0x55203050
            canvas.drawRect(0f, 0f, w, h, overlay)
            canvas.drawBitmap(sel, 0f, 0f, Paint().apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_OUT) })
            canvas.restore()
            doc.selectionPath?.let { p ->
                overlay.style = Paint.Style.STROKE; overlay.strokeWidth = 1.5f * px1
                overlay.color = Color.WHITE; canvas.drawPath(p, overlay)
                overlay.color = Color.BLACK
                overlay.pathEffect = DashPathEffect(floatArrayOf(6 * px1, 6 * px1), antsPhase)
                canvas.drawPath(p, overlay)
                overlay.pathEffect = null
                antsPhase += 0.6f * px1
                postInvalidateDelayed(80)
            }
        }
        overlay.style = Paint.Style.STROKE
        overlay.strokeWidth = 1.5f * px1

        if (dragging && gestureTool == PTool.MARQUEE) {
            overlay.color = Theme.SELECTION
            val r = RectF(min(startX, curX), min(startY, curY), max(startX, curX), max(startY, curY))
            if (marqueeEllipse) canvas.drawOval(r, overlay) else canvas.drawRect(r, overlay)
        }
        if (dragging && gestureTool == PTool.LASSO) { overlay.color = Theme.SELECTION; canvas.drawPath(lassoPath, overlay) }
        if (dragging && gestureTool == PTool.GRADIENT) {
            overlay.color = Color.WHITE; canvas.drawLine(startX, startY, curX, curY, overlay)
            canvas.drawCircle(startX, startY, 5 * px1, overlay); canvas.drawCircle(curX, curY, 5 * px1, overlay)
        }

        // free transform box
        if (pendingMatrix != null && tool == PTool.MOVE) {
            val pts = transformedCorners()
            overlay.color = Theme.SELECTION
            val box = Path(); box.moveTo(pts[0], pts[1]); box.lineTo(pts[2], pts[3]); box.lineTo(pts[4], pts[5]); box.lineTo(pts[6], pts[7]); box.close()
            canvas.drawPath(box, overlay)
            val rh = rotateHandle(pts)
            canvas.drawLine(pts[8], pts[9], rh.x, rh.y, overlay)
            val hs = 6 * px1
            for (i in 0 until 8) {
                overlay.style = Paint.Style.FILL; overlay.color = Color.WHITE
                canvas.drawRect(pts[i * 2] - hs, pts[i * 2 + 1] - hs, pts[i * 2] + hs, pts[i * 2 + 1] + hs, overlay)
                overlay.style = Paint.Style.STROKE; overlay.color = Theme.SELECTION
                canvas.drawRect(pts[i * 2] - hs, pts[i * 2 + 1] - hs, pts[i * 2] + hs, pts[i * 2 + 1] + hs, overlay)
            }
            overlay.style = Paint.Style.FILL; overlay.color = Color.WHITE; canvas.drawCircle(rh.x, rh.y, hs * 1.3f, overlay)
            overlay.style = Paint.Style.STROKE; overlay.color = Theme.SELECTION; canvas.drawCircle(rh.x, rh.y, hs * 1.3f, overlay)
        }

        // clone source marker
        val cs = cloneSource
        if (cs != null && tool == PTool.CLONE) {
            overlay.color = Color.WHITE
            val r = 10 * px1
            canvas.drawCircle(cs.x, cs.y, r, overlay)
            canvas.drawLine(cs.x - r * 1.6f, cs.y, cs.x + r * 1.6f, cs.y, overlay)
            canvas.drawLine(cs.x, cs.y - r * 1.6f, cs.x, cs.y + r * 1.6f, overlay)
        }

        // brush cursor while hovering with the stylus
        val hv = hover
        if (hv != null && (tool == PTool.BRUSH || tool == PTool.ERASER || tool == PTool.CLONE || tool == PTool.SMUDGE)) {
            val r = settingsFor(tool).size / 2f
            overlay.style = Paint.Style.STROKE
            overlay.color = Color.WHITE; canvas.drawCircle(hv.x, hv.y, r, overlay)
            overlay.color = Color.BLACK; canvas.drawCircle(hv.x, hv.y, r + px1, overlay)
            if (tool == PTool.CLONE && cloneAligned) {
                overlay.color = Color.WHITE; canvas.drawCircle(hv.x + cloneOffX, hv.y + cloneOffY, r, overlay)
            }
        }
    }

    // ------------------------------------------------------------------ layer commands

    fun addEmptyLayer() {
        commitTransform(); pushUndo()
        doc.addLayer(doc.uniqueName("Layer"))
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    /** Adds an image as a new layer, scaled to fit inside the canvas and centred. */
    fun addImageLayer(name: String, src: Bitmap, cover: Boolean = false) {
        commitTransform(); pushUndo()
        val bmp = doc.newBitmap()
        val s = if (cover) max(doc.width.toFloat() / src.width, doc.height.toFloat() / src.height)
        else min(1f, min(doc.width.toFloat() / src.width, doc.height.toFloat() / src.height))
        val m = Matrix()
        m.postScale(s, s)
        m.postTranslate((doc.width - src.width * s) / 2f, (doc.height - src.height * s) / 2f)
        Canvas(bmp).drawBitmap(src, m, Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG))
        doc.addLayer(name, bmp)
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun addSmartLayer(v: VectorDoc, withBackground: Boolean) {
        commitTransform(); pushUndo()
        val l = doc.addLayer("Illustration (linked)")
        l.smart = true; l.smartBg = withBackground
        smartVector = v
        renderSmart(l, v)
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun duplicateLayer() {
        val l = doc.activeLayer ?: return
        commitTransform(); pushUndo()
        val c = doc.addLayer(l.name + " copy", l.bitmap.copy(Bitmap.Config.ARGB_8888, true))
        c.opacity = l.opacity; c.blend = l.blend; c.mask = l.mask?.copy(Bitmap.Config.ALPHA_8, true); c.maskEnabled = l.maskEnabled
        c.smart = l.smart; c.smartBg = l.smartBg; c.smartMatrix.set(l.smartMatrix)
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun deleteLayer() {
        if (doc.layers.size <= 1) { onMessage?.invoke("Can't delete the only layer"); return }
        commitTransform(); pushUndo()
        doc.layers.removeAt(doc.active)
        doc.active = doc.active.coerceIn(0, doc.layers.size - 1)
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun moveLayer(dir: Int) {
        val i = doc.active; val j = i + dir
        if (j !in doc.layers.indices) return
        commitTransform(); pushUndo()
        val t = doc.layers[i]; doc.layers[i] = doc.layers[j]; doc.layers[j] = t
        doc.active = j
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun mergeDown() {
        val i = doc.active
        if (i <= 0) { onMessage?.invoke("No layer below to merge into"); return }
        commitTransform()
        val top = doc.layers[i]; val below = doc.layers[i - 1]
        pushUndo(below.bitmap.byteCount.toLong())
        val nb = doc.newBitmap()
        val c = Canvas(nb)
        doc.drawLayers(c, listOf(RasterLayer("tmp", below.bitmap).also { it.mask = below.mask; it.maskEnabled = below.maskEnabled }))
        doc.drawLayers(c, listOf(top))
        below.bitmap = nb; below.mask = null; below.smart = false; below.version++
        doc.layers.removeAt(i); doc.active = i - 1
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun flattenImage() {
        commitTransform(); pushUndo()
        val flat = doc.flatten()
        doc.layers.clear()
        doc.addLayer("Background", flat, 0)
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun addMask(fromSelection: Boolean) {
        val l = doc.activeLayer ?: return
        pushUndo()
        l.mask = if (fromSelection && doc.selection != null) doc.selection!!.copy(Bitmap.Config.ALPHA_8, true) else doc.newMask(255)
        l.maskEnabled = true; editMask = true; l.version++
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun deleteMask(apply: Boolean) {
        val l = doc.activeLayer ?: return
        val m = l.mask ?: return
        if (apply) {
            if (l.smart) { onNeedsRasterize?.invoke(l) { deleteMask(true) }; return }
            pushUndo(l.bitmap.byteCount.toLong())
            val nb = l.bitmap.copy(Bitmap.Config.ARGB_8888, true)
            Canvas(nb).drawBitmap(m, 0f, 0f, dstIn)
            l.bitmap = nb
        } else pushUndo()
        l.mask = null; editMask = false; l.version++
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun invertMask() {
        val l = doc.activeLayer ?: return
        val m = l.mask ?: return
        beginPixelEdit(l, true)
        val inv = doc.newMask(255)
        Canvas(inv).drawBitmap(m, 0f, 0f, Paint().apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_OUT) })
        l.mask = inv
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun setLayerProps(l: RasterLayer, record: Boolean, block: (RasterLayer) -> Unit) {
        if (record) pushUndo()
        block(l)
        onChanged?.invoke(); invalidate()
    }

    /** Flips or rotates the whole canvas (all layers). */
    fun transformCanvas(kind: Int) {
        commitTransform(); pushUndo()
        val w = doc.width.toFloat(); val h = doc.height.toFloat()
        val m = Matrix()
        when (kind) {
            0 -> m.setScale(-1f, 1f, w / 2, h / 2)
            1 -> m.setScale(1f, -1f, w / 2, h / 2)
            2 -> m.setRotate(90f, w / 2, h / 2)
            3 -> m.setRotate(-90f, w / 2, h / 2)
        }
        for (l in doc.layers) {
            if (l.smart) { l.smartMatrix.postConcat(m); smartVector?.let { renderSmart(l, it) } }
            else {
                val nb = doc.newBitmap(); Canvas(nb).drawBitmap(l.bitmap, m, Paint(Paint.FILTER_BITMAP_FLAG)); l.bitmap = nb
            }
            l.mask?.let { mk -> val nm = doc.newMask(0); Canvas(nm).drawBitmap(mk, m, Paint(Paint.FILTER_BITMAP_FLAG)); l.mask = nm }
            l.version++
        }
        deselect()
        onLayersChanged?.invoke(); onChanged?.invoke(); invalidate()
    }

    fun release() { worker.shutdownNow() }
}
