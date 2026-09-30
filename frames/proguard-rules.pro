# Methods called from JavaScript through addJavascriptInterface must keep their names.
-keepclassmembers class com.coverstudio.frames.MainActivity$Bridge {
    @android.webkit.JavascriptInterface <methods>;
}
-keepattributes JavascriptInterface
