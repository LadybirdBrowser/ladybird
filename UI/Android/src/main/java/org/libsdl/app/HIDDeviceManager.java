package org.libsdl.app;

public final class HIDDeviceManager {
    private native void HIDDeviceRegisterCallback();
    private native void HIDDeviceReleaseCallback();
    native void HIDDeviceConnected(int deviceId, String identifier, int vendorId, int productId, String serialNumber, int releaseNumber, String manufacturerString, String productString, int interfaceNumber, int interfaceClass, int interfaceSubclass, int interfaceProtocol, boolean bluetooth);
    native void HIDDeviceOpenPending(int deviceId);
    native void HIDDeviceOpenResult(int deviceId, boolean opened);
    native void HIDDeviceDisconnected(int deviceId);
    native void HIDDeviceInputReport(int deviceId, byte[] report);
    native void HIDDeviceReportResponse(int deviceId, byte[] report);
}
