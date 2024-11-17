/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import Foundation

typealias EntryPoint = @convention(thin) @Sendable (_ configurationJSON: UnsafeRawBufferPointer?, _ recordHandler: @escaping @Sendable (_ recordJSON: UnsafeRawBufferPointer) -> Void) async throws -> Bool

@_extern(c, "swt_abiv0_getEntryPoint")
func swt_abiv0_getEntryPoint() -> UnsafeRawPointer

@main struct Runner {
    static func main() async throws {
        let configurationJSON: UnsafeRawBufferPointer? = nil
        let recordHandler: @Sendable (UnsafeRawBufferPointer) -> Void = { _ in }

        let entryPoint = unsafeBitCast(swt_abiv0_getEntryPoint(), to: EntryPoint.self)

        if try await entryPoint(configurationJSON, recordHandler) {
            exit(EXIT_SUCCESS)
        } else {
            exit(EXIT_FAILURE)
        }
    }
}
