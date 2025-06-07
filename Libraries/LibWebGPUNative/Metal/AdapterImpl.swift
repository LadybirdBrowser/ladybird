/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
@_exported import CoreCxx
import Metal

public class AdapterImpl {
    private var device: MTLDevice?

    public init() {
    }

    public func initialize() -> Bool {
        guard let device = MTLCreateSystemDefaultDevice() else {
            return false
        }
        self.device = device
        return true
    }

    public func getMetalDevice() -> UnsafeMutableRawPointer? {
        guard let device = device else { return nil }
        return Unmanaged.passUnretained(device).toOpaque()
    }
}
