package com.coverstudio.app.vector

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PointF
import android.graphics.RectF
import com.coverstudio.app.common.BaseCanvasView
import com.coverstudio.app.common.PointerInfo
import com.coverstudio.app.common.Sample
import com.coverstudio.app.ui.Theme
import com.coverstudio.app.ui.promptText
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

enum class VTool(val icon: String, val label: String) {
    SELECT("select", "Select / transform (V)"),
    DIRECT("direct", "Direct selection – edit anchor points (A)"),
    PEN("pen", "Pen – tap for corners, drag for curves (P)"),
    PENCIL("pencil", "Pencil – freehand smooth path (N)"),
    BRUSH("brush", "Pressure brush (B)"),
    RECT("rect", "Rectangle (M)"),
    ELLIPSE("ellipse", "Ellipse (L)"),
    POLY("star", "Polygon / Star"),
    LINE("line", "Line (\\)"),
    TEXT("text", "Type (T)"),
    ERASER("eraser", "Object eraser (E)"),
    EYEDROPPER("eyedropper", "Eyedropper (I)"),
    HAND("hand", "Hand / pan (H)")
}

class VectorCanvasView(context: Context) : BaseCanvasView(context) {
    var doc = VectorDoc(3000, 3000)
        private set

    var tool = VTool.BRUSH
        set(v) {
            if (field == VTool.PEN && v != VTool.PEN) finishPen()
            field = v; activeNode = -1; invalidate()
        }

    // ---- current style for new objects
    var fill = Fill(Fill.SOLID, Color.BLACK)
    var stroke = StrokeStyle(false, Color.BLACK, 12f)
    var brushSize = 24f
    var brushMinRatio = 0.15f
    var brushSmoothing = 0.45f
    var brushTaper = true
    var eraserSize = 40f
    var polySides = 5
    var polyStar = true
    var polyInner = 0.45f
    var cornerRadius = 0f
    var constrain = false
    var proportional = true
    var textFont = "sans-serif"
    var textBold = true
    var textItalic = false

    val selection = ArrayList<Shape>()
    var activeNode = -1

    var onSelectionChanged: (() -> Unit)? = null
    var onDocChanged: (() -> Unit)? = null
    var onColorPicked: ((Int) -> Unit)? = null

    // ---- history
    private val undoStack = ArrayList<VectorDoc>()
    private val redoStack = ArrayList<VectorDoc>()
    val canUndo get() = undoStack.isNotEmpty()
    val canRedo get() = redoStack.isNotEmpty()

    private val paint = Paint()
    private val overlay = Paint(Paint.ANTI_ALIAS_FLAG)

    fun setDocument(d: VectorDoc) {
        doc = d; selection.clear(); undoStack.clear(); redoStack.clear()
        setDocSize(d.width, d.height)
        onSelectionChanged?.invoke(); invalidate()
    }

    /** Record the current state so the next change can be undone. */
    fun checkpoint() {
        undoStack.add(doc.snapshot())
        if (undoStack.size > 80) undoStack.removeAt(0)
        redoStack.clear()
    }

    fun changed() {
        onDocChanged?.invoke(); invalidate()
    }

    fun undo() {
        finishPen()
        if (undoStack.isEmpty()) return
        redoStack.add(doc.snapshot())
        doc.restoreFrom(undoStack.removeAt(undoStack.size - 1))
        selection.clear(); onSelectionChanged?.invoke(); changed()
    }

    fun redo() {
        if (redoStack.isEmpty()) return
        undoStack.add(doc.snapshot())
        doc.restoreFrom(redoStack.removeAt(redoStack.size - 1))
        selection.clear(); onSelectionChanged?.invoke(); changed()
    }

    fun select(list: List<Shape>) {
        selection.clear(); selection.addAll(list); activeNode = -1
        list.firstOrNull()?.let { s -> doc.layers.indexOfFirst { it.shapes.contains(s) }.takeIf { it >= 0 }?.let { doc.active = it } }
        onSelectionChanged?.invoke(); invalidate()
    }

    fun selectionBounds(): RectF? {
        if (selection.isEmpty()) return null
        val r = RectF(selection[0].docBounds())
        for (i in 1 until selection.size) r.union(selection[i].docBounds())
        return r
    }

    override fun drawDocument(canvas: Canvas) {
        doc.render(canvas)
    }

    override fun toolIsPan() = tool == VTool.HAND

    // ------------------------------------------------------------------ gesture state
    private enum class Drag { NONE, MOVE, SCALE, ROTATE, MARQUEE, NODE, HANDLE_IN, HANDLE_OUT, CREATE, BRUSH, PENCIL, PEN_HANDLE, ERASE, PICK }
    private var drag = Drag.NONE
    private var gestureTool = VTool.SELECT
    private var startX = 0f
    private var startY = 0f
    private var curX = 0f
    private var curY = 0f
    private var handleIdx = -1
    private var startBounds = RectF()
    private val startMatrices = ArrayList<Matrix>()
    private var pendingSnapshot: VectorDoc? = null
    private var creating: Shape? = null
    private var penShape: PathShape? = null
    private var penNode: Node? = null
    private val stroke3 = FloatArrayList()
    private var smX = 0f
    private var smY = 0f
    private var smP = 1f
    private var snapX: Float? = null
    private var snapY: Float? = null
    private var nodeShape: PathShape? = null
    private var nodeStart: Node? = null

