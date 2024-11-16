/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Swift.h>
#include <AK/TypeCasts.h>
#include <LibGC/Cell.h>
#include <LibGC/DeferGC.h>

namespace GC {

template<typename T>
struct ForeignRef;

template<typename T>
struct ForeignPtr;

#define FOREIGN_CELL(class_, base_class) \
    using Base = base_class;             \
    friend class GC::Heap;

class ForeignCell : public Cell {
    FOREIGN_CELL(ForeignCell, Cell);

public:
    struct Vtable {
        // Holds a pointer to the foreign vtable information such as
        // a jclass in Java, or a Swift type metadata pointer
        void* class_metadata_pointer = nullptr;

        // FIXME: FlyString? The class name must be owned by the ForeignCell so it can vend StringViews
        //    We should properly cache the name and class info pointer to avoid string churn
        String class_name;

        size_t alignment { 1 };

        void (*initialize)(void* thiz, void* clazz, Ref<Cell>);
        void (*destroy)(void* thiz, void* clazz);
        void (*finalize)(void* thiz, void* clazz);
        void (*visit_edges)(void* thiz, void* clazz, Cell::Visitor&);
    };
    static Ref<ForeignCell> create(Heap&, size_t size, Vtable);

    void* foreign_data() SWIFT_RETURNS_INDEPENDENT_VALUE; // technically lying to swift, but it's fiiiiine

    // ^Cell
    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor& visitor) override;
    virtual StringView class_name() const override { return m_vtable.class_name; }

    ~ForeignCell();

private:
    ForeignCell(Vtable vtable);

    Vtable m_vtable;
} SWIFT_IMMORTAL_REFERENCE;

template<typename T>
struct ForeignRef {
    friend struct ForeignPtr<T>;

    template<typename... Args>
    static ForeignRef allocate(Heap& heap, Args... args)
    {
        DeferGC const defer_gc(heap);
        auto* cell = T::create(&heap, forward<Args>(args)...);
        if constexpr (IsSame<decltype(cell), Cell*>) {
            return ForeignRef(*verify_cast<ForeignCell>(cell));
        } else {
            static_assert(IsSame<decltype(cell), void*>);
            auto* cast_cell = static_cast<Cell*>(cell);
            return ForeignRef(*verify_cast<ForeignCell>(cast_cell));
        }
    }

    ForeignRef() = delete;

    // This constructor should only be called directly after allocating a foreign cell by calling an FFI create method
    ForeignRef(ForeignCell& cell)
        : m_cell(cell)
    {
        // FIXME: This is super dangerous. How can we assert that the cell is actually a T?
        m_data = static_cast<T*>(m_cell->foreign_data());
    }

    ~ForeignRef() = default;
    ForeignRef(ForeignRef const& other) = default;
    ForeignRef& operator=(ForeignRef const& other) = default;

    RETURNS_NONNULL T* operator->() const { return m_data; }
    [[nodiscard]] T& operator*() const { return *m_data; }

    RETURNS_NONNULL T* ptr() const { return m_data; }
    RETURNS_NONNULL operator T*() const { return m_data; }

    operator T&() const { return *m_data; }

    Ref<ForeignCell> cell() const { return m_cell; }

    void visit_edges(Cell::Visitor& visitor)
    {
        visitor.visit(m_cell);
    }

private:
    Ref<ForeignCell> m_cell;
    T* m_data { nullptr };
};

template<typename T>
struct ForeignPtr {
    constexpr ForeignPtr() = default;

    // This constructor should only be called directly after allocating a foreign cell by calling an FFI create method
    ForeignPtr(ForeignCell& cell)
        : m_cell(&cell)
    {
        // FIXME: This is super dangerous. How can we assert that the cell is actually a T?
        m_data = static_cast<T*>(m_cell->foreign_data());
    }

    // This constructor should only be called directly after allocating a foreign cell by calling an FFI create method
    ForeignPtr(ForeignCell* cell)
        : m_cell(cell)
    {
        // FIXME: This is super dangerous. How can we assert that the cell is actually a T?
        m_data = m_cell ? static_cast<T*>(m_cell->foreign_data()) : nullptr;
    }

    ForeignPtr(ForeignRef<T> const& other)
        : m_cell(other.m_cell)
        , m_data(other.m_data)
    {
    }

    ForeignPtr(nullptr_t)
        : m_cell(nullptr)
    {
    }

    ForeignPtr(ForeignPtr const& other) = default;
    ForeignPtr& operator=(ForeignPtr const& other) = default;

    T* operator->() const
    {
        ASSERT(m_cell && m_data);
        return m_data;
    }

    [[nodiscard]] T& operator*() const
    {
        ASSERT(m_cell && m_data);
        return *m_data;
    }

    operator T*() const { return m_data; }
    T* ptr() const { return m_data; }

    explicit operator bool() const { return !!m_cell; }
    bool operator!() const { return !m_cell; }

    Ptr<ForeignCell> cell() const { return m_cell; }

    void visit_edges(Cell::Visitor& visitor)
    {
        visitor.visit(m_cell);
    }

private:
    Ptr<ForeignCell> m_cell;
    T* m_data { nullptr };
};

}
