/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibWasm/Export.h>
#include <LibWasm/Types.h>

namespace Wasm {

struct ValidationError;

// https://webassembly.github.io/spec/core/valid/conventions.html#defined-types
// https://webassembly.github.io/spec/core/valid/conventions.html#rolling-and-unrolling
class WASM_API DefinedType {
    AK_MAKE_NONCOPYABLE(DefinedType);
    AK_MAKE_NONMOVABLE(DefinedType);

public:
    // https://webassembly.github.io/spec/core/valid/conventions.html#aux-unroll-deftype
    TypeSection::Type const& sub_type() const { return m_sub_type; }

    // https://webassembly.github.io/spec/core/valid/conventions.html#aux-expand-deftype
    auto& expansion() const { return m_sub_type.description(); }

    // The (single) declared supertype.
    DefinedType const* supertype() const { return m_supertype; }
    bool is_final() const { return m_sub_type.is_final(); }

    u32 subtyping_depth() const { return m_subtyping_depth; }
    ReadonlySpan<DefinedType const*> ancestors() const { return m_ancestors; }

    u32 registry_index() const { return m_registry_index; }

private:
    friend class TypeRegistry;

    DefinedType(TypeSection::Type sub_type, u32 registry_index)
        : m_sub_type(move(sub_type))
        , m_registry_index(registry_index)
    {
    }

    TypeSection::Type m_sub_type;
    DefinedType const* m_supertype { nullptr };
    Vector<DefinedType const*> m_ancestors;
    u32 m_subtyping_depth { 0 };
    u32 m_registry_index { 0 };
};

// https://webassembly.github.io/spec/core/valid/conventions.html#contexts
struct WASM_API TypeContext {
    DefinedType const* resolve(TypeIndex index) const;

    ReadonlySpan<DefinedType const*> types {};
};

WASM_API ErrorOr<Vector<DefinedType const*>, ValidationError> canonicalize_module_types(TypeSection const&);
WASM_API DefinedType const* canonicalize_type(TypeSection::Type const&, TypeContext const& context);
// https://webassembly.github.io/spec/core/valid/conventions.html#aux-clostype
WASM_API ValueType canonicalized(ValueType, TypeContext const&);

// https://webassembly.github.io/spec/core/valid/matching.html
WASM_API ValueType::Kind top_of_heap_type(ValueType const&, TypeContext const&);
WASM_API bool matches_heap_type(ValueType const& heap_type1, ValueType const& heap_type2, TypeContext const&);
WASM_API bool matches_reference_type(ValueType const& reference_type1, ValueType const& reference_type2, TypeContext const&);
WASM_API bool matches_value_type(ValueType const& value_type1, ValueType const& value_type2, TypeContext const&);
WASM_API bool matches_result_types(ReadonlySpan<ValueType> result_types1, ReadonlySpan<ValueType> result_types2, TypeContext const&);
WASM_API bool matches_field_type(FieldType const&, FieldType const&, TypeContext const&);
WASM_API bool matches_composite_type(TypeSection::Type::CompositeType const&, TypeSection::Type::CompositeType const&, TypeContext const&);
WASM_API bool matches_defined_type(DefinedType const&, DefinedType const&);
WASM_API bool matches_limits(Limits const&, Limits const&);
WASM_API bool matches_memory_type(MemoryType const&, MemoryType const&);
WASM_API bool matches_table_type(TableType const&, TableType const&, TypeContext const& context1, TypeContext const& context2);
WASM_API bool matches_global_type(GlobalType const&, GlobalType const&, TypeContext const& context1, TypeContext const& context2);
WASM_API bool matches_tag_type(DefinedType const&, DefinedType const&);

}
