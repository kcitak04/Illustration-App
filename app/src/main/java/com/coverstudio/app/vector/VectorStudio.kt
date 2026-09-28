package com.coverstudio.app.vector

import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
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
import com.coverstudio.app.ui.choose
import com.coverstudio.app.ui.dp
import com.coverstudio.app.ui.label
import com.coverstudio.app.ui.promptText
import com.coverstudio.app.ui.roundRect
import com.coverstudio.app.ui.showColorPicker
import com.coverstudio.app.ui.textButton
import com.coverstudio.app.ui.toast
import java.io.File
import kotlin.math.roundToInt

class VectorStudio(private val host: StudioHost, doc: VectorDoc) : Studio {
    private val ctx = host.activity
    val canvas = VectorCanvasView(ctx)
    private val options = OptionsBar(ctx)
    private val toolButtons = HashMap<VTool, IconButton>()
    private val layersPanel = LinearLayout(ctx)
    private val layersScroll = ScrollView(ctx)
    private var layersVisible = false

    override val root: View

    init {
        canvas.setDocument(doc)
        val strip = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(2), dp(4), dp(2), dp(4)) }
        for (t in VTool.values()) {
            val b = IconButton(ctx, t.icon, t.label) { selectTool(t) }
            toolButtons[t] = b; strip.addView(b)
        }
        val stripScroll = ScrollView(ctx).apply {
            setBackgroundColor(Theme.PANEL); isVerticalScrollBarEnabled = false; addView(strip)
        }

        layersPanel.orientation = LinearLayout.VERTICAL
        layersPanel.setPadding(dp(8), dp(8), dp(8), dp(8))
        layersScroll.addView(layersPanel)
        layersScroll.background = roundRect(0xF0202226.toInt(), dp(10f), Theme.DIVIDER, dp(1))
        layersScroll.visibility = View.GONE

        val canvasFrame = FrameLayout(ctx)
        canvasFrame.addView(canvas, FrameLayout.LayoutParams(-1, -1))
        canvasFrame.addView(layersScroll, FrameLayout.LayoutParams(dp(270), -2, Gravity.END or Gravity.TOP).apply { setMargins(0, dp(8), dp(8), dp(8)) })

        val body = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        body.addView(stripScroll, LinearLayout.LayoutParams(dp(52), -1))
        body.addView(canvasFrame, LinearLayout.LayoutParams(0, -1, 1f))

        root = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            addView(options, LinearLayout.LayoutParams(-1, dp(52)))
            addView(View(ctx).apply { setBackgroundColor(Theme.DIVIDER) }, LinearLayout.LayoutParams(-1, 1))
            addView(body, LinearLayout.LayoutParams(-1, 0, 1f))
        }

        canvas.onSelectionChanged = {
            canvas.selection.singleOrNull()?.let { s ->
                canvas.fill = s.fill.copy(); canvas.stroke = s.stroke.copy()
            }
            rebuildOptions(); host.refreshTopBar()
        }
        canvas.onDocChanged = { host.markDirty(); host.refreshTopBar(); if (layersVisible) rebuildLayers() }
        canvas.onUndoGesture = { undo() }
        canvas.onRedoGesture = { redo() }
        canvas.onColorPicked = { c ->
            canvas.fill = Fill(Fill.SOLID, c)
            if (canvas.selection.isNotEmpty()) canvas.editSelection { it.fill = Fill(Fill.SOLID, c) }
            rebuildOptions()
        }
        canvas.onEditText = { existing, x, y ->
            if (existing != null) promptText(ctx, "Edit text", existing.text, true) { t ->
                if (t.isNotBlank()) canvas.editSelection { (it as? TextShape)?.text = t }
            } else promptText(ctx, "Add text", "", true) { canvas.addText(it, x, y) }
        }
        selectTool(VTool.BRUSH)
    }

    val doc get() = canvas.doc

    override val canUndo get() = canvas.canUndo
    override val canRedo get() = canvas.canRedo
    override fun undo() { canvas.undo(); rebuildOptions(); host.refreshTopBar() }
    override fun redo() { canvas.redo(); rebuildOptions(); host.refreshTopBar() }
    override fun onShown() { canvas.invalidate() }
    override fun onHidden() { canvas.finishPen() }

    private fun dp(v: Int) = ctx.dp(v)
    private fun dp(v: Float) = ctx.dp(v)

    fun selectTool(t: VTool) {
        canvas.tool = t
        toolButtons.forEach { (k, b) -> b.active = k == t }
        rebuildOptions()
    }

    // ------------------------------------------------------------------ options bar

    private fun toggle(labelText: String, on: Boolean, cb: (Boolean) -> Unit): TextView {
        val tv = TextView(ctx)
        fun style(v: Boolean) {
            tv.setTextColor(if (v) Color.WHITE else Theme.TEXT_DIM)
            tv.background = roundRect(if (v) Theme.ACCENT else Theme.PANEL_2, dp(8f))
        }
        var state = on
        tv.text = labelText; tv.textSize = 13f; tv.gravity = Gravity.CENTER
        tv.setPadding(dp(10), dp(6), dp(10), dp(6))
        style(state)
        tv.setOnClickListener { state = !state; style(state); cb(state) }
        tv.layoutParams = LinearLayout.LayoutParams(-2, -2).apply { setMargins(dp(3), 0, dp(3), 0) }
        return tv
    }

    private fun applyToSelection(block: (Shape) -> Unit) {
        if (canvas.tool == VTool.SELECT || canvas.tool == VTool.DIRECT) canvas.editSelection(block)
    }

    private fun addStyleControls() {
        val fillChip = ColorChip(ctx) {
            showColorPicker(ctx, "Fill", if (canvas.fill.type == Fill.NONE) Color.BLACK else canvas.fill.color, true) { c ->
                canvas.fill = if (c == null) Fill(Fill.NONE, canvas.fill.color) else Fill(Fill.SOLID, c, canvas.fill.color2, canvas.fill.angle)
                val f = canvas.fill
                applyToSelection { it.fill = f.copy() }
                rebuildOptions()
            }
        }
        fillChip.color = if (canvas.fill.type == Fill.NONE) null else canvas.fill.color
        options.add(label(ctx, "Fill", 11f)); options.add(fillChip)

        val gradBtn = textButton(ctx, when (canvas.fill.type) { Fill.LINEAR -> "Linear"; Fill.RADIAL -> "Radial"; else -> "Grad…" }) { gradientDialog() }
        options.add(gradBtn)

        val strokeChip = ColorChip(ctx, hollow = true) {
            showColorPicker(ctx, "Stroke", canvas.stroke.color, true) { c ->
                if (c == null) canvas.stroke.enabled = false else { canvas.stroke.enabled = true; canvas.stroke.color = c }
                val s = canvas.stroke
                applyToSelection { it.stroke = s.copy() }
                rebuildOptions()
            }
        }
        strokeChip.color = if (canvas.stroke.enabled) canvas.stroke.color else null
        options.add(label(ctx, "Stroke", 11f)); options.add(strokeChip)
        options.add(SliderRow(ctx, "W", 0.5f, doc.width / 20f, canvas.stroke.width, { String.format("%.1f", it) }, 110) { v, done ->
            canvas.stroke.width = v
            if (done) applyToSelection { it.stroke.width = v; if (!it.stroke.enabled) it.stroke.enabled = canvas.stroke.enabled }
        })
    }

    private fun gradientDialog() {
        val f = canvas.fill.copy()
        if (f.type == Fill.NONE || f.type == Fill.SOLID) f.type = Fill.LINEAR
        val box = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(16), dp(8), dp(16), 0) }
        val types = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        val c1 = ColorChip(ctx) { chip -> showColorPicker(ctx, "Start colour", f.color) { c -> if (c != null) { f.color = c; chip.color = c } } }
        val c2 = ColorChip(ctx) { chip -> showColorPicker(ctx, "End colour", f.color2) { c -> if (c != null) { f.color2 = c; chip.color = c } } }
        c1.color = f.color; c2.color = f.color2
        val linear = textButton(ctx, "Linear", f.type == Fill.LINEAR) {}
        val radial = textButton(ctx, "Radial", f.type == Fill.RADIAL) {}
        fun refresh() {
            linear.background = roundRect(if (f.type == Fill.LINEAR) Theme.ACCENT else Theme.PANEL_2, dp(8f))
            radial.background = roundRect(if (f.type == Fill.RADIAL) Theme.ACCENT else Theme.PANEL_2, dp(8f))
        }
        linear.setOnClickListener { f.type = Fill.LINEAR; refresh() }
        radial.setOnClickListener { f.type = Fill.RADIAL; refresh() }
        types.addView(linear); types.addView(radial)
        box.addView(types)
        box.addView(LinearLayout(ctx).apply {
            gravity = Gravity.CENTER_VERTICAL
            addView(label(ctx, "Start")); addView(c1); addView(label(ctx, "End")); addView(c2)
            addView(textButton(ctx, "Swap") { val t = f.color; f.color = f.color2; f.color2 = t; c1.color = f.color; c2.color = f.color2 })
        })
        box.addView(SliderRow(ctx, "Angle", 0f, 360f, f.angle, { "${it.roundToInt()}°" }, 160) { v, _ -> f.angle = v })
        AlertDialog.Builder(ctx).setTitle("Gradient fill").setView(box)
            .setPositiveButton("Apply") { _, _ -> canvas.fill = f; applyToSelection { it.fill = f.copy() }; rebuildOptions() }
            .setNeutralButton("Solid") { _, _ ->
                canvas.fill = Fill(Fill.SOLID, f.color); applyToSelection { it.fill = Fill(Fill.SOLID, f.color) }; rebuildOptions()
            }
            .setNegativeButton("Cancel", null).show()
    }

    fun rebuildOptions() {
        options.clear()
        val sel = canvas.selection
        when (canvas.tool) {
            VTool.SELECT -> {
                addStyleControls()
                if (sel.isNotEmpty()) {
                    options.divider()
                    val first = sel.first()
                    val live = LiveEdit { canvas.checkpoint() }
                    options.add(SliderRow(ctx, "Opacity", 0f, 100f, first.opacity * 100, { "${it.roundToInt()}%" }, 100) { v, done ->
                        live.change(done) { sel.forEach { it.opacity = v / 100f }; if (done) canvas.changed() else canvas.invalidate() }
                    })
                    options.add(textButton(ctx, BLEND_NAMES[first.blend]) {
                        choose(ctx, "Blend mode", BLEND_NAMES) { i -> canvas.editSelection { it.blend = i }; rebuildOptions() }
                    })
                    (sel.singleOrNull() as? TextShape)?.let { addTextControls(it) }
                    options.divider()
                    options.add(IconButton(ctx, "copy", "Duplicate") { canvas.duplicateSelection() })
                    options.add(IconButton(ctx, "trash", "Delete") { canvas.deleteSelection() })
                    options.add(IconButton(ctx, "arrange", "Arrange") { arrangeMenu(it) })
                    options.add(IconButton(ctx, "align", "Align to artboard") { alignMenu(it) })
                    options.add(IconButton(ctx, "flip_h", "Flip horizontal") { canvas.flip(true) })
                    options.add(IconButton(ctx, "flip_v", "Flip vertical") { canvas.flip(false) })
                    options.add(IconButton(ctx, "rotate", "Rotate 90°") { canvas.rotateBy(90f) })
                }
                options.divider()
                options.add(toggle("Keep ratio", canvas.proportional) { canvas.proportional = it })
                options.add(textButton(ctx, "Select all") { canvas.selectAll() })
            }
            VTool.DIRECT -> {
                addStyleControls()
                options.divider()
                options.add(textButton(ctx, "Delete point") { canvas.deleteActiveNode() })
                options.add(textButton(ctx, "Smooth / Corner") { canvas.toggleNodeSmooth() })
                options.add(textButton(ctx, "Add point") { canvas.addNodeAfterActive() })
                options.add(textButton(ctx, "Open / Close") { canvas.togglePathClosed() })
            }
            VTool.PEN -> {
                addStyleControls()
                options.divider()
                options.add(textButton(ctx, "Finish path", true) { canvas.finishPen() })
                options.add(label(ctx, "Tap = corner · drag = curve · tap first point to close", 11f))
            }
            VTool.PENCIL -> {
                addStyleControls()
                options.add(label(ctx, "Draw freely – converted to a smooth editable path", 11f))
            }
            VTool.BRUSH -> {
                val chip = ColorChip(ctx) {
                    showColorPicker(ctx, "Brush colour", canvas.fill.color) { c -> if (c != null) { canvas.fill = Fill(Fill.SOLID, c); rebuildOptions() } }
                }
                chip.color = canvas.fill.color
                options.add(chip)
                options.add(textButton(ctx, when (canvas.fill.type) { Fill.LINEAR -> "Linear"; Fill.RADIAL -> "Radial"; else -> "Grad…" }) { gradientDialog() })
                options.add(SliderRow(ctx, "Size", 1f, doc.width / 8f, canvas.brushSize, widthDp = 130) { v, _ -> canvas.brushSize = v })
                options.add(SliderRow(ctx, "Pressure", 0f, 100f, (1 - canvas.brushMinRatio) * 100, { "${it.roundToInt()}%" }, 100) { v, _ ->
                    canvas.brushMinRatio = 1 - v / 100f
                })
                options.add(SliderRow(ctx, "Smooth", 0f, 90f, canvas.brushSmoothing * 100, widthDp = 100) { v, _ -> canvas.brushSmoothing = v / 100f })
                options.add(toggle("Taper", canvas.brushTaper) { canvas.brushTaper = it })
            }
            VTool.RECT -> {
                addStyleControls()
                options.add(SliderRow(ctx, "Corner", 0f, doc.width / 6f, canvas.cornerRadius, widthDp = 110) { v, done ->
                    canvas.cornerRadius = v
                    if (done) canvas.selection.filterIsInstance<RectShape>().takeIf { it.isNotEmpty() }?.let { canvas.editSelection { s -> (s as? RectShape)?.radius = v } }
                })
                options.add(toggle("Square", canvas.constrain) { canvas.constrain = it })
            }
            VTool.ELLIPSE -> { addStyleControls(); options.add(toggle("Circle", canvas.constrain) { canvas.constrain = it }) }
            VTool.POLY -> {
                addStyleControls()
                options.add(toggle("Star", canvas.polyStar) { canvas.polyStar = it })
                options.add(SliderRow(ctx, "Points", 3f, 24f, canvas.polySides.toFloat(), widthDp = 100) { v, _ -> canvas.polySides = v.roundToInt() })
                options.add(SliderRow(ctx, "Inner", 10f, 95f, canvas.polyInner * 100, { "${it.roundToInt()}%" }, 90) { v, _ -> canvas.polyInner = v / 100f })
                options.add(toggle("Even", canvas.constrain) { canvas.constrain = it })
            }
            VTool.LINE -> { addStyleControls(); options.add(toggle("45° snap", canvas.constrain) { canvas.constrain = it }) }
            VTool.TEXT -> {
                addStyleControls()
                options.add(textButton(ctx, Fonts.displayName(canvas.textFont)) { fontPicker { f -> canvas.textFont = f; rebuildOptions() } })
                options.add(toggle("B", canvas.textBold) { canvas.textBold = it })
                options.add(toggle("I", canvas.textItalic) { canvas.textItalic = it })
                options.add(label(ctx, "Tap the artboard to add text", 11f))
            }
            VTool.ERASER -> {
                options.add(SliderRow(ctx, "Size", 2f, doc.width / 6f, canvas.eraserSize, widthDp = 150) { v, _ -> canvas.eraserSize = v })
                options.add(label(ctx, "Removes whole objects you touch", 11f))
            }
            VTool.EYEDROPPER -> options.add(label(ctx, "Tap artwork to pick its colour (tip: stylus button works in any tool)", 11f))
            VTool.HAND -> {
                options.add(textButton(ctx, "Fit") { canvas.fitToScreen() })
                options.add(textButton(ctx, "100%") { canvas.actualPixels() })
            }
        }
    }

    private fun addTextControls(t: TextShape) {
        options.divider()
        options.add(textButton(ctx, "Edit text") {
            promptText(ctx, "Edit text", t.text, true) { s -> if (s.isNotBlank()) canvas.editSelection { (it as? TextShape)?.text = s } }
        })
        options.add(textButton(ctx, Fonts.displayName(t.font)) {
            fontPicker { f -> canvas.textFont = f; canvas.editSelection { (it as? TextShape)?.font = f }; rebuildOptions() }
        })
        options.add(toggle("B", t.bold) { v -> canvas.editSelection { (it as? TextShape)?.bold = v } })
        options.add(toggle("I", t.italic) { v -> canvas.editSelection { (it as? TextShape)?.italic = v } })
        options.add(textButton(ctx, listOf("Left", "Center", "Right")[t.align]) {
            choose(ctx, "Align", listOf("Left", "Center", "Right")) { i -> canvas.editSelection { (it as? TextShape)?.align = i }; rebuildOptions() }
        })
        val live = LiveEdit { canvas.checkpoint() }
        options.add(SliderRow(ctx, "Tracking", -0.2f, 1f, t.letterSpacing, { String.format("%.2f", it) }, 100) { v, done ->
            live.change(done) { t.letterSpacing = v; t.invalidate(); if (done) canvas.changed() else canvas.invalidate() }
        })
        options.add(SliderRow(ctx, "Leading", 0.6f, 2.5f, t.lineHeight, { String.format("%.2f", it) }, 100) { v, done ->
            live.change(done) { t.lineHeight = v; t.invalidate(); if (done) canvas.changed() else canvas.invalidate() }
        })
    }

    private fun fontPicker(onPick: (String) -> Unit) {
        val imported = ProjectStore.fontsDir(ctx).listFiles()?.filter { it.extension.lowercase() in listOf("ttf", "otf") }?.map { it.path } ?: emptyList()
        val all = Fonts.families + imported
        val labels = all.map { Fonts.displayName(it) } + "+ Import font file (.ttf / .otf)…"
        choose(ctx, "Font", labels) { i ->
            if (i < all.size) onPick(all[i]) else importFont(onPick)
        }
    }

    private fun importFont(onPick: (String) -> Unit) {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply { addCategory(Intent.CATEGORY_OPENABLE); type = "*/*" }
        host.launchForResult(i) { data ->
            val uri = data?.data ?: return@launchForResult
            var name = "font_${System.currentTimeMillis()}"
            ctx.contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) name = c.getString(0)
            }
            if (!name.lowercase().endsWith(".ttf") && !name.lowercase().endsWith(".otf")) name += ".ttf"
            val out = File(ProjectStore.fontsDir(ctx), name.replace("/", "_"))
            try {
                ctx.contentResolver.openInputStream(uri)?.use { input -> out.outputStream().use { input.copyTo(it) } }
                Typeface.createFromFile(out)
                onPick(out.path)
            } catch (e: Exception) {
                out.delete(); toast(ctx, "That file isn't a usable font")
            }
        }
    }

    private fun arrangeMenu(anchor: View) {
        val pm = PopupMenu(ctx, anchor)
        listOf("Bring to front" to 2, "Bring forward" to 1, "Send backward" to -1, "Send to back" to -2).forEach { (t, d) ->
            pm.menu.add(t).setOnMenuItemClickListener { canvas.arrange(d); true }
        }
        pm.show()
    }

    private fun alignMenu(anchor: View) {
        val pm = PopupMenu(ctx, anchor)
        pm.menu.add("Center on artboard").setOnMenuItemClickListener { canvas.alignToArtboard(0, 0); true }
        pm.menu.add("Center horizontally").setOnMenuItemClickListener { canvas.alignToArtboard(0, null); true }
        pm.menu.add("Center vertically").setOnMenuItemClickListener { canvas.alignToArtboard(null, 0); true }
        pm.menu.add("Align left").setOnMenuItemClickListener { canvas.alignToArtboard(-1, null); true }
        pm.menu.add("Align right").setOnMenuItemClickListener { canvas.alignToArtboard(1, null); true }
        pm.menu.add("Align top").setOnMenuItemClickListener { canvas.alignToArtboard(null, -1); true }
        pm.menu.add("Align bottom").setOnMenuItemClickListener { canvas.alignToArtboard(null, 1); true }
        pm.show()
    }

    // ------------------------------------------------------------------ layers panel

    override fun toggleLayers() {
        layersVisible = !layersVisible
        layersScroll.visibility = if (layersVisible) View.VISIBLE else View.GONE
        if (layersVisible) rebuildLayers()
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
            val row = LinearLayout(ctx).apply {
                orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
                background = roundRect(if (i == doc.active) Theme.ACCENT_DIM else 0, dp(8f), if (i == doc.active) Theme.ACCENT else 0, if (i == doc.active) dp(1) else 0)
                setPadding(dp(2), dp(2), dp(6), dp(2))
            }
            row.addView(IconButton(ctx, if (l.visible) "eye" else "eye_off", "Show / hide") {
                canvas.checkpoint(); l.visible = !l.visible; canvas.changed(); rebuildLayers()
            })
            row.addView(IconButton(ctx, if (l.locked) "lock" else "unlock", "Lock") {
                canvas.checkpoint(); l.locked = !l.locked; canvas.changed(); rebuildLayers()
            })
            val name = TextView(ctx).apply {
                text = "${l.name}  ·  ${l.shapes.size}"; textSize = 14f; setTextColor(Theme.TEXT)
                setPadding(dp(4), dp(10), dp(4), dp(10))
            }
            row.addView(name, LinearLayout.LayoutParams(0, -2, 1f))
            row.setOnClickListener { doc.active = i; canvas.select(emptyList()); rebuildLayers() }
            row.setOnLongClickListener {
                choose(ctx, l.name, listOf("Rename", "Duplicate", "Delete", "Select all on layer")) { which ->
                    when (which) {
                        0 -> promptText(ctx, "Rename layer", l.name) { n -> canvas.checkpoint(); l.name = n; canvas.changed(); rebuildLayers() }
                        1 -> { canvas.checkpoint(); doc.layers.add(i + 1, l.copy().also { c -> c.name = l.name + " copy"; c.shapes.forEach { it.id = newId() } }); doc.active = i + 1; canvas.changed(); rebuildLayers() }
                        2 -> deleteLayer(i)
                        3 -> { doc.active = i; canvas.select(l.shapes.toList()) }
                    }
                }
                true
            }
            layersPanel.addView(row, LinearLayout.LayoutParams(-1, -2).apply { setMargins(0, dp(2), 0, dp(2)) })
        }
        val active = doc.activeLayer
        val live = LiveEdit { canvas.checkpoint() }
        layersPanel.addView(SliderRow(ctx, "Opacity", 0f, 100f, active.opacity * 100, { "${it.roundToInt()}%" }, 120) { v, done ->
            live.change(done) { active.opacity = v / 100f; if (done) canvas.changed() else canvas.invalidate() }
        })
        layersPanel.addView(LinearLayout(ctx).apply {
            addView(IconButton(ctx, "plus", "New layer") {
                canvas.checkpoint(); doc.layers.add(doc.active + 1, VectorLayer("Layer ${doc.layers.size + 1}")); doc.active++; canvas.changed(); rebuildLayers()
            })
            addView(IconButton(ctx, "up", "Move layer up") { moveLayer(1) })
            addView(IconButton(ctx, "down", "Move layer down") { moveLayer(-1) })
            addView(IconButton(ctx, "trash", "Delete layer") { deleteLayer(doc.active) })
            addView(IconButton(ctx, "send", "Move selection to this layer") { moveSelectionToActiveLayer() })
        })
        val bg = ColorChip(ctx) {
            showColorPicker(ctx, "Artboard background", doc.background, true) { c ->
                canvas.checkpoint(); doc.background = c ?: Color.TRANSPARENT; canvas.changed(); rebuildLayers()
            }
        }
        bg.color = if (Color.alpha(doc.background) == 0) null else doc.background
        layersPanel.addView(LinearLayout(ctx).apply {
            gravity = Gravity.CENTER_VERTICAL
            addView(label(ctx, "Artboard background")); addView(bg)
        })
    }

    private fun moveLayer(dir: Int) {
        val i = doc.active; val j = i + dir
        if (j !in doc.layers.indices) return
        canvas.checkpoint()
        val t = doc.layers[i]; doc.layers[i] = doc.layers[j]; doc.layers[j] = t; doc.active = j
        canvas.changed(); rebuildLayers()
    }

    private fun deleteLayer(i: Int) {
        if (doc.layers.size <= 1) { toast(ctx, "Can't delete the only layer"); return }
        canvas.checkpoint(); doc.layers.removeAt(i); doc.active = doc.active.coerceIn(0, doc.layers.size - 1)
        canvas.select(emptyList()); canvas.changed(); rebuildLayers()
    }

    private fun moveSelectionToActiveLayer() {
        if (canvas.selection.isEmpty()) { toast(ctx, "Select objects first"); return }
        val target = doc.activeLayer
        canvas.checkpoint()
        for (s in canvas.selection) { doc.findLayerOf(s)?.shapes?.remove(s); target.shapes.add(s) }
        canvas.changed(); rebuildLayers()
    }

    // ------------------------------------------------------------------ main menu

    var onSendToPhoto: ((linked: Boolean, withBg: Boolean) -> Unit)? = null

    override fun showMenu(anchor: View) {
        val pm = PopupMenu(ctx, anchor)
        val m = pm.menu
        val send = m.addSubMenu("Send to Photo Studio")
        send.add("As linked illustration (updates live)").setOnMenuItemClickListener { onSendToPhoto?.invoke(true, false); true }
        send.add("As linked illustration + background").setOnMenuItemClickListener { onSendToPhoto?.invoke(true, true); true }
        send.add("As flattened pixel layer").setOnMenuItemClickListener { onSendToPhoto?.invoke(false, true); true }
        val edit = m.addSubMenu("Edit")
        edit.add("Select all").setOnMenuItemClickListener { selectTool(VTool.SELECT); canvas.selectAll(); true }
        edit.add("Deselect").setOnMenuItemClickListener { canvas.select(emptyList()); true }
        edit.add("Duplicate").setOnMenuItemClickListener { canvas.duplicateSelection(); true }
        edit.add("Delete").setOnMenuItemClickListener { canvas.deleteSelection(); true }
        edit.add("Convert shape to editable path").setOnMenuItemClickListener { canvas.convertSelectionToPath(); true }
        edit.add("Flip horizontal").setOnMenuItemClickListener { canvas.flip(true); true }
        edit.add("Flip vertical").setOnMenuItemClickListener { canvas.flip(false); true }
        edit.add("Scale selection…").setOnMenuItemClickListener {
            promptText(ctx, "Scale selection (%)", "100") { s ->
                val k = (s.toFloatOrNull() ?: 100f) / 100f
                canvas.selectionBounds()?.let { b -> val mx = Matrix(); mx.setScale(k, k, b.centerX(), b.centerY()); canvas.transformSelection(mx) }
            }; true
        }
        m.add("Artboard background…").setOnMenuItemClickListener {
            showColorPicker(ctx, "Artboard background", doc.background, true) { c ->
                canvas.checkpoint(); doc.background = c ?: Color.TRANSPARENT; canvas.changed()
            }; true
        }
        val exp = m.addSubMenu("Export")
        exp.add("PNG (full size)").setOnMenuItemClickListener { exportPng(false); true }
        exp.add("PNG (transparent background)").setOnMenuItemClickListener { exportPng(true); true }
        exp.add("SVG vector file").setOnMenuItemClickListener { exportSvgFile(); true }
        m.add("Fit to screen").setOnMenuItemClickListener { canvas.fitToScreen(); true }
        m.add("Stylus & input settings…").setOnMenuItemClickListener { host.showInputSettings(); true }
        pm.show()
    }

    private fun exportPng(transparent: Boolean) {
        host.setBusy(true)
        Thread {
            val bmp = canvas.renderToBitmap(transparent = transparent)
            val uri = ProjectStore.exportImage(ctx, bmp, host.project.name, false)
            ctx.runOnUiThread {
                host.setBusy(false)
                if (uri == null) toast(ctx, "Export failed")
                else AlertDialog.Builder(ctx).setMessage("Saved to Pictures/CoverStudio (${bmp.width}×${bmp.height})")
                    .setPositiveButton("Share") { _, _ -> ProjectStore.share(ctx, uri, "image/png") }
                    .setNegativeButton("Done", null).show()
            }
        }.start()
    }

    private fun exportSvgFile() {
        val svg = exportSvg(doc)
        val uri = ProjectStore.exportText(ctx, svg, host.project.name, "svg", "image/svg+xml")
        if (uri == null) toast(ctx, "Export failed")
        else AlertDialog.Builder(ctx).setMessage("Saved to Download/CoverStudio")
            .setPositiveButton("Share") { _, _ -> ProjectStore.share(ctx, uri, "image/svg+xml") }
            .setNegativeButton("Done", null).show()
    }
}
