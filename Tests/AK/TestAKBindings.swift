/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Foundation

protocol ConformanceMarker {}
enum CxxSequenceMarker<T> {}
extension CxxSequenceMarker: ConformanceMarker where T: CxxSequence {}
private func isCxxSequenceType<T>(_ type: T.Type) -> Bool {
    return CxxSequenceMarker<T>.self is ConformanceMarker.Type
}

class StandardError: TextOutputStream {
    func write(_ string: Swift.String) {
        try! FileHandle.standardError.write(contentsOf: Data(string.utf8))
    }
}

@main
struct TestAKBindings {
    static func testSequenceTypesAreBound() {
        var standardError = StandardError()
        print("Testing CxxSequence types...", to: &standardError)

        precondition(isCxxSequenceType(AK.StringView.self))
        precondition(isCxxSequenceType(AK.Bytes.self))
        precondition(isCxxSequenceType(AK.ReadonlyBytes.self))
        precondition(isCxxSequenceType(AK.Utf16Data.self))

        precondition(!isCxxSequenceType(AK.Utf16View.self))
        precondition(!isCxxSequenceType(AK.String.self))

        print("CxxSequence types pass", to: &standardError)
    }

    static func main() {
        var standardError = StandardError()
        print("Starting test suite...", to: &standardError)
        testSequenceTypesAreBound()

        print("All tests pass", to: &standardError)
    }
}