    private fun commitPending() {
        pendingSnapshot?.let {
            undoStack.add(it); if (undoStack.size > 80) undoStack.removeAt(0); redoStack.clear()
            pendingSnapshot = null
        }
    }

    override fun onToolDown(s: Sample, info: PointerInfo) {
        startX = s.x; startY = s.y; curX = s.x; curY = s.y
        pendingSnapshot = null
        gestureTool = when {
            info.isEraserEnd -> VTool.ERASER
            info.sideButton -> VTool.EYEDROPPER
            else -> tool
        }
        drag = Drag.NONE
        when (gestureTool) {
            VTool.SELECT -> selectDown(s)
            VTool.DIRECT -> directDown(s)
            VTool.PEN -> penDown(s)
            VTool.PENCIL -> {
                drag = Drag.PENCIL; stroke3.clear(); stroke3.add(s.x, s.y, 1f)
            }
            VTool.BRUSH -> {
                drag = Drag.BRUSH
                if (selection.isNotEmpty()) { selection.clear(); onSelectionChanged?.invoke() }
                stroke3.clear(); smX = s.x; smY = s.y; smP = s.pressure
                stroke3.add(s.x, s.y, s.pressure)
                val b = BrushShape(stroke3.toArray(), brushSize, brushMinRatio, brushTaper)
                b.fill = if (fill.type == Fill.NONE) Fill(Fill.SOLID, stroke.color) else fill.copy()
                pendingSnapshot = doc.snapshot()
                if (doc.activeLayer.locked || !doc.activeLayer.visible) { drag = Drag.NONE; return }
                doc.activeLayer.shapes.add(b); creating = b
            }
            VTool.RECT, VTool.ELLIPSE, VTool.POLY, VTool.LINE -> {
                if (doc.activeLayer.locked) return
                drag = Drag.CREATE; creating = null
            }
            VTool.TEXT -> {}
            VTool.ERASER -> { drag = Drag.ERASE; pendingSnapshot = doc.snapshot(); eraseAt(s.x, s.y) }
            VTool.EYEDROPPER -> { drag = Drag.PICK; pick(s.x, s.y) }
            VTool.HAND -> {}
        }
        invalidate()
    }

    override fun onToolMove(s: Sample) {
        curX = s.x; curY = s.y
        when (drag) {
            Drag.MOVE -> moveSelection()
            Drag.SCALE -> scaleSelection()
            Drag.ROTATE -> rotateSelection()
            Drag.MARQUEE -> {}
            Drag.NODE, Drag.HANDLE_IN, Drag.HANDLE_OUT -> dragNode()
            Drag.CREATE -> updateCreate()
            Drag.BRUSH -> {
                val sm = brushSmoothing
                smX = smX * sm + s.x * (1 - sm); smY = smY * sm + s.y * (1 - sm)
                smP = smP * 0.5f + s.pressure * 0.5f
                val minStep = max(docLen(1.2f), brushSize * 0.04f)
                if (stroke3.size >= 3 && hypot(smX - stroke3[stroke3.size - 3], smY - stroke3[stroke3.size - 2]) < minStep) return
                stroke3.add(smX, smY, smP)
                replaceCreatingBrush()
            }
            Drag.PENCIL -> {
                if (hypot(s.x - stroke3[stroke3.size - 3], s.y - stroke3[stroke3.size - 2]) >= docLen(1.5f)) stroke3.add(s.x, s.y, 1f)
            }
            Drag.PEN_HANDLE -> {
                val n = penNode ?: return
                if (hypot(s.x - n.x, s.y - n.y) > docLen(4f)) {
                    n.outX = s.x; n.outY = s.y; n.inX = 2 * n.x - s.x; n.inY = 2 * n.y - s.y; n.smooth = true
                    penShape?.invalidate()
                }
            }
            Drag.ERASE -> eraseAt(s.x, s.y)
            Drag.PICK -> pick(s.x, s.y)
            Drag.NONE -> {}
        }
        invalidate()
    }

    override fun onToolUp(s: Sample) {
        onToolMove(s)
        when (drag) {
            Drag.MARQUEE -> {
                val r = RectF(min(startX, curX), min(startY, curY), max(startX, curX), max(startY, curY))
                val hits = ArrayList<Shape>()
                for (l in doc.layers) if (l.visible && !l.locked) for (sh in l.shapes) if (RectF.intersects(r, sh.docBounds())) hits.add(sh)
                select(hits)
            }
            Drag.BRUSH -> {
                val c = creating
                if (c != null && stroke3.size >= 3) { commitPending(); changed() }
                creating = null
            }
            Drag.PENCIL -> finishPencil()
            Drag.CREATE -> {
                creating?.let { select(listOf(it)); changed() }
                creating = null
            }
            Drag.MOVE, Drag.SCALE, Drag.ROTATE, Drag.NODE, Drag.HANDLE_IN, Drag.HANDLE_OUT -> {
                if (pendingSnapshot == null) changed()
                onSelectionChanged?.invoke()
            }
            Drag.ERASE -> { if (pendingSnapshot == null) changed() }
            Drag.NONE -> if (gestureTool == VTool.TEXT) textTap(s.x, s.y)
            else -> {}
        }
        pendingSnapshot = null
        snapX = null; snapY = null
        drag = Drag.NONE
        invalidate()
    }

