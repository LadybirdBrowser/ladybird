/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import GC
@_exported import TestGCSwiftCxx
import Testing

public struct HeapString: HeapAllocatable {
    public var string: Swift.String

    public init(cell: GC.Cell) {
        self.cell = cell
        self.string = ""
    }

    public static func create(on heap: GC.Heap, string: Swift.String) -> GC.Cell {
        // NOTE: GC must be deferred so that a collection during allocation doesn't get tripped
        //   up looking for the Cell pointer on the stack or in a register when it might only exist in the heap
        precondition(heap.is_gc_deferred())
        let heapString = allocate(on: heap)
        heapString.pointee.string = string
        return heapString.pointee.cell
    }

    public var cell: GC.Cell
}

@Suite(.serialized)
struct TestGCSwiftBindings {

    @Test func createBoundString() {
        let heap = test_gc_heap()
        let string = heap.withDeferredGC {
            return HeapString.allocate(on: heap)
        }
        #expect(string.pointee.string == "")
        heap.collect_garbage(GC.Heap.CollectionType.CollectGarbage)

        string.pointee.string = "Hello, World!"
        heap.collect_garbage(GC.Heap.CollectionType.CollectGarbage)
        #expect(string.pointee.string == "Hello, World!")

        heap.collect_garbage(GC.Heap.CollectionType.CollectEverything)
    }

    @Test func testInterop() {
        test_interop()
    }
}
