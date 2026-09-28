package com.coverstudio.app.project

import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Matrix
import android.graphics.Paint
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import com.coverstudio.app.photo.RasterDoc
import com.coverstudio.app.photo.RasterLayer
import com.coverstudio.app.vector.VectorDoc
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

class ProjectInfo(val dir: File, val name: String, val width: Int, val height: Int, val modified: Long) {
    val thumb: File get() = File(dir, "thumb.jpg")
}

object ProjectStore {
    fun root(ctx: Context) = File(ctx.filesDir, "projects").also { it.mkdirs() }
    fun fontsDir(ctx: Context) = File(ctx.filesDir, "fonts").also { it.mkdirs() }

    fun list(ctx: Context): List<ProjectInfo> =
        root(ctx).listFiles()?.filter { it.isDirectory }?.mapNotNull { info(it) }?.sortedByDescending { it.modified } ?: emptyList()

    fun info(dir: File): ProjectInfo? = try {
        val o = JSONObject(File(dir, "project.json").readText())
        ProjectInfo(dir, o.optString("name", "Untitled"), o.optInt("w", 3000), o.optInt("h", 3000), o.optLong("modified"))
    } catch (e: Exception) { null }

    fun create(ctx: Context, name: String, w: Int, h: Int): ProjectInfo {
        val dir = File(root(ctx), "p" + System.currentTimeMillis())
        dir.mkdirs()
        writeMeta(dir, name, w, h)
        val vd = VectorDoc(w, h)
        File(dir, "vector.json").writeText(vd.toJson().toString())
        return info(dir)!!
    }

    fun writeMeta(dir: File, name: String, w: Int, h: Int) {
        val o = JSONObject().put("name", name).put("w", w).put("h", h).put("modified", System.currentTimeMillis())
        atomicWrite(File(dir, "project.json")) { it.write(o.toString().toByteArray()) }
    }

    fun rename(p: ProjectInfo, name: String) = writeMeta(p.dir, name, p.width, p.height)

    fun delete(p: ProjectInfo) { p.dir.deleteRecursively() }

    fun duplicate(ctx: Context, p: ProjectInfo) {
        val dir = File(root(ctx), "p" + System.currentTimeMillis())
        p.dir.copyRecursively(dir)
        writeMeta(dir, p.name + " copy", p.width, p.height)
    }

    private fun atomicWrite(f: File, block: (FileOutputStream) -> Unit) {
        val tmp = File(f.parentFile, f.name + ".tmp")
        FileOutputStream(tmp).use { block(it); it.fd.sync() }
        if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
    }

    // ------------------------------------------------------------------ vector

    fun loadVector(p: ProjectInfo): VectorDoc = try {
        VectorDoc.fromJson(JSONObject(File(p.dir, "vector.json").readText()))
    } catch (e: Exception) { VectorDoc(p.width, p.height) }

    fun saveVector(dir: File, json: String) = atomicWrite(File(dir, "vector.json")) { it.write(json.toByteArray()) }

    // ------------------------------------------------------------------ raster

    fun loadRaster(p: ProjectInfo): RasterDoc {
        val d = RasterDoc(p.width, p.height)
        val f = File(p.dir, "photo.json")
        if (f.exists()) try {
            val o = JSONObject(f.readText())
            val arr = o.optJSONArray("layers") ?: JSONArray()
            val opts = BitmapFactory.Options().apply { inMutable = true; inPreferredConfig = Bitmap.Config.ARGB_8888 }
            for (i in 0 until arr.length()) {
                val lo = arr.getJSONObject(i)
                val img = File(p.dir, lo.optString("file"))
                val bmp = BitmapFactory.decodeFile(img.path, opts)?.let { fitSize(it, d) } ?: d.newBitmap()
                val l = RasterLayer(lo.optString("name", "Layer"), bmp)
                if (lo.has("id")) l.id = lo.optLong("id")
                l.visible = lo.optBoolean("visible", true); l.locked = lo.optBoolean("locked")
                l.opacity = lo.optDouble("opacity", 1.0).toFloat(); l.blend = lo.optInt("blend")
                l.maskEnabled = lo.optBoolean("maskEnabled", true)
                l.smart = lo.optBoolean("smart"); l.smartBg = lo.optBoolean("smartBg")
                lo.optJSONArray("smartMatrix")?.let { a -> l.smartMatrix.setValues(FloatArray(9) { a.optDouble(it).toFloat() }) }
                val mf = lo.optString("mask", "")
                if (mf.isNotEmpty()) BitmapFactory.decodeFile(File(p.dir, mf).path)?.let { l.mask = fitSize(it, d).extractAlpha() }
                d.layers.add(l)
            }
            d.active = o.optInt("active").coerceIn(0, maxOf(0, d.layers.size - 1))
        } catch (e: Exception) { d.layers.clear() }
        if (d.layers.isEmpty()) {
            val bg = d.newBitmap(); bg.eraseColor(android.graphics.Color.WHITE)
            d.addLayer("Background", bg, 0)
        }
        return d
    }

    private fun fitSize(b: Bitmap, d: RasterDoc): Bitmap {
        if (b.width == d.width && b.height == d.height) return b
        val out = d.newBitmap()
        Canvas(out).drawBitmap(b, null, android.graphics.Rect(0, 0, d.width, d.height), Paint(Paint.FILTER_BITMAP_FLAG))
        return out
    }

    /** Snapshot of what needs saving, safe to hand to a background thread. */
    class RasterSnapshot(val json: String, val files: List<Pair<String, Bitmap>>, val masks: List<Pair<String, Bitmap>>, val keep: Set<String>)

