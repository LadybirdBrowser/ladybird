/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

@_exported import AKCxx
import Foundation

extension Swift.String {
    public init?(akString: AK.String) {
        let bytes = akString.__bytes_as_string_viewUnsafe().bytes()
        let data = Foundation.Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: bytes.data()), count: bytes.size(), deallocator: .none)

        self.init(data: data, encoding: .utf8)
    }

    public init?(akStringView: AK.StringView) {
        let bytes = akStringView.bytes()
        let data = Foundation.Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: bytes.data()), count: bytes.size(), deallocator: .none)

        self.init(data: data, encoding: .utf8)
    }
}

extension AK.StringView: ExpressibleByStringLiteral {
    public typealias StringLiteralType = Swift.StaticString

    public init(stringLiteral value: StringLiteralType) {
        self.init(value.utf8Start, value.utf8CodeUnitCount)
    }

    public func endsWith(_ suffix: AK.StringView) -> Bool {
        if suffix.length() == 1 {
            return self.ends_with(suffix[0])
        }
        return self.ends_with(suffix, AK.CaseSensitivity.sensitive)
    }
}
