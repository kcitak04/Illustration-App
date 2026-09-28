package com.coverstudio.app.photo

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffXfermode
import com.coverstudio.app.vector.blendModeOf

private var layerIds = System.currentTimeMillis()

class RasterLayer(var name: String, var bitmap: Bitmap) {
    var id: Long = ++layerIds
    var visible = true
    var locked = false
    var opacity = 1f
    var blend = 0
    /** ALPHA_8 layer mask: 255 = visible. */
    var mask: Bitmap? = null
    var maskEnabled = true
    /** Smart layers are re-rendered from the vector illustration (like a Photoshop smart object). */
    var smart = false
    var smartBg = false
    val smartMatrix = Matrix()
    /** Incremented whenever pixels change, used for save bookkeeping and thumbnails. */
    var version = 0

    fun state() = LayerState(this, bitmap, mask, name, visible, locked, opacity, blend, maskEnabled, smart, Matrix(smartMatrix))
}

class LayerState(
    val layer: RasterLayer, val bitmap: Bitmap, val mask: Bitmap?, val name: String, val visible: Boolean,
    val locked: Boolean, val opacity: Float, val blend: Int, val maskEnabled: Boolean, val smart: Boolean, val smartMatrix: Matrix
) {
    fun restore() {
        val l = layer
        if (l.bitmap !== bitmap || l.mask !== mask) l.version++
        l.bitmap = bitmap; l.mask = mask; l.name = name; l.visible = visible; l.locked = locked
        l.opacity = opacity; l.blend = blend; l.maskEnabled = maskEnabled; l.smart = smart; l.smartMatrix.set(smartMatrix)
    }
}

class DocState(val layers: List<LayerState>, val active: Int, val bytes: Long)

class RasterDoc(val width: Int, val height: Int) {
    val layers = ArrayList<RasterLayer>()
    var active = 0
    /** ALPHA_8 selection mask, null = everything selected. */
    var selection: Bitmap? = null
    /** Outline of the selection when it came from a path (for marching ants). */
    var selectionPath: android.graphics.Path? = null

    val activeLayer: RasterLayer? get() = layers.getOrNull(active)

    fun newBitmap(): Bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)

    fun newMask(fill: Int = 255): Bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ALPHA_8).also {
        it.eraseColor(Color.argb(fill, 0, 0, 0))
    }

    fun addLayer(name: String, bmp: Bitmap = newBitmap(), at: Int = active + 1): RasterLayer {
        val l = RasterLayer(name, bmp)
        val idx = at.coerceIn(0, layers.size)
        layers.add(idx, l)
        active = idx
        return l
    }

    fun uniqueName(base: String): String {
        var i = layers.size + 1
        while (layers.any { it.name == "$base $i" }) i++
        return "$base $i"
    }

    private val dstIn = Paint(Paint.FILTER_BITMAP_FLAG).apply { xfermode = PorterDuffXfermode(PorterDuff.Mode.DST_IN) }

    fun layerPaint(l: RasterLayer) = Paint(Paint.FILTER_BITMAP_FLAG).apply {
        alpha = (l.opacity * 255).toInt()
        blendMode = blendModeOf(l.blend)
    }

    /** Flattens all visible layers (software rendering, for export / sampling). */
    fun flatten(scale: Float = 1f, config: Bitmap.Config = Bitmap.Config.ARGB_8888): Bitmap {
        val out = Bitmap.createBitmap(Math.max(1, (width * scale).toInt()), Math.max(1, (height * scale).toInt()), config)
        val c = Canvas(out)
        c.scale(scale, scale)
        drawLayers(c, layers)
        return out
    }

    fun drawLayers(c: Canvas, list: List<RasterLayer>) {
        for (l in list) {
            if (!l.visible) continue
            val p = layerPaint(l)
            val m = l.mask
            if (m != null && l.maskEnabled) {
                c.saveLayer(0f, 0f, width.toFloat(), height.toFloat(), p)
                c.drawBitmap(l.bitmap, 0f, 0f, null)
                c.drawBitmap(m, 0f, 0f, dstIn)
                c.restore()
            } else c.drawBitmap(l.bitmap, 0f, 0f, p)
        }
    }

    // ---------------------------------------------------------------- undo helpers

    fun state(extraBytes: Long): DocState = DocState(layers.map { it.state() }, active, extraBytes)

    fun restore(s: DocState) {
        layers.clear()
        for (ls in s.layers) { ls.restore(); layers.add(ls.layer) }
        active = s.active.coerceIn(0, layers.size - 1)
    }

    /** Returns a byte array of the selection mask (or null when nothing is selected). */
    fun selectionBytes(): ByteArray? {
        val s = selection ?: return null
        val buf = java.nio.ByteBuffer.allocate(s.rowBytes * s.height)
        s.copyPixelsToBuffer(buf)
        if (s.rowBytes == s.width) return buf.array()
        val out = ByteArray(s.width * s.height)
        val a = buf.array()
        for (y in 0 until s.height) System.arraycopy(a, y * s.rowBytes, out, y * s.width, s.width)
        return out
    }

    fun setSelectionBytes(bytes: ByteArray?) {
        if (bytes == null) { selection = null; selectionPath = null; return }
        val bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ALPHA_8)
        if (bmp.rowBytes == width) bmp.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(bytes))
        else {
            val padded = ByteArray(bmp.rowBytes * height)
            for (y in 0 until height) System.arraycopy(bytes, y * width, padded, y * bmp.rowBytes, width)
            bmp.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(padded))
        }
        selection = bmp
    }
}
