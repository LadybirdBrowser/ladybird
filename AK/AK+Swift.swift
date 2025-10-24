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
        let data = Data(bytes: bytes.data(), count: bytes.size())
        self.init(data: data, encoding: .utf8)
    }

    public init?(akStringView: AK.StringView) {
        let bytes = akStringView.bytes()
        let data = Data(bytes: bytes.data(), count: bytes.size())
        self.init(data: data, encoding: .utf8)
    }
}

extension AK.String {
    public init(swiftString: consuming Swift.String) {
        self.init()
        swiftString.withUTF8 { buffer in
            if let base = buffer.baseAddress, buffer.count > 0 {
                self = AK.String.from_utf8_without_validation(AK.ReadonlyBytes(base, buffer.count))
            } else {
                self = AK.String()
            }
        }
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