    override fun onToolCancel() {
        when (drag) {
            Drag.BRUSH, Drag.CREATE -> creating?.let { c -> doc.layers.forEach { it.shapes.remove(c) } }
            else -> pendingSnapshot?.let { doc.restoreFrom(it); selection.clear(); onSelectionChanged?.invoke() }
        }
        creating = null; pendingSnapshot = null; drag = Drag.NONE
        invalidate()
    }

    // ------------------------------------------------------------------ select tool

    /** Handle positions: 0..3 corners (tl,tr,br,bl), 4..7 edges (t,r,b,l), 8 rotate. */
    private fun handlePoints(b: RectF): Array<PointF> {
        val rot = docLen(28f)
        return arrayOf(
            PointF(b.left, b.top), PointF(b.right, b.top), PointF(b.right, b.bottom), PointF(b.left, b.bottom),
            PointF(b.centerX(), b.top), PointF(b.right, b.centerY()), PointF(b.centerX(), b.bottom), PointF(b.left, b.centerY()),
            PointF(b.centerX(), b.top - rot)
        )
    }

    private fun hitTopShape(x: Float, y: Float): Shape? {
        val tol = docLen(10f)
        for (li in doc.layers.indices.reversed()) {
            val l = doc.layers[li]
            if (!l.visible || l.locked) continue
            for (i in l.shapes.indices.reversed()) if (l.shapes[i].hit(x, y, tol)) return l.shapes[i]
        }
        return null
    }

    private fun selectDown(s: Sample) {
        val b = selectionBounds()
        if (b != null) {
            val hp = handlePoints(b)
            val tol = docLen(20f)
            for (i in hp.indices.reversed()) {
                if (hypot(hp[i].x - s.x, hp[i].y - s.y) < tol) {
                    handleIdx = i
                    beginTransform(if (i == 8) Drag.ROTATE else Drag.SCALE, b)
                    return
                }
            }
        }
        val hit = hitTopShape(s.x, s.y)
        if (hit != null) {
            if (!selection.contains(hit)) select(listOf(hit))
            beginTransform(Drag.MOVE, selectionBounds()!!)
            return
        }
        if (b != null && b.contains(s.x, s.y) && selection.size > 1) {
            beginTransform(Drag.MOVE, b); return
        }
        select(emptyList())
        drag = Drag.MARQUEE
    }

    private fun beginTransform(d: Drag, b: RectF) {
        drag = d
        startBounds.set(b)
        startMatrices.clear()
        selection.forEach { startMatrices.add(Matrix(it.matrix)) }
        pendingSnapshot = doc.snapshot()
    }

    private fun applyDelta(delta: Matrix) {
        commitPending()
        selection.forEachIndexed { i, sh ->
            sh.matrix.set(startMatrices[i]); sh.matrix.postConcat(delta); sh.invalidateTransform()
        }
    }

    private fun moveSelection() {
        var dx = curX - startX; var dy = curY - startY
        if (pendingSnapshot != null && hypot(dx, dy) < docLen(3f)) return
        // snap selection centre / edges to the artboard centre and edges
        val tol = docLen(8f)
        snapX = null; snapY = null
        val cx = startBounds.centerX() + dx; val cy = startBounds.centerY() + dy
        val candidatesX = floatArrayOf(doc.width / 2f, 0f, doc.width.toFloat())
        val candidatesY = floatArrayOf(doc.height / 2f, 0f, doc.height.toFloat())
        val xs = floatArrayOf(cx, startBounds.left + dx, startBounds.right + dx)
        val ys = floatArrayOf(cy, startBounds.top + dy, startBounds.bottom + dy)
        loop@ for (c in candidatesX) for (x in xs) if (abs(x - c) < tol) { dx += c - x; snapX = c; break@loop }
        loop@ for (c in candidatesY) for (y in ys) if (abs(y - c) < tol) { dy += c - y; snapY = c; break@loop }
        val m = Matrix(); m.setTranslate(dx, dy)
        applyDelta(m)
    }

