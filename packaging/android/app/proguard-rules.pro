# Called by name from the native code (JNI), so the shrinker must leave them whole.
-keep class org.libsdl.app.** { *; }
-keep class tv.stremioorigin.app.** { *; }
# Names stay readable in crash reports from logcat.
-dontobfuscate
