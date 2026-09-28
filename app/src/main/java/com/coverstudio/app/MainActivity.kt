package com.coverstudio.app

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.BitmapFactory
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.GridLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import com.coverstudio.app.common.InputMode
import com.coverstudio.app.common.InputSettings
import com.coverstudio.app.common.Studio
import com.coverstudio.app.common.StudioHost
import com.coverstudio.app.photo.PhotoStudio
import com.coverstudio.app.photo.RasterDoc
import com.coverstudio.app.project.ProjectInfo
import com.coverstudio.app.project.ProjectStore
import com.coverstudio.app.ui.IconButton
import com.coverstudio.app.ui.SliderRow
import com.coverstudio.app.ui.Theme
import com.coverstudio.app.ui.choose
import com.coverstudio.app.ui.dp
import com.coverstudio.app.ui.label
import com.coverstudio.app.ui.promptText
import com.coverstudio.app.ui.roundRect
import com.coverstudio.app.ui.textButton
import com.coverstudio.app.ui.toast
import com.coverstudio.app.vector.VectorDoc
import com.coverstudio.app.vector.VectorStudio
import java.text.DateFormat
import java.util.Date
import java.util.concurrent.Executors

class MainActivity : Activity(), StudioHost {
    override val activity: Activity get() = this
    private val io = Executors.newSingleThreadExecutor()
    private val resultCallbacks = HashMap<Int, (Intent?) -> Unit>()
    private var nextRequest = 100

