/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
@_exported import CoreCxx
import Metal

public struct DeviceError: Swift.Error {
    public let message: Swift.String

    public init(_ message: Swift.String) {
        self.message = message
    }
}

public class DeviceImpl {
    private var device: MTLDevice
    private var commandQueue: MTLCommandQueue?

    public init(metalDevice: UnsafeMutableRawPointer) {
        self.device = Unmanaged<MTLDevice>.fromOpaque(metalDevice).takeUnretainedValue()
    }

    public func initialize() -> DeviceError? {
        defer {
            if let commandQueue = self.commandQueue {
                print("Initialized command queue: \(commandQueue)")
            }
        }

        guard let commandQueue = device.makeCommandQueue() else {
            return DeviceError("Unable to create command queue")
        }

        self.commandQueue = commandQueue
        return nil
    }

    public func getCommandQueue() -> UnsafeMutableRawPointer? {
        guard let commandQueue = commandQueue else { return nil }
        return Unmanaged.passUnretained(commandQueue).toOpaque()
    }
}
