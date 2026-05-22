/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/HeapRoot.h>

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
static void gather_root(HashMap<Cell*, GC::HeapRoot>& roots, T const& value, HeapRoot::Type root_type)
{
    if (auto* cell = RootableValueTraits<T>::cell(value))
        roots.set(cell, HeapRoot { .type = root_type });
}

}
