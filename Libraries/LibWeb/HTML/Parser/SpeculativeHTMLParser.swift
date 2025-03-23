/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import Collections
import Foundation
import GC
@_exported import WebCxx

// Workaround for https://github.com/swiftlang/swift/issues/80231
// If any line of this changes, the whole thing breaks though
extension GC.Cell.Visitor {
    public func visit(_ parser: Web.HTML.HTMLParserGCPtr) {
        if let parser = parser.ptr() {
            let cell: GC.Cell = cxxCast(parser)
            visit(cell)
        }
    }
}

struct SpeculativeMockElement {
    let name: Swift.String
    let localName: Swift.String
    let attributes: [HTMLToken.Attribute]
    var children: [SpeculativeMockElement]

    init(name: Swift.String, localName: Swift.String, attributes: [HTMLToken.Attribute]) {
        self.name = name
        self.localName = localName
        self.attributes = attributes
        self.children = []
    }

    mutating func appendChild(_ child: consuming SpeculativeMockElement) {
        children.append(child)
    }
}

public final class SpeculativeHTMLParser: HeapAllocatable {
    var parser = Web.HTML.HTMLParserGCPtr()  // FIXME: Want HTMLParserGCRef here, but how to initialize it?

    public init(cell: GC.Cell) {
        self.cell = cell
    }
    public var cell: GC.Cell

    public static func create(on heap: GC.Heap, `for` parser: Web.HTML.HTMLParserGCPtr) -> GC.Cell {
        precondition(heap.is_gc_deferred())
        let _self = allocate(on: heap)
        _self.pointee.parser = parser
        return _self.pointee.cell
    }

    public func visitEdges(_ visitor: GC.Cell.Visitor) {
        visitor.visit(parser)
    }
}
