/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/CaptureTraits.h>
#include <LibGC/Cell.h>
#include <LibGC/HeapRoot.h>
#include <LibGC/Ptr.h>

namespace GC::Detail {

template<typename T>
inline constexpr bool IsPointerToForeignCell = false;

template<typename T>
inline constexpr bool IsPointerToForeignCell<T*> = IsBaseOf<ForeignCell, T>;

template<typename T>
struct RootableValueTraits {
    static constexpr bool is_rootable = IsBaseOf<NanBoxedValue, T> || IsConvertible<T, Cell const*> || IsPointerToForeignCell<T>;

    static Cell* cell(T const& value)
    {
        if constexpr (IsBaseOf<NanBoxedValue, T>) {
            if (value.is_cell())
                return &const_cast<T&>(value).as_cell();
        } else if constexpr (IsConvertible<T, Cell const*>) {
            return const_cast<Cell*>(static_cast<Cell const*>(value));
        } else if constexpr (IsPointerToForeignCell<T>) {
            return const_cast<Cell*>(as_cell(value));
        }

        return nullptr;
    }
};

template<typename T>
struct RootableValueTraits<Ref<T>> {
    static constexpr bool is_rootable = true;

    static Cell* cell(Ref<T> const& value)
    {
        return const_cast<Cell*>(reinterpret_cast<Cell const*>(value.ptr()));
    }
};

template<typename T>
struct RootableValueTraits<Ptr<T>> {
    static constexpr bool is_rootable = true;

    static Cell* cell(Ptr<T> const& value)
    {
        return const_cast<Cell*>(reinterpret_cast<Cell const*>(value.ptr()));
    }
};

// Records every cell that a traced value visits as a root.
class GC_API RootGatheringVisitor final : public Cell::Visitor {
public:
    RootGatheringVisitor(HashMap<Cell*, HeapRoot>& roots, HeapRoot::Type root_type)
        : m_roots(roots)
        , m_root_type(root_type)
    {
    }

private:
    virtual void visit_impl(Cell&) override;
    virtual void visit_impl(ReadonlySpan<NanBoxedValue>) override;

    HashMap<Cell*, HeapRoot>& m_roots;
    HeapRoot::Type m_root_type;
};

// A root container holds cells, values that may be cells, or values whose cells their visit_edges() names. Such a
// value follows the rules of what a GC::Function captures: everything in it is traced or holds no cell edges, and it
// holds no root of its own.
template<typename T>
inline constexpr bool IsRootableValue = RootableValueTraits<T>::is_rootable || (IsCapturable<T> && !IsUntracedCapture<T>::value);

template<typename T>
static void gather_root(HashMap<Cell*, GC::HeapRoot>& roots, T const& value, HeapRoot::Type root_type)
{
    if constexpr (RootableValueTraits<T>::is_rootable) {
        if (auto* cell = RootableValueTraits<T>::cell(value))
            roots.set(cell, HeapRoot { .type = root_type });
    } else if constexpr (IsSpecializationOf<T, Variant>) {
        // NB: The alternatives are rooted as they are rather than visited, which needs no type of a cell among them to
        //     be complete wherever a vector of them is.
        value.visit([&](auto const& alternative) {
            if constexpr (IsRootableValue<RemoveCVReference<decltype(alternative)>>)
                gather_root(roots, alternative, root_type);
        });
    } else if constexpr (IsSpecializationOf<T, Optional>) {
        if (value.has_value())
            gather_root(roots, value.value(), root_type);
    } else {
        RootGatheringVisitor visitor { roots, root_type };
        visit_capture_member(visitor, value);
    }
}

}
