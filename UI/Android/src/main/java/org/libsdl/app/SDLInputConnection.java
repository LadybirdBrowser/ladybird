package org.libsdl.app;

final class SDLInputConnection {
    public static native void nativeCommitText(String text, int newCursorPosition);
    public static native void nativeGenerateScancodeForUnichar(char character);

    private SDLInputConnection() { }
}
