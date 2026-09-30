package com.coverstudio.frames

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.ContentValues
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.MediaStore
import android.util.Base64
import android.view.WindowInsets
import android.view.WindowManager
import android.webkit.JavascriptInterface
import android.webkit.MimeTypeMap
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
import android.widget.Toast
import java.io.IOException
import java.io.OutputStream
import java.util.concurrent.ConcurrentHashMap

/**
 * Frame Studio for Android: hosts the animation studio (bundled in assets/www) in a WebView,
 * with the phone's file picker for imports and MediaStore saving for exports.
 */
class MainActivity : Activity() {
    private lateinit var web: WebView
    private var fileCallback: ValueCallback<Array<Uri>>? = null

    private class PendingSave(val uri: Uri, val stream: OutputStream, val folder: String)
    private val saves = ConcurrentHashMap<String, PendingSave>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) WebView.setWebContentsDebuggingEnabled(true)

        web = WebView(this)
        web.setBackgroundColor(0xFF16171B.toInt())
        with(web.settings) {
            javaScriptEnabled = true
            domStorageEnabled = true
            @Suppress("DEPRECATION")
            databaseEnabled = true
            mediaPlaybackRequiresUserGesture = false
            allowFileAccess = false
            allowContentAccess = true
            setSupportZoom(false)
            builtInZoomControls = false
            displayZoomControls = false
            textZoom = 100
            mixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW
            cacheMode = WebSettings.LOAD_DEFAULT
        }
        web.webViewClient = AssetClient()
        web.webChromeClient = Chrome()
        web.addJavascriptInterface(Bridge(), "AndroidBridge")

        val root = FrameLayout(this)
        root.addView(web, FrameLayout.LayoutParams(-1, -1))
        root.setBackgroundColor(0xFF16171B.toInt())
        // keep the studio clear of the status / navigation bars and the keyboard (Android 15 is edge-to-edge)
        root.setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val i = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout() or WindowInsets.Type.ime())
                v.setPadding(i.left, i.top, i.right, i.bottom)
            } else {
                @Suppress("DEPRECATION")
                v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        setContentView(root)
        root.requestApplyInsets()

        if (savedInstanceState != null) web.restoreState(savedInstanceState)
        else web.loadUrl("https://$HOST/index.html")
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        web.saveState(outState)
    }

    override fun onPause() {
        web.evaluateJavascript("window.__androidPause && window.__androidPause()", null)
        web.onPause()
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        web.onResume()
    }

    override fun onDestroy() {
        saves.values.forEach { runCatching { it.stream.close() } }
        web.destroy()
        super.onDestroy()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        // let the page close its dialogs / panels first; leave the app only when nothing is open
        web.evaluateJavascript("window.__androidBack ? window.__androidBack() : false") { handled ->
            if (handled != "true") {
                @Suppress("DEPRECATION")
                super.onBackPressed()
            }
        }
    }

    // ------------------------------------------------------------------ serving the bundled app

    private inner class AssetClient : WebViewClient() {
        override fun shouldInterceptRequest(view: WebView, request: WebResourceRequest): WebResourceResponse? {
            val url = request.url
            if (url.host != HOST) return null
            var path = url.path?.trimStart('/') ?: ""
            if (path.isEmpty()) path = "index.html"
            return try {
                val stream = assets.open("www/$path")
                WebResourceResponse(mimeFor(path), "UTF-8", 200, "OK", mapOf("Access-Control-Allow-Origin" to "*", "Cache-Control" to "no-cache"), stream)
            } catch (e: IOException) {
                WebResourceResponse("text/plain", "UTF-8", 404, "Not Found", emptyMap(), "Not found".byteInputStream())
            }
        }

        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
            val url = request.url
            if (url.host == HOST) return false
            // links to other sites open in the browser
            return try { startActivity(Intent(Intent.ACTION_VIEW, url)); true } catch (e: ActivityNotFoundException) { true }
        }
    }

    private fun mimeFor(path: String): String = when (path.substringAfterLast('.', "").lowercase()) {
        "html" -> "text/html"
        "js", "mjs" -> "text/javascript"
        "css" -> "text/css"
        "json" -> "application/json"
        "svg" -> "image/svg+xml"
        "png" -> "image/png"
        "wasm" -> "application/wasm"
        else -> "application/octet-stream"
    }

    // ------------------------------------------------------------------ importing: the phone's file picker

    private inner class Chrome : WebChromeClient() {
        override fun onShowFileChooser(view: WebView, callback: ValueCallback<Array<Uri>>, params: FileChooserParams): Boolean {
            fileCallback?.onReceiveValue(null)
            fileCallback = callback
            val tokens = params.acceptTypes.flatMap { it.split(",") }.map { it.trim().lowercase() }.filter { it.isNotEmpty() }
            val mimes = tokens.mapNotNull { t ->
                if (t.contains("/")) t else MimeTypeMap.getSingleton().getMimeTypeFromExtension(t.removePrefix("."))
            }.distinct()
            val intent = Intent(Intent.ACTION_GET_CONTENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = if (mimes.size == 1) mimes[0] else "*/*"
                if (mimes.size > 1) putExtra(Intent.EXTRA_MIME_TYPES, mimes.toTypedArray())
                putExtra(Intent.EXTRA_ALLOW_MULTIPLE, params.mode == FileChooserParams.MODE_OPEN_MULTIPLE)
            }
            return try {
                @Suppress("DEPRECATION")
                startActivityForResult(Intent.createChooser(intent, "Import into Frame Studio"), REQ_FILES)
                true
            } catch (e: ActivityNotFoundException) {
                fileCallback = null
                Toast.makeText(this@MainActivity, "No app on this phone can pick files.", Toast.LENGTH_LONG).show()
                false
            }
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_FILES) return
        val cb = fileCallback ?: return
        fileCallback = null
        if (resultCode != RESULT_OK || data == null) { cb.onReceiveValue(null); return }
        val uris = ArrayList<Uri>()
        val clip = data.clipData
        if (clip != null) for (i in 0 until clip.itemCount) clip.getItemAt(i).uri?.let { uris.add(it) }
        else data.data?.let { uris.add(it) }
        cb.onReceiveValue(if (uris.isEmpty()) null else uris.toTypedArray())
    }

    // ------------------------------------------------------------------ exporting: JavaScript bridge

    /** Methods the page calls as window.AndroidBridge.*. They run on a background thread. */
    inner class Bridge {
        /** Starts a new file in Movies / Pictures / Download (FrameStudio folder); returns an id or "". */
        @JavascriptInterface
        fun begin(name: String, mime: String): String {
            return try {
                val safeName = name.replace(Regex("[\\\\/:*?\"<>|]"), "_").ifBlank { "export" }
                val (collection, dir) = when {
                    mime.startsWith("video/") -> MediaStore.Video.Media.EXTERNAL_CONTENT_URI to Environment.DIRECTORY_MOVIES
                    mime.startsWith("image/") && mime != "image/svg+xml" -> MediaStore.Images.Media.EXTERNAL_CONTENT_URI to Environment.DIRECTORY_PICTURES
                    else -> MediaStore.Downloads.EXTERNAL_CONTENT_URI to Environment.DIRECTORY_DOWNLOADS
                }
                val folder = "$dir/FrameStudio"
                val values = ContentValues().apply {
                    put(MediaStore.MediaColumns.DISPLAY_NAME, safeName)
                    put(MediaStore.MediaColumns.MIME_TYPE, mime)
                    put(MediaStore.MediaColumns.RELATIVE_PATH, folder)
                    put(MediaStore.MediaColumns.IS_PENDING, 1)
                }
                val uri = contentResolver.insert(collection, values) ?: return ""
                val stream = contentResolver.openOutputStream(uri) ?: return ""
                val id = System.nanoTime().toString()
                saves[id] = PendingSave(uri, stream, folder)
                id
            } catch (e: Exception) {
                ""
            }
        }

        @JavascriptInterface
        fun append(id: String, base64: String): Boolean {
            val s = saves[id] ?: return false
            return try { s.stream.write(Base64.decode(base64, Base64.DEFAULT)); true } catch (e: Exception) { false }
        }

        /** Finishes the file; returns "contentUri|Folder" or "" on failure. */
        @JavascriptInterface
        fun finish(id: String): String {
            val s = saves.remove(id) ?: return ""
            return try {
                s.stream.close()
                val values = ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }
                contentResolver.update(s.uri, values, null, null)
                "${s.uri}|${s.folder}"
            } catch (e: Exception) {
                runCatching { contentResolver.delete(s.uri, null, null) }
                ""
            }
        }

        @JavascriptInterface
        fun share(uri: String, mime: String) {
            runOnUiThread {
                val send = Intent(Intent.ACTION_SEND).apply {
                    type = mime
                    putExtra(Intent.EXTRA_STREAM, Uri.parse(uri))
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                try { startActivity(Intent.createChooser(send, "Share")) } catch (e: ActivityNotFoundException) {
                    Toast.makeText(this@MainActivity, "No app can share this file.", Toast.LENGTH_SHORT).show()
                }
            }
        }

        /** Keeps the screen on while a long export runs. */
        @JavascriptInterface
        fun keepAwake(on: Boolean) {
            runOnUiThread {
                if (on) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
        }

        @JavascriptInterface
        fun isAndroidApp(): Boolean = true
    }

    companion object {
        /** Reserved host that never reaches the network; every request to it is served from assets/www. */
        const val HOST = "appassets.androidplatform.net"
        private const val REQ_FILES = 41
    }
}
