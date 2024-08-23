/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Foundation

public extension Foundation.Data {
    init(_ string: AK.StringView) {
        let bytes = string.bytes()
        self.init(bytesNoCopy: UnsafeMutableRawPointer(mutating: bytes.data()), count: bytes.size(), deallocator: .none)
    }
}

public extension Swift.String {
    init?(_ string: AK.String) {
        self.init(data: Foundation.Data(string.__bytes_as_string_viewUnsafe()), encoding: .utf8)
    }

    init?(_ string: AK.StringView) {
        self.init(data: Foundation.Data(string), encoding: .utf8)
    }
}
