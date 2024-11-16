/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import AK
@_exported import GCCxx

extension GC.Heap {
    public func withDeferredGC<R, E>(_ body: () throws(E) -> R) throws(E) -> R {
        let deferredRAII = GC.DeferGC(self)
        _ = deferredRAII
        return try body()
    }
}

// FIXME: Cell and Cell::Visitor are not imported properly, so we have to treat them as OpaquePointer
public protocol HeapAllocatable {
    static func allocate(on heap: GC.Heap) -> UnsafeMutablePointer<Self>

    init(cell: OpaquePointer)

    func finalize()
    func visitEdges(_ visitor: OpaquePointer)

    var cell: OpaquePointer { get }
}

// FIXME: Figure out why other modules can't conform to HeapAllocatable
public struct HeapString: HeapAllocatable {
    public var string: Swift.String

    public init(cell: OpaquePointer) {
        self.cell = cell
        self.string = ""
    }

    // FIXME: HeapAllocatable cannot be exposed to C++ yet, so we're off to void* paradise
    public static func create(on heap: GC.Heap, string: Swift.String) -> OpaquePointer {
        // NOTE: GC must be deferred so that a collection during allocation doesn't get tripped
        //   up looking for the Cell pointer on the stack or in a register when it might only exist in the heap
        precondition(heap.is_gc_deferred())
        let heapString = allocate(on: heap)
        heapString.pointee.string = string
        return heapString.pointee.cell
    }

    public var cell: OpaquePointer
}

// Here be dragons

func asTypeMetadataPointer(_ type: Any.Type) -> UnsafeMutableRawPointer {
    unsafeBitCast(type, to: UnsafeMutableRawPointer.self)
}

func asHeapAllocatableType(_ typeMetadata: UnsafeMutableRawPointer) -> any HeapAllocatable.Type {
    let typeObject = unsafeBitCast(typeMetadata, to: Any.Type.self)
    guard let type = typeObject as? any HeapAllocatable.Type else {
        fatalError("Passed foreign class but it wasn't a Swift type!")
    }
    return type
}

extension HeapAllocatable {
    fileprivate static func initializeFromFFI(at this: UnsafeMutableRawPointer, cell: OpaquePointer) {
        this.assumingMemoryBound(to: Self.self).initialize(to: Self.self.init(cell: cell))
    }

    fileprivate static func destroyFromFFI(at this: UnsafeMutableRawPointer) {
        this.assumingMemoryBound(to: Self.self).deinitialize(count: 1)
    }

    fileprivate static func finalizeFromFFI(at this: UnsafeMutableRawPointer) {
        this.assumingMemoryBound(to: Self.self).pointee.finalize()
    }

    fileprivate static func visitEdgesFromFFI(at this: UnsafeMutableRawPointer, visitor: OpaquePointer) {
        this.assumingMemoryBound(to: Self.self).pointee.visitEdges(visitor)
    }

    public static func allocate(on heap: GC.Heap) -> UnsafeMutablePointer<Self> {
        let vtable = GC.ForeignCell.Vtable(
            class_metadata_pointer: asTypeMetadataPointer(Self.self),
            class_name: AK.String(swiftString: Swift.String(describing: Self.self)),
            alignment: MemoryLayout<Self>.alignment,
            initialize: { this, typeMetadata, cell in
                asHeapAllocatableType(typeMetadata!).initializeFromFFI(at: this!, cell: cell.ptr())
            },
            destroy: { this, typeMetadata in
                asHeapAllocatableType(typeMetadata!).destroyFromFFI(at: this!)
            },
            finalize: { this, typeMetadata in
                asHeapAllocatableType(typeMetadata!).finalizeFromFFI(at: this!)
            },
            visit_edges: nil
        )
        let cell = GC.ForeignCell.create(heap, MemoryLayout<Self>.stride, vtable)
        return cell.pointee.foreign_data().assumingMemoryBound(to: Self.self)
    }

    public func finalize() {}
    public func visitEdges(_ visitor: OpaquePointer) {}
}
