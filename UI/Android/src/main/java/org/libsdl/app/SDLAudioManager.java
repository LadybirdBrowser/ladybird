package org.libsdl.app;

final class SDLAudioManager {
    static native void nativeSetupJNI();
    static native void nativeAddAudioDevice(boolean recording, String name, int deviceId);
    static native void nativeRemoveAudioDevice(boolean recording, int deviceId);

    private SDLAudioManager() { }
}