    private fun scaleSelection() {
        val b = startBounds
        val i = handleIdx
        val ax = when (i) { 0, 3, 7 -> b.right; 1, 2, 5 -> b.left; else -> b.centerX() }
        val ay = when (i) { 0, 1, 4 -> b.bottom; 2, 3, 6 -> b.top; else -> b.centerY() }
        val hx = when (i) { 0, 3, 7 -> b.left; 1, 2, 5 -> b.right; else -> b.centerX() }
        val hy = when (i) { 0, 1, 4 -> b.top; 2, 3, 6 -> b.bottom; else -> b.centerY() }
        var sx = if (i == 4 || i == 6 || abs(hx - ax) < 1e-3f) 1f else (curX - ax) / (hx - ax)
        var sy = if (i == 5 || i == 7 || abs(hy - ay) < 1e-3f) 1f else (curY - ay) / (hy - ay)
        if (i < 4 && proportional) {
            val sc = if (abs(sx) > abs(sy)) sx else sy
            sx = sc; sy = sc
        }
        if (abs(sx) < 0.001f) sx = 0.001f
        if (abs(sy) < 0.001f) sy = 0.001f
        val m = Matrix(); m.setScale(sx, sy, ax, ay)
        applyDelta(m)
    }

    private fun rotateSelection() {
        val cx = startBounds.centerX(); val cy = startBounds.centerY()
        val a0 = atan2(startY - cy, startX - cx); val a1 = atan2(curY - cy, curX - cx)
        var deg = Math.toDegrees((a1 - a0).toDouble()).toFloat()
        val snapped = (deg / 15f).roundToInt() * 15f
        if (abs(deg - snapped) < 3f) deg = snapped
        val m = Matrix(); m.setRotate(deg, cx, cy)
        applyDelta(m)
    }

    // ------------------------------------------------------------------ direct selection

    fun nodeTarget(): PathShape? = selection.singleOrNull() as? PathShape

    private fun docNode(sh: Shape, x: Float, y: Float): PointF {
        val p = floatArrayOf(x, y); sh.matrix.mapPoints(p); return PointF(p[0], p[1])
    }

    private fun directDown(s: Sample) {
        val sh = selection.singleOrNull()
        if (sh != null) {
            val path = if (sh is PathShape) sh else toPathShape(sh)
            if (path != null) {
                val tol = docLen(18f)
                // handles of the active node first
                if (activeNode in path.nodes.indices) {
                    val n = path.nodes[activeNode]
                    val hin = docNode(sh, n.inX, n.inY); val hout = docNode(sh, n.outX, n.outY)
                    if (n.hasOut() && hypot(hout.x - s.x, hout.y - s.y) < tol) { beginNodeDrag(sh, path, Drag.HANDLE_OUT); return }
                    if (n.hasIn() && hypot(hin.x - s.x, hin.y - s.y) < tol) { beginNodeDrag(sh, path, Drag.HANDLE_IN); return }
                }
                for (i in path.nodes.indices.reversed()) {
                    val p = docNode(sh, path.nodes[i].x, path.nodes[i].y)
                    if (hypot(p.x - s.x, p.y - s.y) < tol) {
                        activeNode = i
                        beginNodeDrag(sh, path, Drag.NODE)
                        onSelectionChanged?.invoke()
                        return
                    }
                }
            }
        }
        val hit = hitTopShape(s.x, s.y)
        select(if (hit != null) listOf(hit) else emptyList())
    }

    private fun beginNodeDrag(sh: Shape, path: PathShape, d: Drag) {
        pendingSnapshot = doc.snapshot()
        var target = path
        if (sh !is PathShape) {
            // convert live shape to an editable path the first time a node is dragged
            val layer = doc.findLayerOf(sh) ?: return
            layer.shapes[layer.shapes.indexOf(sh)] = path
            selection.clear(); selection.add(path)
            target = path
        }
        nodeShape = target
        nodeStart = target.nodes.getOrNull(activeNode)?.copy()
        drag = d
    }

    private fun dragNode() {
        val sh = nodeShape ?: return
        val n = sh.nodes.getOrNull(activeNode) ?: return
        val st = nodeStart ?: return
        if (pendingSnapshot != null && hypot(curX - startX, curY - startY) < docLen(3f)) return
        commitPending()
        val inv = Matrix(); sh.matrix.invert(inv)
        val p = floatArrayOf(curX, curY, startX, startY); inv.mapPoints(p)
        val dx = p[0] - p[2]; val dy = p[1] - p[3]
        when (drag) {
            Drag.NODE -> {
                n.x = st.x + dx; n.y = st.y + dy; n.inX = st.inX + dx; n.inY = st.inY + dy; n.outX = st.outX + dx; n.outY = st.outY + dy
            }
            Drag.HANDLE_OUT -> {
                n.outX = p[0]; n.outY = p[1]
                if (n.smooth) mirror(n.x, n.y, n.outX, n.outY, st.inX, st.inY, n, true)
            }
            Drag.HANDLE_IN -> {
                n.inX = p[0]; n.inY = p[1]
                if (n.smooth) mirror(n.x, n.y, n.inX, n.inY, st.outX, st.outY, n, false)
            }
            else -> {}
        }
        sh.invalidate()
    }

    /** Keeps the opposite handle colinear, preserving its original length. */
    private fun mirror(cx: Float, cy: Float, hx: Float, hy: Float, ox: Float, oy: Float, n: Node, setIn: Boolean) {
        val len = hypot(ox - cx, oy - cy).let { if (it < 0.01f) hypot(hx - cx, hy - cy) else it }
        val d = hypot(hx - cx, hy - cy); if (d < 0.01f) return
        val nx = cx - (hx - cx) / d * len; val ny = cy - (hy - cy) / d * len
        if (setIn) { n.inX = nx; n.inY = ny } else { n.outX = nx; n.outY = ny }
    }

