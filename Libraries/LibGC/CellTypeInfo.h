/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <LibGC/Cell.h>

namespace GC {

struct CellTypeInfo {
    u32 cell_size { 0 };
    u32 alignment { 0 };
    CellKind kind { CellKind::Other };
    void (*visit_edges)(Cell*, Cell::Visitor*) { nullptr };
    void (*finalize)(Cell*) { nullptr };
    void (*destroy)(Cell*) { nullptr };
    size_t (*external_memory_size)(Cell const*) { nullptr };
    char const* (*class_name)(Cell const*, size_t* length) { nullptr };
};

struct CellTypeThunks {
    template<typename T>
    static void visit_edges(Cell* cell, Cell::Visitor* visitor)
    {
        static_cast<T*>(cell)->T::visit_edges(*visitor);
    }

    template<typename T>
    static void finalize(Cell* cell)
    {
        static_cast<T*>(cell)->T::finalize();
    }

    template<typename T>
    static void destroy(Cell* cell)
    {
        static_cast<T*>(cell)->T::~T();
    }

    template<typename T>
    static size_t external_memory_size(Cell const* cell)
    {
        return static_cast<T const*>(cell)->T::external_memory_size();
    }

    static char const* class_name(Cell const* cell, size_t* length)
    {
        auto name = cell->class_name();
        *length = name.length();
        return name.characters_without_null_termination();
    }

    template<typename T>
    static constexpr bool overrides_finalize()
    {
        return !IsSame<decltype(&T::finalize), void (Cell::*)()>;
    }

    template<typename T>
    static constexpr bool overrides_external_memory_size()
    {
        return !IsSame<decltype(&T::external_memory_size), size_t (Cell::*)() const>;
    }

    template<typename T>
    static constexpr CellTypeInfo info()
    {
        static_assert(alignof(T) <= __BIGGEST_ALIGNMENT__);
        return {
            .cell_size = static_cast<u32>(sizeof(T)),
            .alignment = static_cast<u32>(alignof(T)),
            .kind = T::cell_kind_for_class,
            .visit_edges = &visit_edges<T>,
            .finalize = overrides_finalize<T>() ? &finalize<T> : nullptr,
            .destroy = &destroy<T>,
            .external_memory_size = overrides_external_memory_size<T>() ? &external_memory_size<T> : nullptr,
            .class_name = &class_name,
        };
    }
};

template<typename T>
inline constexpr CellTypeInfo cell_type_info_for = CellTypeThunks::info<T>();

}
