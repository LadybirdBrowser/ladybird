/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
import GC
import GCTesting
import Testing

// FIXME: We want a type declared *here* for HeapString, but it gives a compiler warning:
//  error: type 'GCString' cannot conform to protocol 'HeapAllocatable' because it has requirements that cannot be satisfied
//  Even using the same exact code from LibGC/Heap+Swift.swift
//  This is likely because one of the required types for HeapAllocatable is not fully imported from C++ and thus can't
//  be re-exported by the GC module.

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
