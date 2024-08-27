/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Web
import Foundation

class StandardError: TextOutputStream {
    func write(_ string: Swift.String) {
        try! FileHandle.standardError.write(contentsOf: Data(string.utf8))
    }
}

@main
struct TestLibWebSwiftBindings {

    static func testEnumsAreBound() {
        var standardError = StandardError()
        print("Testing LibWeb enum types...", to: &standardError)

        print("Web.DOM.NodeType.ELEMENT_NODE == \(Web.DOM.NodeType.ELEMENT_NODE)", to: &standardError)
        precondition(Web.DOM.NodeType.ELEMENT_NODE.rawValue == 1)

        print("Web.Bindings.NavigationType.Push == \(Web.Bindings.NavigationType.Push)", to: &standardError)
        precondition(Web.Bindings.NavigationType.Push.rawValue == 0)

        let end = Web.Bindings.idl_enum_to_string(Web.Bindings.ScrollLogicalPosition.End)
        let end_view = end.__bytes_as_string_viewUnsafe().bytes();
        let end_string = Swift.String(bytes: end_view, encoding: .utf8)!

        print("Web.Bindings.idl_enum_to_string(Web.Bindings.ScrollLogicalPosition.End) == \(end_string)", to: &standardError)
        precondition(end_string == "end")

        print("LibWeb enum types pass", to: &standardError)
    }

    static func main() {
        var standardError = StandardError()
        print("Starting test suite...", to: &standardError)
        testEnumsAreBound()

        print("All tests pass", to: &standardError)
    }
}
