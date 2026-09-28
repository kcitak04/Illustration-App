package com.coverstudio.app.photo

import android.app.AlertDialog
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.ImageDecoder
import android.graphics.Matrix
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.ScrollView
import android.widget.TextView
import com.coverstudio.app.common.Studio
import com.coverstudio.app.common.StudioHost
import com.coverstudio.app.project.ProjectStore
import com.coverstudio.app.ui.ColorChip
import com.coverstudio.app.ui.IconButton
import com.coverstudio.app.ui.LiveEdit
import com.coverstudio.app.ui.OptionsBar
import com.coverstudio.app.ui.SliderRow
import com.coverstudio.app.ui.Theme
import com.coverstudio.app.ui.checkerShader
import com.coverstudio.app.ui.choose
import com.coverstudio.app.ui.dp
import com.coverstudio.app.ui.label
import com.coverstudio.app.ui.promptText
import com.coverstudio.app.ui.roundRect
import com.coverstudio.app.ui.showColorPicker
import com.coverstudio.app.ui.textButton
import com.coverstudio.app.ui.toast
import com.coverstudio.app.vector.BLEND_NAMES
import com.coverstudio.app.vector.VectorDoc
import kotlin.math.max
import kotlin.math.roundToInt

class PhotoStudio(private val host: StudioHost, doc: RasterDoc, private val vectorDoc: () -> VectorDoc) : Studio {
    private val ctx = host.activity
    val canvas = PhotoCanvasView(ctx)
    private val options = OptionsBar(ctx)
    private val toolButtons = HashMap<PTool, IconButton>()
    private val layersPanel = LinearLayout(ctx)
    private val layersScroll = ScrollView(ctx)
    private var layersVisible = false
    private val canvasFrame = FrameLayout(ctx)
    private var opPanel: View? = null
    private val thumbs = HashMap<Long, Pair<Int, Bitmap>>()

    override val root: View

