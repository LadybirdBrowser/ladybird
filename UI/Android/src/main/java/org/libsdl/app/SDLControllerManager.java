package org.libsdl.app;

final class SDLControllerManager {
    public static native int nativeSetupJNI();
    public static native boolean onNativePadDown(int deviceId, int keycode);
    public static native boolean onNativePadUp(int deviceId, int keycode);
    public static native void onNativeJoy(int deviceId, int axis, float value);
    public static native void onNativeHat(int deviceId, int hatId, int x, int y);
    public static native void nativeAddJoystick(int deviceId, String name, String description, int vendorId, int productId, int buttonMask, int axisCount, int axisMask, int hatCount, boolean canRumble);
    public static native void nativeRemoveJoystick(int deviceId);
    public static native void nativeAddHaptic(int deviceId, String name);
    public static native void nativeRemoveHaptic(int deviceId);

    private SDLControllerManager() { }
}
