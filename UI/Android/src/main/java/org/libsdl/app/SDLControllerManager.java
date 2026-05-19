package org.libsdl.app;

final class SDLControllerManager {
    static native void nativeSetupJNI();
    static native boolean onNativePadDown(int deviceId, int keycode, int scancode);
    static native boolean onNativePadUp(int deviceId, int keycode, int scancode);
    static native void onNativeJoy(int deviceId, int axis, float value);
    static native void onNativeHat(int deviceId, int hatId, int x, int y);
    static native void onNativeJoySensor(int deviceId, int sensorType, long sensorTimestamp, float x, float y, float z);
    static native void nativeAddJoystick(int deviceId, String deviceName, String deviceDescription, int vendorId, int productId, int buttonMask, int axisCount, int axisMask, int hatCount, boolean canRumble, boolean hasRgbLed, boolean hasAccelerometer, boolean hasGyroscope);
    static native void nativeRemoveJoystick(int deviceId);
    static native void nativeAddHaptic(int deviceId, String deviceName);
    static native void nativeRemoveHaptic(int deviceId);

    private SDLControllerManager() { }
}
