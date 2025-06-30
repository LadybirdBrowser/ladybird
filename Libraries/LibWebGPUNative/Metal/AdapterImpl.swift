/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
@_exported import CoreCxx
import Metal

public struct AdapterError: Swift.Error {
    public let message: Swift.String

    public init(_ message: Swift.String) {
        self.message = message
    }
}

public class AdapterImpl {
    private var device: MTLDevice?

    public init() {
    }

    public func initialize() -> AdapterError? {
        defer {
            if let device = self.device {
                print("Selected device: \(device.name)")
            }
        }

        let devices = MTLCopyAllDevices()

        // FIXME: Expose and acknowledge options for guiding adapter selection
        //  https://www.w3.org/TR/webgpu/#adapter-selection
        for device in devices {
            if !device.isLowPower {
                self.device = device
                return nil
            }
        }

        for device in devices {
            if device.isLowPower {
                self.device = device
                return nil
            }
        }

        guard let defaultDevice = MTLCreateSystemDefaultDevice() else {
            return AdapterError("No devices found")
        }

        self.device = defaultDevice
        return nil
    }

    public func getMetalDevice() -> UnsafeMutableRawPointer? {
        guard let device = device else { return nil }
        return Unmanaged.passUnretained(device).toOpaque()
    }
}
