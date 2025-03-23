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

public protocol HeapAllocatable {
    static func allocate(on heap: GC.Heap) -> UnsafeMutablePointer<Self>

    init(cell: GC.Cell)

    func finalize()
    func visitEdges(_ visitor: GC.Cell.Visitor)

    var cell: GC.Cell { get }
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
    fileprivate static func initializeFromFFI(at this: UnsafeMutableRawPointer, cell: GC.Cell) {
        this.assumingMemoryBound(to: Self.self).initialize(to: Self.self.init(cell: cell))
    }

    fileprivate static func destroyFromFFI(at this: UnsafeMutableRawPointer) {
        this.assumingMemoryBound(to: Self.self).deinitialize(count: 1)
    }

    fileprivate static func finalizeFromFFI(at this: UnsafeMutableRawPointer) {
        this.assumingMemoryBound(to: Self.self).pointee.finalize()
    }

    fileprivate static func visitEdgesFromFFI(at this: UnsafeMutableRawPointer, visitor: GC.Cell.Visitor) {
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
    public func visitEdges(_ visitor: GC.Cell.Visitor) {}
}
