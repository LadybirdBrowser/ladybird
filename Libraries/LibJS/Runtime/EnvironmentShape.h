/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Heap/Cell.h>

namespace JS {

class VM;

class JS_API EnvironmentShape final : public Cell {
    GC_CELL(EnvironmentShape, Cell);
    GC_DECLARE_ALLOCATOR(EnvironmentShape);

public:
    enum BindingFlag : u8 {
        BindingFlagStrict = 1 << 0,
        BindingFlagMutable = 1 << 1,
        BindingFlagCanBeDeleted = 1 << 2,
    };

    EnvironmentShape(Vector<Utf16FlyString>, Vector<u8>, HashMap<Utf16FlyString, size_t>);
    virtual ~EnvironmentShape() override = default;

    [[nodiscard]] static GC::Ref<EnvironmentShape> create(VM&, ReadonlySpan<Utf16FlyString> names, ReadonlySpan<u8> flags);

    [[nodiscard]] size_t size() const { return m_binding_names.size(); }
    [[nodiscard]] Utf16FlyString const& binding_name(size_t index) const { return m_binding_names[index]; }
    [[nodiscard]] u8 binding_flags(size_t index) const { return m_binding_flags[index]; }
    [[nodiscard]] Optional<size_t> find_binding(Utf16FlyString const&) const;

private:
    virtual size_t external_memory_size() const override;

    Vector<Utf16FlyString> m_binding_names;
    Vector<u8> m_binding_flags;
    HashMap<Utf16FlyString, size_t> m_binding_indices;
};

}
