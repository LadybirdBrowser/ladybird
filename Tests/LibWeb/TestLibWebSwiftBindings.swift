/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Testing
import Web

@Suite
struct TestLibWebSwiftBindings {

    @Test func enumsAreBound() {
        #expect(Web.DOM.NodeType.ELEMENT_NODE.rawValue == 1)

        #expect(Web.Bindings.NavigationType.Push.rawValue == 0)

        let end = Web.Bindings.idl_enum_to_string(Web.Bindings.ScrollLogicalPosition.End)
        let end_view = end.__bytes_as_string_viewUnsafe().bytes()
        let end_string = Swift.String(bytes: end_view, encoding: .utf8)!

        #expect(end_string == "end")
    }
}