    // editor state
    private var currentProject: ProjectInfo? = null
    override val project: ProjectInfo get() = currentProject!!
    private var vector: VectorStudio? = null
    private var photo: PhotoStudio? = null
    private var active: Studio? = null
    private var dirty = false
    private var busyView: View? = null
    private var undoBtn: IconButton? = null
    private var redoBtn: IconButton? = null
    private var modeVector: TextView? = null
    private var modePhoto: TextView? = null
    private var titleView: TextView? = null
    private var content: FrameLayout? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = Theme.PANEL
        window.navigationBarColor = Theme.PANEL
        window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS)
        loadSettings()
        showProjects()
    }

    // ------------------------------------------------------------------ settings

    private fun loadSettings() {
        val p = getSharedPreferences("settings", MODE_PRIVATE)
        InputSettings.mode = InputMode.values()[p.getInt("inputMode", 0).coerceIn(0, 2)]
        InputSettings.pressureGamma = p.getFloat("gamma", 1f)
        InputSettings.twoFingerTapUndo = p.getBoolean("tapUndo", true)
    }

    private fun saveSettings() {
        getSharedPreferences("settings", MODE_PRIVATE).edit()
            .putInt("inputMode", InputSettings.mode.ordinal)
            .putFloat("gamma", InputSettings.pressureGamma)
            .putBoolean("tapUndo", InputSettings.twoFingerTapUndo)
            .apply()
    }

    override fun showInputSettings() {
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(18), dp(8), dp(18), 0) }
        val modes = listOf(
            "Auto – fingers draw until the stylus is used, then fingers only zoom/pan (palm rejection)",
            "Stylus only – fingers always zoom/pan",
            "Stylus and fingers both draw"
        )
        val modeLabel = label(this, modes[InputSettings.mode.ordinal], 13f, Theme.TEXT)
        box.addView(label(this, "Drawing input", 12f))
        box.addView(modeLabel)
        box.addView(textButton(this, "Change…") {
            choose(this, "Drawing input", modes) { i -> InputSettings.mode = InputMode.values()[i]; modeLabel.text = modes[i]; saveSettings() }
        })
        box.addView(label(this, "Pressure curve (lower = lighter touch gives thicker lines)", 12f))
        box.addView(SliderRow(this, "", 0.3f, 2.5f, InputSettings.pressureGamma, { String.format("%.2f", it) }, 180) { v, done ->
            InputSettings.pressureGamma = v; if (done) saveSettings()
        })
        val tap = label(this, "", 13f, Theme.TEXT)
        fun refreshTap() { tap.text = "Two-finger tap = undo, three-finger tap = redo: " + if (InputSettings.twoFingerTapUndo) "ON" else "OFF" }
        refreshTap()
        box.addView(tap)
        box.addView(textButton(this, "Toggle") { InputSettings.twoFingerTapUndo = !InputSettings.twoFingerTapUndo; refreshTap(); saveSettings() })
        box.addView(label(this, "Tips: pinch with two fingers to zoom · stylus side button = eyedropper (clone source in the Clone tool) · stylus eraser end erases.", 12f))
        AlertDialog.Builder(this).setTitle("Stylus & input").setView(ScrollView(this).apply { addView(box) })
            .setPositiveButton("Done", null).show()
    }

    // ------------------------------------------------------------------ projects screen

    private fun showProjects() {
        currentProject = null; vector = null; photo = null; active = null
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setBackgroundColor(Theme.BG) }
        val header = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(16), dp(14), dp(12), dp(14))
            setBackgroundColor(Theme.PANEL)
        }
        header.addView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(TextView(this@MainActivity).apply { text = "Cover Studio"; textSize = 22f; setTextColor(Theme.TEXT); paint.isFakeBoldText = true })
            addView(TextView(this@MainActivity).apply { text = "Vector illustration + photo editing for album art"; textSize = 12f; setTextColor(Theme.TEXT_DIM) })
        }, LinearLayout.LayoutParams(0, -2, 1f))
        header.addView(IconButton(this, "settings", "Stylus & input settings") { showInputSettings() })
        header.addView(textButton(this, "+ New cover", true) { newProjectDialog() })
        root.addView(header)

        val grid = GridLayout(this).apply {
            val cols = maxOf(2, (resources.displayMetrics.widthPixels / dp(170)))
            columnCount = cols
            setPadding(dp(8), dp(8), dp(8), dp(8))
        }
        val projects = ProjectStore.list(this)
        if (projects.isEmpty()) {
            root.addView(TextView(this).apply {
                text = "No covers yet.\n\nTap “+ New cover” to start a square artboard. Draw it in the Vector studio, then switch to the Photo studio to add photos, textures, blend modes, masks and filters."
                textSize = 15f; setTextColor(Theme.TEXT_DIM); gravity = Gravity.CENTER; setPadding(dp(32), dp(48), dp(32), dp(32))
            })
        }
        val cellW = (resources.displayMetrics.widthPixels - dp(16)) / grid.columnCount - dp(8)
        for (p in projects) {
            val card = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                background = roundRect(Theme.PANEL, dp(10f))
                setPadding(dp(6), dp(6), dp(6), dp(8))
                isClickable = true
                setOnClickListener { openProject(p) }
                setOnLongClickListener { projectMenu(p); true }
            }
            val img = ImageView(this).apply {
                scaleType = ImageView.ScaleType.CENTER_CROP
                setBackgroundColor(Theme.PANEL_2)
                if (p.thumb.exists()) setImageBitmap(BitmapFactory.decodeFile(p.thumb.path))
            }
            card.addView(img, LinearLayout.LayoutParams(-1, cellW - dp(12)))
            card.addView(TextView(this).apply { text = p.name; textSize = 14f; setTextColor(Theme.TEXT); setPadding(dp(2), dp(6), 0, 0); maxLines = 1 })
            card.addView(TextView(this).apply {
                text = "${p.width}×${p.height} · ${DateFormat.getDateInstance(DateFormat.SHORT).format(Date(p.modified))}"
                textSize = 11f; setTextColor(Theme.TEXT_DIM); setPadding(dp(2), 0, 0, 0)
            })
            grid.addView(card, GridLayout.LayoutParams().apply { width = cellW; setMargins(dp(4), dp(4), dp(4), dp(4)) })
        }
        root.addView(ScrollView(this).apply { addView(grid) }, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(root)
    }

    private fun projectMenu(p: ProjectInfo) {
        choose(this, p.name, listOf("Open", "Rename", "Duplicate", "Delete")) { i ->
            when (i) {
                0 -> openProject(p)
                1 -> promptText(this, "Rename", p.name) { ProjectStore.rename(p, it); showProjects() }
                2 -> { ProjectStore.duplicate(this, p); showProjects() }
                3 -> AlertDialog.Builder(this).setTitle("Delete “${p.name}”?").setMessage("This can't be undone.")
                    .setPositiveButton("Delete") { _, _ -> ProjectStore.delete(p); showProjects() }
                    .setNegativeButton("Cancel", null).show()
            }
        }
    }

    private fun newProjectDialog() {
        val sizes = listOf(
            "3000 × 3000 – streaming standard (Spotify / Apple Music)" to 3000,
            "4000 × 4000 – print / vinyl" to 4000,
            "2400 × 2400 – lighter on memory" to 2400,
            "1400 × 1400 – minimum for podcasts / Apple" to 1400,
            "Custom square…" to -1
        )
        choose(this, "New cover – artboard size", sizes.map { it.first }) { i ->
            val s = sizes[i].second
            fun create(size: Int) {
                promptText(this, "Cover name", "Untitled cover") { name ->
                    val p = ProjectStore.create(this, name.ifBlank { "Untitled cover" }, size, size)
                    openProject(p)
                }
            }
            if (s > 0) create(s) else promptText(this, "Square size in pixels (500–5000)", "3000") { t ->
                create((t.toIntOrNull() ?: 3000).coerceIn(500, 5000))
            }
        }
    }

    private fun openProject(p: ProjectInfo) {
        val loading = FrameLayout(this).apply {
            setBackgroundColor(Theme.BG)
            addView(ProgressBar(this@MainActivity), FrameLayout.LayoutParams(-2, -2, Gravity.CENTER))
        }
        setContentView(loading)
        io.execute {
            val v = ProjectStore.loadVector(p)
            val r = try { ProjectStore.loadRaster(p) } catch (e: OutOfMemoryError) { null }
            runOnUiThread {
                if (r == null) { toast(this, "Not enough memory to open this project"); showProjects() }
                else showEditor(p, v, r)
            }
        }
    }

    // ------------------------------------------------------------------ editor screen

    private fun showEditor(p: ProjectInfo, v: VectorDoc, r: RasterDoc) {
        currentProject = p
        ProjectStore.forgetVersions()
        ProjectStore.markSaved(p.dir, r)
        dirty = false
        val vs = VectorStudio(this, v)
        val ps = PhotoStudio(this, r) { vs.doc }
        vector = vs; photo = ps
        vs.onSendToPhoto = { linked, withBg ->
            switchToPhoto()
            ps.placeIllustration(linked, withBg)
            toast(this, if (linked) "Placed as a linked layer – edits in Vector update it" else "Placed as a pixel layer")
        }

        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setBackgroundColor(Theme.BG) }
        val top = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(Theme.PANEL)
            setPadding(dp(2), 0, dp(2), 0)
        }
        top.addView(IconButton(this, "back", "Save & close") { closeEditor() })
        titleView = TextView(this).apply {
            text = p.name; textSize = 15f; setTextColor(Theme.TEXT); maxLines = 1
            setPadding(dp(4), 0, dp(4), 0)
            setOnClickListener {
                promptText(this@MainActivity, "Rename", project.name) { n ->
                    ProjectStore.rename(project, n); currentProject = ProjectStore.info(project.dir); text = n
                }
            }
        }
        top.addView(titleView, LinearLayout.LayoutParams(0, -2, 1f))

        val seg = LinearLayout(this).apply { background = roundRect(Theme.PANEL_2, dp(10f)); setPadding(dp(2), dp(2), dp(2), dp(2)) }
        modeVector = TextView(this).apply {
            text = "Vector"; textSize = 13f; gravity = Gravity.CENTER; setPadding(dp(10), dp(6), dp(10), dp(6))
            setOnClickListener { switchTo(vs) }
        }
        modePhoto = TextView(this).apply {
            text = "Photo"; textSize = 13f; gravity = Gravity.CENTER; setPadding(dp(10), dp(6), dp(10), dp(6))
            setOnClickListener { switchTo(ps) }
        }
        seg.addView(modeVector); seg.addView(modePhoto)
        top.addView(seg)
        undoBtn = IconButton(this, "undo", "Undo (or two-finger tap)") { active?.undo(); refreshTopBar() }
        redoBtn = IconButton(this, "redo", "Redo (or three-finger tap)") { active?.redo(); refreshTopBar() }
        top.addView(undoBtn); top.addView(redoBtn)
        top.addView(IconButton(this, "layers", "Layers") { active?.toggleLayers() })
        top.addView(IconButton(this, "menu", "Menu") { active?.showMenu(it) })
        root.addView(top, LinearLayout.LayoutParams(-1, dp(50)))

        val c = FrameLayout(this)
        c.addView(vs.root, FrameLayout.LayoutParams(-1, -1))
        c.addView(ps.root, FrameLayout.LayoutParams(-1, -1))
        content = c
        root.addView(c, LinearLayout.LayoutParams(-1, 0, 1f))

        val frame = FrameLayout(this)
        frame.addView(root, FrameLayout.LayoutParams(-1, -1))
        val busy = FrameLayout(this).apply {
            setBackgroundColor(0x66000000); isClickable = true; visibility = View.GONE
            addView(ProgressBar(this@MainActivity), FrameLayout.LayoutParams(-2, -2, Gravity.CENTER))
        }
        busyView = busy
        frame.addView(busy, FrameLayout.LayoutParams(-1, -1))
        setContentView(frame)

        // open in photo mode if the project already has photo work, otherwise start drawing
        val photoUsed = r.layers.size > 1 || r.layers.any { it.smart }
        switchTo(if (photoUsed) ps else vs)
    }

    private fun switchTo(s: Studio) {
        if (active === s) return
        active?.onHidden()
        active = s
        vector?.root?.visibility = if (s === vector) View.VISIBLE else View.GONE
        photo?.root?.visibility = if (s === photo) View.VISIBLE else View.GONE
        val isVector = s === vector
        modeVector?.apply { background = if (isVector) roundRect(Theme.ACCENT, dp(8f)) else null; setTextColor(if (isVector) Color.WHITE else Theme.TEXT_DIM) }
        modePhoto?.apply { background = if (!isVector) roundRect(Theme.PHOTO_ACCENT, dp(8f)) else null; setTextColor(if (!isVector) Color.WHITE else Theme.TEXT_DIM) }
        s.onShown()
        refreshTopBar()
    }

    override fun switchToPhoto() { photo?.let { switchTo(it) } }

    override fun refreshTopBar() {
        undoBtn?.isEnabled = active?.canUndo == true; undoBtn?.invalidate()
        redoBtn?.isEnabled = active?.canRedo == true; redoBtn?.invalidate()
    }

    override fun markDirty() { dirty = true }

    override fun setBusy(busy: Boolean) { busyView?.visibility = if (busy) View.VISIBLE else View.GONE }

    override fun launchForResult(intent: Intent, onResult: (Intent?) -> Unit) {
        val code = nextRequest++
        resultCallbacks[code] = onResult
        try {
            @Suppress("DEPRECATION")
            startActivityForResult(intent, code)
        } catch (e: Exception) {
            resultCallbacks.remove(code); toast(this, "No app available for that")
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        resultCallbacks.remove(requestCode)?.invoke(if (resultCode == RESULT_OK) data else null)
    }

    // ------------------------------------------------------------------ saving

    private fun save(then: (() -> Unit)? = null) {
        val p = currentProject
        val vs = vector; val ps = photo
        if (p == null || vs == null || ps == null) { then?.invoke(); return }
        vs.canvas.finishPen()
        ps.canvas.commitTransform()
        val vjson = vs.doc.toJson().toString()
        val rsnap = ProjectStore.snapshotRaster(p.dir, ps.doc)
        val usePhoto = ps.doc.layers.size > 1 || ps.doc.layers.any { it.version > 0 || it.smart }
        val thumbSource = try {
            if (usePhoto) ps.doc.flatten(320f / maxOf(ps.doc.width, ps.doc.height)) else vs.canvas.renderToBitmap(320)
        } catch (e: OutOfMemoryError) { null }
        dirty = false
        io.execute {
            try {
                ProjectStore.saveVector(p.dir, vjson)
                ProjectStore.writeRaster(p.dir, rsnap)
                thumbSource?.let { ProjectStore.writeThumb(p.dir, it) }
                ProjectStore.writeMeta(p.dir, p.name, p.width, p.height)
            } catch (e: Exception) {
                runOnUiThread { toast(this, "Saving failed: ${e.message}") }
            }
            then?.let { runOnUiThread(it) }
        }
    }

    private fun closeEditor() {
        setBusy(true)
        save {
            photo?.canvas?.release()
            showProjects()
        }
    }

    override fun onPause() {
        super.onPause()
        if (currentProject != null && dirty) save()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (currentProject != null) closeEditor()
        else {
            @Suppress("DEPRECATION")
            super.onBackPressed()
        }
    }

    /** Keeps content clear of the status / navigation bars (Android 15 draws edge-to-edge). */
    override fun setContentView(view: View) {
        view.setOnApplyWindowInsetsListener { v, insets ->
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                val i = insets.getInsets(android.view.WindowInsets.Type.systemBars() or android.view.WindowInsets.Type.displayCutout())
                v.setPadding(i.left, i.top, i.right, i.bottom)
            } else {
                @Suppress("DEPRECATION")
                v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        if (view.background == null) view.setBackgroundColor(Theme.PANEL)
        super.setContentView(view)
        view.requestApplyInsets()
    }
}
