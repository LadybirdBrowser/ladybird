/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/HeapRoot.h>
#include <LibGC/Ptr.h>

namespace GC::Detail {

template<typename T>
struct RootableValueTraits {
    static constexpr bool is_rootable = IsBaseOf<NanBoxedValue, T> || IsConvertible<T, Cell const*>;

    static Cell* cell(T const& value)
    {
        if constexpr (IsBaseOf<NanBoxedValue, T>) {
            if (value.is_cell())
                return &const_cast<T&>(value).as_cell();
        } else if constexpr (IsConvertible<T, Cell const*>) {
            return const_cast<Cell*>(static_cast<Cell const*>(value));
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

template<typename T>
static void gather_root(HashMap<Cell*, GC::HeapRoot>& roots, T const& value, HeapRoot::Type root_type)
{
    if (auto* cell = RootableValueTraits<T>::cell(value))
        roots.set(cell, HeapRoot { .type = root_type });
}

}