    fun deleteActiveNode() {
        val p = nodeTarget() ?: return
        if (activeNode !in p.nodes.indices) return
        checkpoint()
        p.nodes.removeAt(activeNode); activeNode = -1
        if (p.nodes.size < 2) { doc.findLayerOf(p)?.shapes?.remove(p); selection.clear() }
        p.invalidate(); onSelectionChanged?.invoke(); changed()
    }

    fun toggleNodeSmooth() {
        val p = nodeTarget() ?: return
        val n = p.nodes.getOrNull(activeNode) ?: return
        checkpoint()
        if (n.hasIn() || n.hasOut()) {
            n.inX = n.x; n.inY = n.y; n.outX = n.x; n.outY = n.y; n.smooth = false
        } else {
            val i = activeNode
            val prev = p.nodes[(i - 1 + p.nodes.size) % p.nodes.size]
            val next = p.nodes[(i + 1) % p.nodes.size]
            val tx = (next.x - prev.x) / 6f; val ty = (next.y - prev.y) / 6f
            n.inX = n.x - tx; n.inY = n.y - ty; n.outX = n.x + tx; n.outY = n.y + ty; n.smooth = true
        }
        p.invalidate(); changed()
    }

    fun addNodeAfterActive() {
        val p = nodeTarget() ?: return
        checkpoint()
        val idx = p.subdivide(if (activeNode in p.nodes.indices) activeNode else 0)
        if (idx >= 0) activeNode = idx
        changed()
    }

    fun togglePathClosed() {
        val p = nodeTarget() ?: return
        checkpoint(); p.closed = !p.closed; p.invalidate(); changed()
    }

    // ------------------------------------------------------------------ pen tool

    private fun penDown(s: Sample) {
        val tol = docLen(16f)
        val cur = penShape
        if (cur != null && cur.nodes.size >= 2 && hypot(cur.nodes[0].x - s.x, cur.nodes[0].y - s.y) < tol) {
            cur.closed = true; cur.invalidate(); finishPen(); return
        }
        if (cur == null) {
            if (doc.activeLayer.locked) return
            checkpoint()
            val p = PathShape(ArrayList(), false)
            p.fill = fill.copy(); p.stroke = stroke.copy()
            if (p.fill.type == Fill.NONE && !p.stroke.enabled) p.stroke.enabled = true
            doc.activeLayer.shapes.add(p)
            penShape = p
            select(listOf(p))
        }
        val n = Node(s.x, s.y)
        penShape!!.nodes.add(n)
        penShape!!.invalidate()
        penNode = n
        drag = Drag.PEN_HANDLE
    }

    val penActive get() = penShape != null

    fun finishPen() {
        val p = penShape ?: return
        penShape = null; penNode = null
        if (p.nodes.size < 2) {
            doc.layers.forEach { it.shapes.remove(p) }
            if (undoStack.isNotEmpty()) undoStack.removeAt(undoStack.size - 1)
            selection.clear()
        } else select(listOf(p))
        onSelectionChanged?.invoke()
        changed()
    }

    // ------------------------------------------------------------------ pencil / brush

    private fun replaceCreatingBrush() {
        val old = creating as? BrushShape ?: return
        val b = BrushShape(stroke3.toArray(), old.size, old.minRatio, old.taper)
        b.fill = old.fill; b.id = old.id
        val shapes = doc.activeLayer.shapes
        val i = shapes.indexOf(old)
        if (i >= 0) shapes[i] = b
        creating = b
    }

    private fun finishPencil() {
        if (stroke3.size < 6 || doc.activeLayer.locked) return
        val raw = ArrayList<PointF>()
        var i = 0
        while (i < stroke3.size) { raw.add(PointF(stroke3[i], stroke3[i + 1])); i += 3 }
        val simplified = rdp(raw, docLen(2.5f))
        val closed = simplified.size > 3 && hypot(simplified.first().x - simplified.last().x, simplified.first().y - simplified.last().y) < docLen(14f)
        val pts = if (closed) simplified.dropLast(1) else simplified
        val nodes = ArrayList<Node>()
        val n = pts.size
        for (k in 0 until n) {
            val p = pts[k]
            val prev = if (k > 0) pts[k - 1] else if (closed) pts[n - 1] else null
            val next = if (k < n - 1) pts[k + 1] else if (closed) pts[0] else null
            if (prev == null || next == null) { nodes.add(Node(p.x, p.y)); continue }
            val tx = (next.x - prev.x) / 6f; val ty = (next.y - prev.y) / 6f
            nodes.add(Node(p.x, p.y, p.x - tx, p.y - ty, p.x + tx, p.y + ty, true))
        }
        checkpoint()
        val shape = PathShape(nodes, closed)
        shape.stroke = stroke.copy().also { it.enabled = true; if (!stroke.enabled) it.color = fill.color }
        shape.fill = if (closed) fill.copy() else Fill(Fill.NONE)
        doc.activeLayer.shapes.add(shape)
        select(listOf(shape))
        changed()
    }