    init {
        canvas.setDocument(doc)
        canvas.smartVector = vectorDoc()
        val strip = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(2), dp(4), dp(2), dp(4)) }
        for (t in PTool.values()) {
            val b = IconButton(ctx, t.icon, t.label) { selectTool(t) }
            b.accent = Theme.PHOTO_ACCENT
            toolButtons[t] = b; strip.addView(b)
        }
        val stripScroll = ScrollView(ctx).apply { setBackgroundColor(Theme.PANEL); isVerticalScrollBarEnabled = false; addView(strip) }

        layersPanel.orientation = LinearLayout.VERTICAL
        layersPanel.setPadding(dp(8), dp(8), dp(8), dp(8))
        layersScroll.addView(layersPanel)
        layersScroll.background = roundRect(0xF0202226.toInt(), dp(10f), Theme.DIVIDER, dp(1))
        layersScroll.visibility = View.GONE

        canvasFrame.addView(canvas, FrameLayout.LayoutParams(-1, -1))
        canvasFrame.addView(layersScroll, FrameLayout.LayoutParams(dp(290), -2, Gravity.END or Gravity.TOP).apply { setMargins(0, dp(8), dp(8), dp(8)) })

        val body = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        body.addView(stripScroll, LinearLayout.LayoutParams(dp(52), -1))
        body.addView(canvasFrame, LinearLayout.LayoutParams(0, -1, 1f))
        root = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            addView(options, LinearLayout.LayoutParams(-1, dp(52)))
            addView(View(ctx).apply { setBackgroundColor(Theme.DIVIDER) }, LinearLayout.LayoutParams(-1, 1))
            addView(body, LinearLayout.LayoutParams(-1, 0, 1f))
        }

        canvas.onChanged = { host.markDirty(); host.refreshTopBar() }
        canvas.onLayersChanged = { if (layersVisible) rebuildLayers(); rebuildOptions() }
        canvas.onColorPicked = { c -> canvas.color = c; rebuildOptions() }
        canvas.onBusy = { host.setBusy(it) }
        canvas.onMessage = { toast(ctx, it) }
        canvas.onTransformStateChanged = { rebuildOptions() }
        canvas.onUndoGesture = { undo() }
        canvas.onRedoGesture = { redo() }
        canvas.onNeedsRasterize = { l, then ->
            AlertDialog.Builder(ctx).setTitle("Rasterize linked illustration?")
                .setMessage("\"${l.name}\" is linked to your vector artwork. To paint or filter it, it must be converted to pixels (it will stop updating from the Vector Studio).")
                .setPositiveButton("Rasterize") { _, _ -> canvas.rasterize(l); then() }
                .setNegativeButton("Cancel", null).show()
        }
        selectTool(PTool.BRUSH)
    }

    val doc get() = canvas.doc

    override val canUndo get() = canvas.canUndo
    override val canRedo get() = canvas.canRedo
    override fun undo() { closeOpPanel(); canvas.undo(); host.refreshTopBar() }
    override fun redo() { closeOpPanel(); canvas.redo(); host.refreshTopBar() }

    override fun onShown() {
        canvas.smartVector = vectorDoc()
        canvas.refreshSmartLayers()
        canvas.invalidate()
    }

    override fun onHidden() { canvas.commitTransform(); closeOpPanel() }

    private fun dp(v: Int) = ctx.dp(v)
    private fun dp(v: Float) = ctx.dp(v)

    fun selectTool(t: PTool) {
        canvas.tool = t
        toolButtons.forEach { (k, b) -> b.active = k == t }
        if (t == PTool.MOVE) canvas.beginTransform()
        rebuildOptions()
    }

    // ------------------------------------------------------------------ options

    private fun toggle(text: String, on: Boolean, cb: (Boolean) -> Unit): TextView {
        val tv = TextView(ctx)
        var state = on
        fun style() {
            tv.setTextColor(if (state) Color.WHITE else Theme.TEXT_DIM)
            tv.background = roundRect(if (state) Theme.PHOTO_ACCENT else Theme.PANEL_2, dp(8f))
        }
        tv.text = text; tv.textSize = 13f; tv.gravity = Gravity.CENTER
        tv.setPadding(dp(10), dp(6), dp(10), dp(6)); style()
        tv.setOnClickListener { state = !state; style(); cb(state) }
        tv.layoutParams = LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(3), 0, dp(3), 0) }
        return tv
    }

    private fun segmented(items: List<String>, selected: Int, cb: (Int) -> Unit): LinearLayout {
        val row = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        val views = ArrayList<TextView>()
        items.forEachIndexed { i, s ->
            val tv = TextView(ctx).apply {
                text = s; textSize = 12f; gravity = Gravity.CENTER; setPadding(dp(9), dp(6), dp(9), dp(6))
            }
            views.add(tv)
            tv.setOnClickListener {
                views.forEachIndexed { j, v ->
                    v.background = roundRect(if (j == i) Theme.PHOTO_ACCENT else Theme.PANEL_2, dp(6f))
                    v.setTextColor(if (j == i) Color.WHITE else Theme.TEXT_DIM)
                }
                cb(i)
            }
            tv.background = roundRect(if (i == selected) Theme.PHOTO_ACCENT else Theme.PANEL_2, dp(6f))
            tv.setTextColor(if (i == selected) Color.WHITE else Theme.TEXT_DIM)
            row.addView(tv, LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(1), 0, dp(1), 0) })
        }
        return row
    }

    private fun colorChip(get: () -> Int, set: (Int) -> Unit, title: String = "Colour"): ColorChip {
        val chip = ColorChip(ctx) { c -> showColorPicker(ctx, title, get()) { v -> if (v != null) { set(v); c.color = v } } }
        chip.color = get()
        return chip
    }

    private fun brushControls(b: BrushSettings, withColor: Boolean, withFlow: Boolean = true) {
        if (withColor) options.add(colorChip({ canvas.color }, { canvas.color = it }))
        options.add(SliderRow(ctx, "Size", 1f, max(doc.width, doc.height) / 5f, b.size, widthDp = 130) { v, _ -> b.size = v })
        options.add(SliderRow(ctx, "Hard", 0f, 100f, b.hardness * 100, { "${it.roundToInt()}%" }, 90) { v, _ -> b.hardness = v / 100f })
        options.add(SliderRow(ctx, "Opacity", 1f, 100f, b.opacity * 100, { "${it.roundToInt()}%" }, 100) { v, _ -> b.opacity = v / 100f })
        if (withFlow) options.add(SliderRow(ctx, "Flow", 1f, 100f, b.flow * 100, { "${it.roundToInt()}%" }, 90) { v, _ -> b.flow = v / 100f })
        options.add(toggle("Pressure→Size", b.pressureSize) { b.pressureSize = it })
        options.add(toggle("Pressure→Opacity", b.pressureOpacity) { b.pressureOpacity = it })
        maskToggle()
    }

    private fun maskToggle() {
        val l = doc.activeLayer ?: return
        if (l.mask != null) {
            options.divider()
            options.add(toggle("Paint on mask", canvas.editMask) { canvas.editMask = it; rebuildLayersIfOpen() })
            if (canvas.editMask) options.add(label(ctx, "Dark brush hides · light brush / eraser reveals", 11f))
        }
    }

    private fun selectionControls() {
        options.add(segmented(listOf("New", "Add", "Subtract"), canvas.selectionMode) { canvas.selectionMode = it })
        options.add(textButton(ctx, "All") { canvas.selectAll() })
        options.add(textButton(ctx, "None") { canvas.deselect() })
        options.add(textButton(ctx, "Invert") { canvas.invertSelection() })
        options.add(textButton(ctx, "Feather…") { featherDialog() })
    }

    fun rebuildOptions() {
        options.clear()
        when (canvas.tool) {
            PTool.MOVE -> {
                if (canvas.transforming) {
                    options.add(textButton(ctx, "✓ Apply", true) { canvas.commitTransform() }.apply { background = roundRect(Theme.PHOTO_ACCENT, dp(8f)) })
                    options.add(textButton(ctx, "Cancel") { canvas.cancelTransform(); canvas.beginTransform() })
                }
                options.add(toggle("Keep ratio", canvas.proportional) { canvas.proportional = it })
                options.add(textButton(ctx, "Fit") { canvas.fitLayerToCanvas(false) })
                options.add(textButton(ctx, "Fill canvas") { canvas.fitLayerToCanvas(true) })
                options.add(IconButton(ctx, "flip_h", "Flip layer horizontally") {
                    canvas.transformBy { m, b -> m.postScale(-1f, 1f, b.centerX(), b.centerY()) }
                })
                options.add(IconButton(ctx, "flip_v", "Flip layer vertically") {
                    canvas.transformBy { m, b -> m.postScale(1f, -1f, b.centerX(), b.centerY()) }
                })
                options.add(IconButton(ctx, "rotate", "Rotate layer 90°") {
                    canvas.transformBy { m, b -> m.postRotate(90f, b.centerX(), b.centerY()) }
                })
                options.add(label(ctx, "Drag to move · corners scale · top knob rotates", 11f))
            }
            PTool.MARQUEE -> {
                options.add(segmented(listOf("Rect", "Ellipse"), if (canvas.marqueeEllipse) 1 else 0) { canvas.marqueeEllipse = it == 1 })
                options.divider(); selectionControls()
            }
            PTool.LASSO -> selectionControls()
            PTool.WAND -> {
                options.add(SliderRow(ctx, "Tolerance", 0f, 255f, canvas.fillTolerance, widthDp = 110) { v, _ -> canvas.fillTolerance = v })
                options.add(toggle("Contiguous", canvas.contiguous) { canvas.contiguous = it })
                options.add(toggle("All layers", canvas.sampleAllLayers) { canvas.sampleAllLayers = it })
                options.divider(); selectionControls()
            }
            PTool.BRUSH -> brushControls(canvas.brush, true)
            PTool.ERASER -> brushControls(canvas.eraser, false)
            PTool.SMUDGE -> {
                options.add(SliderRow(ctx, "Size", 2f, max(doc.width, doc.height) / 6f, canvas.smudge.size, widthDp = 130) { v, _ -> canvas.smudge.size = v })
                options.add(SliderRow(ctx, "Strength", 1f, 99f, canvas.smudgeStrength * 100, { "${it.roundToInt()}%" }, 110) { v, _ -> canvas.smudgeStrength = v / 100f })
                options.add(SliderRow(ctx, "Hard", 0f, 100f, canvas.smudge.hardness * 100, { "${it.roundToInt()}%" }, 90) { v, _ -> canvas.smudge.hardness = v / 100f })
                options.add(toggle("Pressure", canvas.smudge.pressureOpacity) { canvas.smudge.pressureOpacity = it })
            }
            PTool.CLONE -> {
                options.add(textButton(ctx, if (canvas.pickCloneSource) "Tap source…" else "Set source", canvas.pickCloneSource) {
                    canvas.pickCloneSource = true; rebuildOptions()
                })
                brushControls(canvas.clone, false, false)
            }
            PTool.GRADIENT -> {
                options.add(colorChip({ canvas.color }, { canvas.color = it }, "Start colour"))
                options.add(colorChip({ canvas.color2 }, { canvas.color2 = it }, "End colour"))
                options.add(IconButton(ctx, "send", "Swap colours") { val t = canvas.color; canvas.color = canvas.color2; canvas.color2 = t; rebuildOptions() })
                options.add(segmented(listOf("Linear", "Radial"), if (canvas.gradientRadial) 1 else 0) { canvas.gradientRadial = it == 1 })
                options.add(toggle("To transparent", canvas.gradientToTransparent) { canvas.gradientToTransparent = it })
                options.add(SliderRow(ctx, "Opacity", 1f, 100f, canvas.gradientOpacity * 100, { "${it.roundToInt()}%" }, 100) { v, _ -> canvas.gradientOpacity = v / 100f })
            }
            PTool.BUCKET -> {
                options.add(colorChip({ canvas.color }, { canvas.color = it }))
                options.add(SliderRow(ctx, "Tolerance", 0f, 255f, canvas.fillTolerance, widthDp = 110) { v, _ -> canvas.fillTolerance = v })
                options.add(SliderRow(ctx, "Opacity", 1f, 100f, canvas.fillOpacity * 100, { "${it.roundToInt()}%" }, 100) { v, _ -> canvas.fillOpacity = v / 100f })
                options.add(toggle("Contiguous", canvas.contiguous) { canvas.contiguous = it })
                options.add(toggle("All layers", canvas.sampleAllLayers) { canvas.sampleAllLayers = it })
            }
            PTool.EYEDROPPER -> {
                options.add(colorChip({ canvas.color }, { canvas.color = it }))
                options.add(label(ctx, "Tap the image to pick a colour (stylus button works in any tool)", 11f))
            }
            PTool.HAND -> {
                options.add(textButton(ctx, "Fit") { canvas.fitToScreen() })
                options.add(textButton(ctx, "100%") { canvas.actualPixels() })
            }
        }
    }

    private fun featherDialog() {
        if (doc.selection == null) { toast(ctx, "Make a selection first"); return }
        promptText(ctx, "Feather radius (px)", "20") { s -> s.toFloatOrNull()?.let { canvas.featherSelection(it) } }
    }

    // ------------------------------------------------------------------ layers panel

    override fun toggleLayers() {
        layersVisible = !layersVisible
        layersScroll.visibility = if (layersVisible) View.VISIBLE else View.GONE
        if (layersVisible) rebuildLayers()
    }

    private fun rebuildLayersIfOpen() { if (layersVisible) rebuildLayers() }

    private fun thumbFor(l: RasterLayer, mask: Boolean = false): Bitmap? {
        val key = if (mask) -l.id else l.id
        val cached = thumbs[key]
        if (cached != null && cached.first == l.version) return cached.second
        val src = (if (mask) l.mask else l.bitmap) ?: return null
        val s = 56f / max(src.width, src.height)
        val m = Matrix(); m.setScale(s, s)
        val t = try { Bitmap.createBitmap(src, 0, 0, src.width, src.height, m, true) } catch (e: Exception) { return null }
        val out = if (mask) Bitmap.createBitmap(t.width, t.height, Bitmap.Config.ARGB_8888).also {
            val c = android.graphics.Canvas(it); c.drawColor(Color.BLACK)
            c.drawBitmap(t, 0f, 0f, android.graphics.Paint().apply { color = Color.WHITE })
        } else t
        thumbs[key] = l.version to out
        return out
    }

    private fun rebuildLayers() {
        layersPanel.removeAllViews()
        layersPanel.addView(LinearLayout(ctx).apply {
            gravity = Gravity.CENTER_VERTICAL
            addView(label(ctx, "Layers", 15f, Theme.TEXT), LinearLayout.LayoutParams(0, -2, 1f))
            addView(IconButton(ctx, "close", "Close") { toggleLayers() })
        })
        for (i in doc.layers.indices.reversed()) {
            val l = doc.layers[i]
            val activeRow = i == doc.active
            val row = LinearLayout(ctx).apply {
                orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
                background = roundRect(if (activeRow) 0x33FF8A4F else 0, dp(8f), if (activeRow) Theme.PHOTO_ACCENT else 0, if (activeRow) dp(1) else 0)
                setPadding(dp(2), dp(2), dp(4), dp(2))
            }
            row.addView(IconButton(ctx, if (l.visible) "eye" else "eye_off", "Show / hide") {
                canvas.setLayerProps(l, true) { it.visible = !it.visible }; rebuildLayers()
            }.apply { layoutParams = LinearLayout.LayoutParams(dp(36), dp(40)) })
            val thumb = ImageView(ctx).apply {
                setImageBitmap(thumbFor(l))
                background = android.graphics.drawable.ShapeDrawable().apply { paint.shader = checkerShader(ctx, dp(4)) }
                scaleType = ImageView.ScaleType.FIT_CENTER
                if (activeRow && !canvas.editMask) { setPadding(dp(1), dp(1), dp(1), dp(1)) }
                setOnClickListener { doc.active = i; canvas.editMask = false; canvas.commitTransform(); rebuildLayers(); rebuildOptions() }
            }
            row.addView(thumb, LinearLayout.LayoutParams(dp(40), dp(40)))
            if (l.mask != null) {
                val mt = ImageView(ctx).apply {
                    setImageBitmap(thumbFor(l, true))
                    alpha = if (l.maskEnabled) 1f else 0.35f
                    if (activeRow && canvas.editMask) background = roundRect(0, 0f, Theme.PHOTO_ACCENT, dp(2))
                    setPadding(dp(2), dp(2), dp(2), dp(2))
                    setOnClickListener { doc.active = i; canvas.editMask = true; rebuildLayers(); rebuildOptions() }
                }
                row.addView(mt, LinearLayout.LayoutParams(dp(40), dp(40)).apply { setMargins(dp(3), 0, 0, 0) })
            }
            val tags = buildString {
                if (l.smart) append("🔗 ")
                if (l.blend != 0) append(BLEND_NAMES[l.blend]).append(" · ")
                append("${(l.opacity * 100).roundToInt()}%")
            }
            val texts = LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL; setPadding(dp(6), 0, 0, 0)
                addView(TextView(ctx).apply { text = l.name; textSize = 13f; setTextColor(Theme.TEXT); maxLines = 1 })
                addView(TextView(ctx).apply { text = tags; textSize = 11f; setTextColor(Theme.TEXT_DIM); maxLines = 1 })
            }
            row.addView(texts, LinearLayout.LayoutParams(0, -2, 1f))
            if (l.locked) row.addView(IconButton(ctx, "lock", "Unlock") { canvas.setLayerProps(l, true) { it.locked = false }; rebuildLayers() }
                .apply { layoutParams = LinearLayout.LayoutParams(dp(34), dp(40)) })
            row.setOnClickListener { doc.active = i; canvas.editMask = false; canvas.commitTransform(); rebuildLayers(); rebuildOptions() }
            row.setOnLongClickListener { layerMenu(row, i); true }
            layersPanel.addView(row, LinearLayout.LayoutParams(-1, -2).apply { setMargins(0, dp(2), 0, dp(2)) })
        }
        val l = doc.activeLayer ?: return
        val live = LiveEdit { canvas.pushUndo() }
        layersPanel.addView(SliderRow(ctx, "Opacity", 0f, 100f, l.opacity * 100, { "${it.roundToInt()}%" }, 130) { v, done ->
            live.change(done) { canvas.setLayerProps(l, false) { it.opacity = v / 100f } }
            if (done) { host.refreshTopBar(); rebuildLayers() }
        })
        layersPanel.addView(LinearLayout(ctx).apply {
            gravity = Gravity.CENTER_VERTICAL
            addView(label(ctx, "Blend"))
            addView(textButton(ctx, BLEND_NAMES[l.blend]) {
                choose(ctx, "Blend mode", BLEND_NAMES) { b -> canvas.setLayerProps(l, true) { it.blend = b }; rebuildLayers() }
            })
            addView(IconButton(ctx, if (l.locked) "lock" else "unlock", "Lock layer") { canvas.setLayerProps(l, true) { it.locked = !it.locked }; rebuildLayers() })
        })
        val row1 = LinearLayout(ctx).apply {
            addView(IconButton(ctx, "plus", "New layer") { canvas.addEmptyLayer() })
            addView(IconButton(ctx, "image", "Import photo as layer") { importPhoto() })
            addView(IconButton(ctx, "copy", "Duplicate layer") { canvas.duplicateLayer() })
            addView(IconButton(ctx, "mask", "Add layer mask (from selection if any)") {
                if (l.mask == null) canvas.addMask(doc.selection != null) else maskMenu(it)
            })
            addView(IconButton(ctx, "merge", "Merge down") { canvas.mergeDown() })
        }
        val row2 = LinearLayout(ctx).apply {
            addView(IconButton(ctx, "up", "Move layer up") { canvas.moveLayer(1) })
            addView(IconButton(ctx, "down", "Move layer down") { canvas.moveLayer(-1) })
            addView(IconButton(ctx, "vector", "Place vector illustration (linked)") { placeIllustration(true, false) })
            addView(IconButton(ctx, "trash", "Delete layer") { canvas.deleteLayer() })
            addView(IconButton(ctx, "menu", "More layer options") { layerMenu(it, doc.active) })
        }
        layersPanel.addView(row1); layersPanel.addView(row2)
    }

    private fun maskMenu(anchor: View) {
        val pm = PopupMenu(ctx, anchor)
        pm.menu.add(if (canvas.editMask) "Edit layer pixels" else "Edit mask").setOnMenuItemClickListener { canvas.editMask = !canvas.editMask; rebuildLayers(); rebuildOptions(); true }
        pm.menu.add(if (doc.activeLayer?.maskEnabled == true) "Disable mask" else "Enable mask").setOnMenuItemClickListener {
            doc.activeLayer?.let { l -> canvas.setLayerProps(l, true) { it.maskEnabled = !it.maskEnabled; it.version++ } }; rebuildLayers(); true
        }
        pm.menu.add("Invert mask").setOnMenuItemClickListener { canvas.invertMask(); true }
        pm.menu.add("Apply mask").setOnMenuItemClickListener { canvas.deleteMask(true); true }
        pm.menu.add("Delete mask").setOnMenuItemClickListener { canvas.deleteMask(false); true }
        pm.show()
    }

    private fun layerMenu(anchor: View, i: Int) {
        doc.active = i
        val l = doc.layers[i]
        val pm = PopupMenu(ctx, anchor)
        pm.menu.add("Rename…").setOnMenuItemClickListener {
            promptText(ctx, "Rename layer", l.name) { n -> canvas.setLayerProps(l, true) { it.name = n }; rebuildLayers() }; true
        }
        if (l.smart) pm.menu.add("Rasterize").setOnMenuItemClickListener { canvas.rasterize(l); true }
        pm.menu.add("Select layer pixels").setOnMenuItemClickListener { canvas.selectLayerContent(); true }
        pm.menu.add("Merge down").setOnMenuItemClickListener { canvas.mergeDown(); true }
        pm.menu.add("Flatten image").setOnMenuItemClickListener { canvas.flattenImage(); true }
        pm.menu.add("Delete").setOnMenuItemClickListener { canvas.deleteLayer(); true }
        pm.setOnDismissListener { rebuildLayersIfOpen() }
        pm.show()
    }

    // ------------------------------------------------------------------ adjustments & filters panel

    private fun closeOpPanel() {
        opPanel?.let { canvasFrame.removeView(it) }
        if (opPanel != null) canvas.endPreview()
        opPanel = null
    }

    fun openOp(op: ImageOp) {
        val l = doc.activeLayer ?: return
        if (l.smart) { canvas.onNeedsRasterize?.invoke(l) { openOp(op) }; return }
        if (l.locked) { toast(ctx, "Layer is locked"); return }
        closeOpPanel()
        canvas.commitTransform()
        if (!canvas.startPreview()) return
        val values = FloatArray(op.params.size) { op.params[it].def }
        val colors = IntArray(op.colorNames.size) { op.defColors[it] }
        val panel = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            background = roundRect(0xF0202226.toInt(), dp(12f), Theme.DIVIDER, dp(1))
            setPadding(dp(12), dp(8), dp(12), dp(8))
            isClickable = true
        }
        panel.addView(label(ctx, op.name + if (doc.selection != null) "  (selection)" else "", 15f, Theme.TEXT))
        fun update() = canvas.updatePreview(op, values, colors)
        op.params.forEachIndexed { i, p ->
            panel.addView(SliderRow(ctx, p.name, p.min, p.max, p.def, p.fmt, 170) { v, _ -> values[i] = v; update() })
        }
        if (op.colorNames.isNotEmpty()) {
            val row = LinearLayout(ctx).apply { gravity = Gravity.CENTER_VERTICAL }
            op.colorNames.forEachIndexed { i, n ->
                row.addView(label(ctx, n))
                row.addView(colorChip({ colors[i] }, { colors[i] = it; update() }, n))
            }
            panel.addView(row)
        }
        panel.addView(LinearLayout(ctx).apply {
            gravity = Gravity.END
            addView(textButton(ctx, "Cancel") { closeOpPanel() })
            addView(textButton(ctx, "Apply", true) {
                val p = opPanel
                canvas.applyOp(op, values, colors) { if (opPanel === p) { opPanel?.let { canvasFrame.removeView(it) }; opPanel = null } }
            }.apply { background = roundRect(Theme.PHOTO_ACCENT, dp(8f)) })
        })
        val scroll = ScrollView(ctx).apply { addView(panel) }
        canvasFrame.addView(scroll, FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM).apply { setMargins(dp(8), dp(8), dp(8), dp(8)) })
        opPanel = scroll
        update()
    }

    // ------------------------------------------------------------------ import / place

    fun importPhoto() {
        val i = Intent(Intent.ACTION_GET_CONTENT).apply { type = "image/*"; addCategory(Intent.CATEGORY_OPENABLE) }
        host.launchForResult(Intent.createChooser(i, "Import photo")) { data ->
            val uri = data?.data ?: return@launchForResult
            host.setBusy(true)
            Thread {
                val bmp = try {
                    val src = ImageDecoder.createSource(ctx.contentResolver, uri)
                    ImageDecoder.decodeBitmap(src) { dec, info, _ ->
                        dec.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                        val maxSide = max(doc.width, doc.height) * 1.5f
                        val s = maxSide / max(info.size.width, info.size.height)
                        if (s < 1f) dec.setTargetSize((info.size.width * s).roundToInt(), (info.size.height * s).roundToInt())
                    }
                } catch (e: Exception) { null } catch (e: OutOfMemoryError) { null }
                ctx.runOnUiThread {
                    host.setBusy(false)
                    if (bmp == null) toast(ctx, "Couldn't open that image")
                    else {
                        choose(ctx, "Place photo", listOf("Fill the canvas (crop edges)", "Fit inside the canvas")) { which ->
                            canvas.addImageLayer("Photo", bmp, which == 0)
                            selectTool(PTool.MOVE)
                        }
                    }
                }
            }.start()
        }
    }

    fun placeIllustration(linked: Boolean, withBg: Boolean) {
        val v = vectorDoc()
        if (linked) canvas.addSmartLayer(v, withBg)
        else {
            val bmp = Bitmap.createBitmap(doc.width, doc.height, Bitmap.Config.ARGB_8888)
            val c = android.graphics.Canvas(bmp)
            c.scale(doc.width.toFloat() / v.width, doc.height.toFloat() / v.height)
            v.render(c, withBg)
            canvas.addImageLayer("Illustration", bmp)
        }
    }

    // ------------------------------------------------------------------ menu

    override fun showMenu(anchor: View) {
        val pm = PopupMenu(ctx, anchor)
        val m = pm.menu
        val adj = m.addSubMenu("Adjustments")
        ADJUSTMENTS.forEach { op -> adj.add(op.name).setOnMenuItemClickListener { openOp(op); true } }
        val fil = m.addSubMenu("Filters")
        FILTERS.forEach { op -> fil.add(op.name).setOnMenuItemClickListener { openOp(op); true } }
        val sel = m.addSubMenu("Select")
        sel.add("All").setOnMenuItemClickListener { canvas.selectAll(); true }
        sel.add("Deselect").setOnMenuItemClickListener { canvas.deselect(); true }
        sel.add("Inverse").setOnMenuItemClickListener { canvas.invertSelection(); true }
        sel.add("Feather…").setOnMenuItemClickListener { featherDialog(); true }
        sel.add("Layer pixels").setOnMenuItemClickListener { canvas.selectLayerContent(); true }
        sel.add("Layer via copy").setOnMenuItemClickListener { canvas.layerViaCopy(false); true }
        sel.add("Layer via cut").setOnMenuItemClickListener { canvas.layerViaCopy(true); true }
        sel.add("Delete selected pixels").setOnMenuItemClickListener { canvas.clearSelectionPixels(); true }
        sel.add("Fill with colour…").setOnMenuItemClickListener {
            showColorPicker(ctx, "Fill selection", canvas.color) { c -> if (c != null) canvas.fillSelection(c) }; true
        }
        val layer = m.addSubMenu("Layer")
        layer.add("New layer").setOnMenuItemClickListener { canvas.addEmptyLayer(); true }
        layer.add("Duplicate layer").setOnMenuItemClickListener { canvas.duplicateLayer(); true }
        layer.add("Add mask").setOnMenuItemClickListener { canvas.addMask(false); true }
        layer.add("Mask from selection").setOnMenuItemClickListener { canvas.addMask(true); true }
        layer.add("Merge down").setOnMenuItemClickListener { canvas.mergeDown(); true }
        layer.add("Flatten image").setOnMenuItemClickListener { canvas.flattenImage(); true }
        val place = m.addSubMenu("Place")
        place.add("Photo from gallery…").setOnMenuItemClickListener { importPhoto(); true }
        place.add("Illustration (linked, transparent)").setOnMenuItemClickListener { placeIllustration(true, false); true }
        place.add("Illustration (linked, with background)").setOnMenuItemClickListener { placeIllustration(true, true); true }
        place.add("Illustration (pixels)").setOnMenuItemClickListener { placeIllustration(false, true); true }
        val img = m.addSubMenu("Image")
        img.add("Flip canvas horizontal").setOnMenuItemClickListener { canvas.transformCanvas(0); true }
        img.add("Flip canvas vertical").setOnMenuItemClickListener { canvas.transformCanvas(1); true }
        if (doc.width == doc.height) {
            img.add("Rotate canvas 90° CW").setOnMenuItemClickListener { canvas.transformCanvas(2); true }
            img.add("Rotate canvas 90° CCW").setOnMenuItemClickListener { canvas.transformCanvas(3); true }
        }
        val exp = m.addSubMenu("Export")
        exp.add("PNG").setOnMenuItemClickListener { export(false, 1f); true }
        exp.add("JPEG (best for distributors)").setOnMenuItemClickListener { export(true, 1f); true }
        exp.add("JPEG 1400 px (small)").setOnMenuItemClickListener { export(true, 1400f / max(doc.width, doc.height)); true }
        m.add("Fit to screen").setOnMenuItemClickListener { canvas.fitToScreen(); true }
        m.add("Stylus & input settings…").setOnMenuItemClickListener { host.showInputSettings(); true }
        pm.show()
    }

    private fun export(jpeg: Boolean, scale: Float) {
        canvas.commitTransform()
        host.setBusy(true)
        Thread {
            val bmp = try { doc.flatten(scale.coerceAtMost(1f)) } catch (e: OutOfMemoryError) { null }
            val uri = bmp?.let { ProjectStore.exportImage(ctx, it, host.project.name, jpeg) }
            ctx.runOnUiThread {
                host.setBusy(false)
                if (uri == null || bmp == null) toast(ctx, "Export failed")
                else AlertDialog.Builder(ctx).setMessage("Saved to Pictures/CoverStudio (${bmp.width}×${bmp.height})")
                    .setPositiveButton("Share") { _, _ -> ProjectStore.share(ctx, uri, if (jpeg) "image/jpeg" else "image/png") }
                    .setNegativeButton("Done", null).show()
            }
        }.start()
    }
}
