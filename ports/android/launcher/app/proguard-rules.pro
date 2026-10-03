# GameActivity's callbacks are resolved from native code with GetMethodID, so
# R8 cannot see that they are used. Stripping or renaming any of them turns
# every native -> Kotlin call into a silent no-op (jmethodID comes back null)
# which is the worst possible failure mode: the game runs, but errors never
# surface and the clipboard never works.
-keepclassmembers class ovh.kisak.cod4.GameActivity {
    public void showFatalError(java.lang.String);
    public void requestQuit();
    public void openUrl(java.lang.String);
    public java.lang.String getClipboardText();
    public void setClipboardText(java.lang.String);
    public void showRestartPrompt(java.lang.String);
    public void setSoftKeyboardVisible(boolean);
    public void setPreferredFrameRate(float);
}

# The native methods themselves are bound by name at first call.
-keepclasseswithmembernames class ovh.kisak.cod4.EngineBridge {
    native <methods>;
}
-keep class ovh.kisak.cod4.EngineBridge { *; }