    private fun rdp(pts: List<PointF>, eps: Float): List<PointF> {
        if (pts.size < 3) return pts
        var maxD = 0f; var idx = 0
        val a = pts.first(); val b = pts.last()
        val len = hypot(b.x - a.x, b.y - a.y)
        for (i in 1 until pts.size - 1) {
            val p = pts[i]
            val d = if (len < 1e-3f) hypot(p.x - a.x, p.y - a.y)
            else abs((b.x - a.x) * (a.y - p.y) - (a.x - p.x) * (b.y - a.y)) / len
            if (d > maxD) { maxD = d; idx = i }
        }
        return if (maxD > eps) {
            val l = rdp(pts.subList(0, idx + 1), eps); val r = rdp(pts.subList(idx, pts.size), eps)
            l.dropLast(1) + r
        } else listOf(a, b)
    }

    // ------------------------------------------------------------------ shape creation

    private fun updateCreate() {
        var dx = curX - startX; var dy = curY - startY
        if (creating == null && hypot(dx, dy) < docLen(6f)) return
        if (constrain && tool != VTool.LINE) {
            val m = max(abs(dx), abs(dy)); dx = if (dx < 0) -m else m; dy = if (dy < 0) -m else m
        }
        val x0 = min(startX, startX + dx); val y0 = min(startY, startY + dy)
        val w = max(1f, abs(dx)); val h = max(1f, abs(dy))
        var sh = creating
        if (sh == null) {
            checkpoint()
            sh = when (tool) {
                VTool.RECT -> RectShape(w, h, cornerRadius)
                VTool.ELLIPSE -> EllipseShape(w, h)
                VTool.POLY -> PolyShape(w, h, polySides, polyStar, polyInner)
                else -> PathShape(mutableListOf(Node(startX, startY), Node(curX, curY)), false)
            }
            sh.fill = if (tool == VTool.LINE) Fill(Fill.NONE) else fill.copy()
            sh.stroke = stroke.copy()
            if (tool == VTool.LINE) { sh.stroke.enabled = true; if (!stroke.enabled) sh.stroke.color = fill.color }
            doc.activeLayer.shapes.add(sh)
            creating = sh
        }
        when (sh) {
            is RectShape -> { sh.w = w; sh.h = h; sh.matrix.setTranslate(x0, y0) }
            is EllipseShape -> { sh.w = w; sh.h = h; sh.matrix.setTranslate(x0, y0) }
            is PolyShape -> { sh.w = w; sh.h = h; sh.matrix.setTranslate(x0, y0) }
            is PathShape -> {
                var ex = curX; var ey = curY
                if (constrain) {
                    val ang = Math.round(Math.toDegrees(atan2((ey - startY).toDouble(), (ex - startX).toDouble())) / 45.0) * 45.0
                    val d = hypot(ex - startX, ey - startY)
                    ex = startX + (Math.cos(Math.toRadians(ang)) * d).toFloat(); ey = startY + (Math.sin(Math.toRadians(ang)) * d).toFloat()
                }
                sh.nodes[1].x = ex; sh.nodes[1].y = ey
                sh.nodes[1].inX = ex; sh.nodes[1].inY = ey; sh.nodes[1].outX = ex; sh.nodes[1].outY = ey
            }
        }
        sh.invalidate()
    }

    // ------------------------------------------------------------------ text

    var onEditText: ((TextShape?, Float, Float) -> Unit)? = null

    private fun textTap(x: Float, y: Float) {
        val hit = hitTopShape(x, y)
        if (hit is TextShape) { select(listOf(hit)); onEditText?.invoke(hit, x, y) }
        else onEditText?.invoke(null, x, y) ?: promptText(context, "Text", "", true) { addText(it, x, y) }
    }

    fun addText(t: String, x: Float, y: Float) {
        if (t.isBlank() || doc.activeLayer.locked) return
        checkpoint()
        val ts = TextShape(t, doc.width / 10f, textFont, textBold, textItalic)
        ts.fill = if (fill.type == Fill.NONE) Fill(Fill.SOLID, Color.BLACK) else fill.copy()
        ts.stroke = stroke.copy()
        ts.stroke.width = stroke.width.coerceAtMost(ts.size / 8f)
        val box = ts.localBox()
        ts.matrix.setTranslate(x - box.width() / 2, y - box.height() / 2)
        doc.activeLayer.shapes.add(ts)
        tool = VTool.SELECT
        select(listOf(ts))
        changed()
    }

    // ------------------------------------------------------------------ eraser & eyedropper

    private fun eraseAt(x: Float, y: Float) {
        val tol = max(eraserSize / 2f, docLen(4f))
        var removed = false
        for (l in doc.layers) {
            if (!l.visible || l.locked) continue
            val it = l.shapes.iterator()
            while (it.hasNext()) {
                val s = it.next()
                if (s.hit(x, y, tol)) {
                    if (!removed) commitPending()
                    it.remove(); selection.remove(s); removed = true
                }
            }
        }
        if (removed) onSelectionChanged?.invoke()
    }

