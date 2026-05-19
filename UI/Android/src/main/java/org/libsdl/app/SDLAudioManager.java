package org.libsdl.app;

final class SDLAudioManager {
    public static native int nativeSetupJNI();
    public static native void removeAudioDevice(boolean recording, int deviceId);
    public static native void addAudioDevice(boolean recording, String name, int deviceId);

    private SDLAudioManager() { }
}