    private val savedVersions = HashMap<String, Int>()

    fun snapshotRaster(dir: File, d: RasterDoc): RasterSnapshot {
        val arr = JSONArray()
        val files = ArrayList<Pair<String, Bitmap>>()
        val masks = ArrayList<Pair<String, Bitmap>>()
        val keep = HashSet<String>()
        for (l in d.layers) {
            val file = "layer_${l.id}.png"
            val maskFile = if (l.mask != null) "mask_${l.id}.png" else ""
            keep.add(file); if (maskFile.isNotEmpty()) keep.add(maskFile)
            val key = dir.path + "/" + l.id
            if (savedVersions[key] != l.version || !File(dir, file).exists()) {
                // smart layers are regenerated from the vector file but we still save pixels for thumbnails / safety
                files.add(file to l.bitmap)
                l.mask?.let { masks.add(maskFile to it) }
                savedVersions[key] = l.version
            }
            val m = FloatArray(9); l.smartMatrix.getValues(m)
            arr.put(JSONObject().put("id", l.id).put("name", l.name).put("file", file).put("mask", maskFile).put("visible", l.visible)
                .put("locked", l.locked).put("opacity", l.opacity.toDouble()).put("blend", l.blend).put("maskEnabled", l.maskEnabled)
                .put("smart", l.smart).put("smartBg", l.smartBg).put("smartMatrix", JSONArray().also { a -> m.forEach { a.put(it.toDouble()) } }))
        }
        val json = JSONObject().put("layers", arr).put("active", d.active).toString()
        return RasterSnapshot(json, files, masks, keep)
    }

    fun writeRaster(dir: File, s: RasterSnapshot) {
        for ((name, bmp) in s.files) atomicWrite(File(dir, name)) { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
        for ((name, m) in s.masks) {
            val argb = Bitmap.createBitmap(m.width, m.height, Bitmap.Config.ARGB_8888)
            Canvas(argb).drawBitmap(m, 0f, 0f, Paint())
            atomicWrite(File(dir, name)) { argb.compress(Bitmap.CompressFormat.PNG, 100, it) }
        }
        atomicWrite(File(dir, "photo.json")) { it.write(s.json.toByteArray()) }
        dir.listFiles()?.forEach { f ->
            if ((f.name.startsWith("layer_") || f.name.startsWith("mask_")) && f.name.endsWith(".png") && f.name !in s.keep) f.delete()
        }
    }

    fun forgetVersions() = savedVersions.clear()

    /** Freshly loaded layers are already on disk, so don't rewrite them on the first save. */
    fun markSaved(dir: File, d: RasterDoc) {
        for (l in d.layers) if (File(dir, "layer_${l.id}.png").exists()) savedVersions[dir.path + "/" + l.id] = l.version
    }

    fun writeThumb(dir: File, bmp: Bitmap) {
        val s = 320f / maxOf(bmp.width, bmp.height)
        val m = Matrix(); m.setScale(s, s)
        val t = Bitmap.createBitmap(bmp, 0, 0, bmp.width, bmp.height, m, true)
        atomicWrite(File(dir, "thumb.jpg")) { t.compress(Bitmap.CompressFormat.JPEG, 85, it) }
    }

    // ------------------------------------------------------------------ export

    /** Saves an image to Pictures/CoverStudio and returns its content Uri. */
    fun exportImage(ctx: Context, bmp: Bitmap, baseName: String, jpeg: Boolean): Uri? {
        val name = "${baseName.replace(Regex("[^A-Za-z0-9 _-]"), "_")}_${System.currentTimeMillis() / 1000}.${if (jpeg) "jpg" else "png"}"
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, name)
            put(MediaStore.Images.Media.MIME_TYPE, if (jpeg) "image/jpeg" else "image/png")
            put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/CoverStudio")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
        val resolver = ctx.contentResolver
        val uri = resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values) ?: return null
        resolver.openOutputStream(uri)?.use { out ->
            val toWrite = if (jpeg && bmp.hasAlpha()) flattenOnWhite(bmp) else bmp
            toWrite.compress(if (jpeg) Bitmap.CompressFormat.JPEG else Bitmap.CompressFormat.PNG, if (jpeg) 95 else 100, out)
        }
        values.clear(); values.put(MediaStore.Images.Media.IS_PENDING, 0)
        resolver.update(uri, values, null, null)
        return uri
    }

    fun exportText(ctx: Context, text: String, baseName: String, ext: String, mime: String): Uri? {
        val name = "${baseName.replace(Regex("[^A-Za-z0-9 _-]"), "_")}_${System.currentTimeMillis() / 1000}.$ext"
        val values = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, name)
            put(MediaStore.Downloads.MIME_TYPE, mime)
            put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/CoverStudio")
        }
        val uri = ctx.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values) ?: return null
        ctx.contentResolver.openOutputStream(uri)?.use { it.write(text.toByteArray()) }
        return uri
    }

    private fun flattenOnWhite(b: Bitmap): Bitmap {
        val out = Bitmap.createBitmap(b.width, b.height, Bitmap.Config.ARGB_8888)
        val c = Canvas(out); c.drawColor(android.graphics.Color.WHITE); c.drawBitmap(b, 0f, 0f, null)
        return out
    }

    fun share(ctx: Context, uri: Uri, mime: String) {
        val i = Intent(Intent.ACTION_SEND).apply {
            type = mime
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        ctx.startActivity(Intent.createChooser(i, "Share"))
    }
}