    private fun pick(x: Float, y: Float) {
        val bmp = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        c.translate(-x, -y)
        doc.render(c)
        val color = bmp.getPixel(0, 0)
        bmp.recycle()
        onColorPicked?.invoke(color)
    }

    // ------------------------------------------------------------------ overlays

    override fun drawDocOverlay(canvas: Canvas) {
        val z = zoom
        overlay.reset(); overlay.isAntiAlias = true
        overlay.style = Paint.Style.STROKE
        overlay.strokeWidth = 1.5f / z * resources.displayMetrics.density

        // outline of selected shapes
        overlay.color = Theme.SELECTION
        for (s in selection) canvas.drawPath(s.docPath(), overlay)

        val b = selectionBounds()
        if (b != null && (tool == VTool.SELECT)) {
            canvas.drawRect(b, overlay)
            val hp = handlePoints(b)
            canvas.drawLine(b.centerX(), b.top, hp[8].x, hp[8].y, overlay)
            val hs = docLen(6f)
            for (i in hp.indices) {
                overlay.style = Paint.Style.FILL; overlay.color = Color.WHITE
                if (i == 8) canvas.drawCircle(hp[i].x, hp[i].y, hs * 1.2f, overlay)
                else canvas.drawRect(hp[i].x - hs, hp[i].y - hs, hp[i].x + hs, hp[i].y + hs, overlay)
                overlay.style = Paint.Style.STROKE; overlay.color = Theme.SELECTION
                if (i == 8) canvas.drawCircle(hp[i].x, hp[i].y, hs * 1.2f, overlay)
                else canvas.drawRect(hp[i].x - hs, hp[i].y - hs, hp[i].x + hs, hp[i].y + hs, overlay)
            }
        }

        // anchor points for direct selection / pen
        val nodeSh = if (tool == VTool.DIRECT || tool == VTool.PEN) (penShape ?: selection.singleOrNull()) else null
        if (nodeSh != null) {
            val path = if (nodeSh is PathShape) nodeSh else toPathShape(nodeSh)
            if (path != null) drawNodes(canvas, nodeSh, path)
        }

        if (drag == Drag.MARQUEE) {
            overlay.color = Theme.SELECTION
            overlay.pathEffect = DashPathEffect(floatArrayOf(docLen(5f), docLen(4f)), 0f)
            canvas.drawRect(min(startX, curX), min(startY, curY), max(startX, curX), max(startY, curY), overlay)
            overlay.pathEffect = null
        }

        // snapping guides
        overlay.color = 0xFFFF3DB4.toInt()
        snapX?.let { canvas.drawLine(it, 0f, it, doc.height.toFloat(), overlay) }
        snapY?.let { canvas.drawLine(0f, it, doc.width.toFloat(), it, overlay) }

        // stylus hover cursor
        val h = hover
        if (h != null && (tool == VTool.BRUSH || tool == VTool.ERASER)) {
            val r = if (tool == VTool.BRUSH) brushSize / 2f else eraserSize / 2f
            overlay.color = Color.WHITE; canvas.drawCircle(h.x, h.y, r, overlay)
            overlay.color = Color.BLACK; canvas.drawCircle(h.x, h.y, r + docLen(1f), overlay)
        }
    }

    private fun drawNodes(canvas: Canvas, sh: Shape, path: PathShape) {
        val hs = docLen(5f)
        path.nodes.forEachIndexed { i, n ->
            val p = docNode(sh, n.x, n.y)
            if (i == activeNode || (sh === penShape && i == path.nodes.size - 1)) {
                overlay.style = Paint.Style.STROKE; overlay.color = Theme.SELECTION
                if (n.hasIn()) { val q = docNode(sh, n.inX, n.inY); canvas.drawLine(p.x, p.y, q.x, q.y, overlay); canvas.drawCircle(q.x, q.y, hs * 0.8f, overlay) }
                if (n.hasOut()) { val q = docNode(sh, n.outX, n.outY); canvas.drawLine(p.x, p.y, q.x, q.y, overlay); canvas.drawCircle(q.x, q.y, hs * 0.8f, overlay) }
                overlay.style = Paint.Style.FILL; overlay.color = Theme.SELECTION
            } else {
                overlay.style = Paint.Style.FILL; overlay.color = Color.WHITE
            }
            canvas.drawRect(p.x - hs, p.y - hs, p.x + hs, p.y + hs, overlay)
            overlay.style = Paint.Style.STROKE; overlay.color = Theme.SELECTION
            canvas.drawRect(p.x - hs, p.y - hs, p.x + hs, p.y + hs, overlay)
        }
        // rubber band from last pen node to hover point
        val h = hover
        if (sh === penShape && h != null && path.nodes.isNotEmpty()) {
            val last = path.nodes.last()
            val pp = Path(); pp.moveTo(last.x, last.y); pp.cubicTo(last.outX, last.outY, h.x, h.y, h.x, h.y)
            overlay.style = Paint.Style.STROKE; overlay.color = Theme.SELECTION
            canvas.drawPath(pp, overlay)
        }
    }

    // ------------------------------------------------------------------ object commands

    fun deleteSelection() {
        if (selection.isEmpty()) return
        checkpoint()
        for (l in doc.layers) l.shapes.removeAll(selection.toSet())
        select(emptyList()); changed()
    }

    fun duplicateSelection() {
        if (selection.isEmpty()) return
        checkpoint()
        val off = doc.width * 0.02f
        val copies = ArrayList<Shape>()
        for (s in selection) {
            val l = doc.findLayerOf(s) ?: continue
            val c = s.copy(); c.matrix.postTranslate(off, off); c.invalidateTransform()
            l.shapes.add(l.shapes.indexOf(s) + 1, c); copies.add(c)
        }
        select(copies); changed()
    }

    /** dir: 2 = front, 1 = forward, -1 = backward, -2 = back. */
    fun arrange(dir: Int) {
        if (selection.isEmpty()) return
        checkpoint()
        for (l in doc.layers) {
            val sel = l.shapes.filter { selection.contains(it) }
            if (sel.isEmpty()) continue
            when (dir) {
                2 -> { l.shapes.removeAll(sel.toSet()); l.shapes.addAll(sel) }
                -2 -> { l.shapes.removeAll(sel.toSet()); l.shapes.addAll(0, sel) }
                1 -> for (i in l.shapes.size - 2 downTo 0) if (selection.contains(l.shapes[i]) && !selection.contains(l.shapes[i + 1])) {
                    val t = l.shapes[i]; l.shapes[i] = l.shapes[i + 1]; l.shapes[i + 1] = t
                }
                -1 -> for (i in 1 until l.shapes.size) if (selection.contains(l.shapes[i]) && !selection.contains(l.shapes[i - 1])) {
                    val t = l.shapes[i]; l.shapes[i] = l.shapes[i - 1]; l.shapes[i - 1] = t
                }
            }
        }
        changed()
    }

    fun transformSelection(m: Matrix) {
        if (selection.isEmpty()) return
        checkpoint()
        for (s in selection) { s.matrix.postConcat(m); s.invalidateTransform() }
        changed(); onSelectionChanged?.invoke()
    }

    fun flip(horizontal: Boolean) {
        val b = selectionBounds() ?: return
        val m = Matrix()
        if (horizontal) m.setScale(-1f, 1f, b.centerX(), b.centerY()) else m.setScale(1f, -1f, b.centerX(), b.centerY())
        transformSelection(m)
    }

    fun rotateBy(deg: Float) {
        val b = selectionBounds() ?: return
        val m = Matrix(); m.setRotate(deg, b.centerX(), b.centerY()); transformSelection(m)
    }

    /** h: -1 left, 0 centre, 1 right, null = keep; v likewise for top / middle / bottom. */
    fun alignToArtboard(h: Int?, v: Int?) {
        val b = selectionBounds() ?: return
        val dx = when (h) { -1 -> -b.left; 0 -> doc.width / 2f - b.centerX(); 1 -> doc.width - b.right; else -> 0f }
        val dy = when (v) { -1 -> -b.top; 0 -> doc.height / 2f - b.centerY(); 1 -> doc.height - b.bottom; else -> 0f }
        val m = Matrix(); m.setTranslate(dx, dy); transformSelection(m)
    }

    fun convertSelectionToPath() {
        if (selection.isEmpty()) return
        checkpoint()
        val out = ArrayList<Shape>()
        for (s in selection) {
            val l = doc.findLayerOf(s)
            val p = if (s is PathShape || s is BrushShape || s is TextShape) null else toPathShape(s)
            if (l != null && p != null) { l.shapes[l.shapes.indexOf(s)] = p; out.add(p) } else out.add(s)
        }
        select(out); changed()
    }

    fun selectAll() {
        val all = ArrayList<Shape>()
        for (l in doc.layers) if (l.visible && !l.locked) all.addAll(l.shapes)
        select(all)
    }

    /** Applies a mutation to every selected shape with undo support. */
    fun editSelection(block: (Shape) -> Unit) {
        if (selection.isEmpty()) return
        checkpoint()
        selection.forEach { block(it); it.invalidate() }
        changed()
    }

    fun renderToBitmap(maxSize: Int = max(doc.width, doc.height), transparent: Boolean = false): Bitmap {
        val scale = maxSize.toFloat() / max(doc.width, doc.height)
        val bmp = Bitmap.createBitmap((doc.width * scale).roundToInt(), (doc.height * scale).roundToInt(), Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        c.scale(scale, scale)
        doc.render(c, !transparent)
        return bmp
    }
}

/** Tiny growable float list to avoid boxing while recording strokes. */
class FloatArrayList {
    private var data = FloatArray(256)
    var size = 0
        private set
    fun clear() { size = 0 }
    fun add(a: Float, b: Float, c: Float) {
        if (size + 3 > data.size) data = data.copyOf(data.size * 2)
        data[size++] = a; data[size++] = b; data[size++] = c
    }
    operator fun get(i: Int) = data[i]
    fun toArray(): FloatArray = data.copyOf(size)
}
