/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StringBuilder.h>
#include <LibSync/Mutex.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWasm/TypeSystem.h>

namespace Wasm {

// https://webassembly.github.io/spec/core/valid/conventions.html#rolling-and-unrolling
class TypeRegistry {
    AK_MAKE_NONCOPYABLE(TypeRegistry);
    AK_MAKE_NONMOVABLE(TypeRegistry);

public:
    static TypeRegistry& the()
    {
        // Defined types are deliberately immortal: they are the process-wide identities that
        // runtime values and compiled code point at.
        static TypeRegistry* registry = new TypeRegistry;
        return *registry;
    }

    DefinedType const* type_at(u32 registry_index)
    {
        Sync::MutexLocker locker(m_mutex);
        if (registry_index >= m_types.size())
            return nullptr;
        return m_types[registry_index].ptr();
    }

    ErrorOr<Vector<DefinedType const*>, ValidationError> intern_group(ReadonlySpan<TypeSection::Type> types, TypeSection::Type::RecGroupSpan group, ReadonlySpan<DefinedType const*> resolved_so_far);

private:
    TypeRegistry() = default;

    Sync::Mutex m_mutex;
    Vector<NonnullOwnPtr<DefinedType>> m_types;
    HashMap<ByteString, u32> m_interned_groups; // group key -> index of first member
};

DefinedType const* TypeContext::resolve(TypeIndex index) const
{
    if (types.is_empty())
        return TypeRegistry::the().type_at(index.value());
    if (index.value() >= types.size())
        return nullptr;
    return types[index.value()];
}

namespace {

struct RolledTypeUse {
    enum class Form : u8 {
        Closed,
        Recursive,
    };
    Form form;
    u32 index; // registry index (Closed) or group-internal index (Recursive)
};

class GroupRoller {
public:
    GroupRoller(TypeSection::Type::RecGroupSpan group, ReadonlySpan<DefinedType const*> resolved_so_far)
        : m_group(group)
        , m_resolved_so_far(resolved_so_far)
    {
    }

    // https://webassembly.github.io/spec/core/valid/conventions.html#aux-roll-rectype
    // roll_x(rectype) substitutes the type indices [x, x+n) with rec.i; anything below x must already be defined.
    ErrorOr<RolledTypeUse, ValidationError> roll(TypeIndex index) const
    {
        if (index.value() < m_group.first_type_index)
            return RolledTypeUse { RolledTypeUse::Form::Closed, m_resolved_so_far[index.value()]->registry_index() };
        if (index.value() < m_group.first_type_index + m_group.size)
            return RolledTypeUse { RolledTypeUse::Form::Recursive, index.value() - m_group.first_type_index };
        return ValidationError { ByteString::formatted("unknown type {} referenced from recursive type group", index.value()) };
    }

    ErrorOr<void, ValidationError> serialize(StringBuilder& builder, ValueType type) const
    {
        builder.append(static_cast<char>(type.kind()));
        builder.append(type.is_nullable() ? '\1' : '\0');
        if (type.is_typeuse()) {
            auto use = TRY(roll(type.unsafe_typeindex()));
            builder.append(use.form == RolledTypeUse::Form::Closed ? 'C' : 'R');
            builder.append(ReadonlyBytes { &use.index, sizeof(use.index) });
        }
        return {};
    }

    ErrorOr<void, ValidationError> serialize(StringBuilder& builder, FieldType const& field) const
    {
        TRY(serialize(builder, field.type()));
        builder.append(field.is_mutable() ? '\1' : '\0');
        return {};
    }

    ErrorOr<void, ValidationError> serialize(StringBuilder& builder, TypeSection::Type const& type) const
    {
        builder.append(type.is_final() ? '\1' : '\0');
        u32 supertype_count = type.supertypes().size();
        builder.append(ReadonlyBytes { &supertype_count, sizeof(supertype_count) });
        for (auto supertype : type.supertypes()) {
            auto use = TRY(roll(supertype));
            builder.append(use.form == RolledTypeUse::Form::Closed ? 'C' : 'R');
            builder.append(ReadonlyBytes { &use.index, sizeof(use.index) });
        }

        TRY(type.description().visit(
            [&](FunctionType const& function) -> ErrorOr<void, ValidationError> {
                builder.append('F');
                u32 count = function.parameters().size();
                builder.append(ReadonlyBytes { &count, sizeof(count) });
                for (auto& parameter : function.parameters())
                    TRY(serialize(builder, parameter));
                count = function.results().size();
                builder.append(ReadonlyBytes { &count, sizeof(count) });
                for (auto& result : function.results())
                    TRY(serialize(builder, result));
                return {};
            },
            [&](StructType const& struct_) -> ErrorOr<void, ValidationError> {
                builder.append('S');
                u32 count = struct_.fields().size();
                builder.append(ReadonlyBytes { &count, sizeof(count) });
                for (auto& field : struct_.fields())
                    TRY(serialize(builder, field));
                return {};
            },
            [&](ArrayType const& array) -> ErrorOr<void, ValidationError> {
                builder.append('A');
                return serialize(builder, array.type());
            }));
        return {};
    }

    // https://webassembly.github.io/spec/core/valid/conventions.html#aux-unroll-rectype
    ErrorOr<ValueType, ValidationError> substituted(ValueType type, u32 group_base_registry_index) const
    {
        if (!type.is_typeuse())
            return type;
        auto use = TRY(roll(type.unsafe_typeindex()));
        auto registry_index = use.form == RolledTypeUse::Form::Closed ? use.index : group_base_registry_index + use.index;
        return ValueType(ValueType::TypeUseReference, TypeIndex(registry_index), type.is_nullable());
    }

    ErrorOr<TypeSection::Type, ValidationError> substituted(TypeSection::Type const& type, u32 group_base_registry_index) const
    {
        Vector<TypeIndex> supertypes;
        supertypes.ensure_capacity(type.supertypes().size());
        for (auto supertype : type.supertypes()) {
            auto use = TRY(roll(supertype));
            supertypes.append(TypeIndex(use.form == RolledTypeUse::Form::Closed ? use.index : group_base_registry_index + use.index));
        }

        auto composite = TRY(type.description().visit(
            [&](FunctionType const& function) -> ErrorOr<TypeSection::Type::CompositeType, ValidationError> {
                Vector<ValueType> parameters;
                parameters.ensure_capacity(function.parameters().size());
                for (auto& parameter : function.parameters())
                    parameters.append(TRY(substituted(parameter, group_base_registry_index)));
                Vector<ValueType> results;
                results.ensure_capacity(function.results().size());
                for (auto& result : function.results())
                    results.append(TRY(substituted(result, group_base_registry_index)));
                return TypeSection::Type::CompositeType { FunctionType { move(parameters), move(results) } };
            },
            [&](StructType const& struct_) -> ErrorOr<TypeSection::Type::CompositeType, ValidationError> {
                Vector<FieldType> fields;
                fields.ensure_capacity(struct_.fields().size());
                for (auto& field : struct_.fields())
                    fields.append(FieldType { field.is_mutable(), TRY(substituted(field.type(), group_base_registry_index)) });
                return TypeSection::Type::CompositeType { StructType { move(fields) } };
            },
            [&](ArrayType const& array) -> ErrorOr<TypeSection::Type::CompositeType, ValidationError> {
                return TypeSection::Type::CompositeType { ArrayType { FieldType { array.type().is_mutable(), TRY(substituted(array.type().type(), group_base_registry_index)) } } };
            }));

        return TypeSection::Type { move(composite), move(supertypes), type.is_final() };
    }

private:
    TypeSection::Type::RecGroupSpan m_group;
    ReadonlySpan<DefinedType const*> m_resolved_so_far;
};

}

ErrorOr<Vector<DefinedType const*>, ValidationError> TypeRegistry::intern_group(ReadonlySpan<TypeSection::Type> types, TypeSection::Type::RecGroupSpan group, ReadonlySpan<DefinedType const*> resolved_so_far)
{
    GroupRoller roller { group, resolved_so_far };

    StringBuilder key_builder;
    u32 group_size = group.size;
    key_builder.append(ReadonlyBytes { &group_size, sizeof(group_size) });
    for (size_t i = 0; i < group.size; ++i)
        TRY(roller.serialize(key_builder, types[group.first_type_index + i]));
    auto key = key_builder.to_byte_string();

    Sync::MutexLocker locker(m_mutex);

    Optional<u32> base_registry_index = m_interned_groups.get(key);
    if (!base_registry_index.has_value()) {
        auto base = static_cast<u32>(m_types.size());
        for (size_t i = 0; i < group.size; ++i) {
            auto sub_type = TRY(roller.substituted(types[group.first_type_index + i], base));
            sub_type.set_rec_group({ base, group.size });
            m_types.append(adopt_own(*new DefinedType(move(sub_type), base + i)));
        }
        for (size_t i = 0; i < group.size; ++i) {
            auto& defined_type = *m_types[base + i];
            if (!defined_type.m_sub_type.supertypes().is_empty()) {
                auto supertype_registry_index = defined_type.m_sub_type.supertypes().first().value();
                if (supertype_registry_index >= base + i)
                    return ValidationError { ByteString::formatted("Invalid supertype {}, supertypes must precede their subtypes", supertype_registry_index) };
                defined_type.m_supertype = m_types[supertype_registry_index].ptr();
                defined_type.m_subtyping_depth = defined_type.m_supertype->m_subtyping_depth + 1;
                defined_type.m_ancestors.ensure_capacity(defined_type.m_subtyping_depth + 1);
                defined_type.m_ancestors.extend(defined_type.m_supertype->m_ancestors);
            }
            defined_type.m_ancestors.append(&defined_type);
        }
        m_interned_groups.set(move(key), base);
        base_registry_index = base;
    }

    Vector<DefinedType const*> result;
    result.ensure_capacity(group.size);
    for (size_t i = 0; i < group.size; ++i)
        result.append(m_types[*base_registry_index + i].ptr());
    return result;
}

// https://webassembly.github.io/spec/core/valid/modules.html#types
ErrorOr<Vector<DefinedType const*>, ValidationError> canonicalize_module_types(TypeSection const& section)
{
    Vector<DefinedType const*> resolved;
    resolved.ensure_capacity(section.types().size());

    for (size_t type_index = 0; type_index < section.types().size();) {
        auto group = section.types()[type_index].rec_group();
        if (group.first_type_index != type_index || group.first_type_index + group.size > section.types().size())
            return ValidationError { ByteString("malformed recursive type group spans"sv) };
        auto group_types = TRY(TypeRegistry::the().intern_group(section.types(), group, resolved));
        resolved.extend(move(group_types));
        type_index += group.size;
    }

    return resolved;
}

DefinedType const* canonicalize_type(TypeSection::Type const& type, TypeContext const& context)
{
    Vector<TypeSection::Type> closed_types;

    Vector<TypeIndex> supertypes;
    for (auto supertype : type.supertypes()) {
        auto const* resolved = context.resolve(supertype);
        VERIFY(resolved);
        supertypes.append(TypeIndex(resolved->registry_index()));
    }

    auto close = [&](ValueType value_type) {
        return canonicalized(value_type, context);
    };

    auto composite = type.description().visit(
        [&](FunctionType const& function) -> TypeSection::Type::CompositeType {
            Vector<ValueType> parameters;
            for (auto& parameter : function.parameters())
                parameters.append(close(parameter));
            Vector<ValueType> results;
            for (auto& result : function.results())
                results.append(close(result));
            return FunctionType { move(parameters), move(results) };
        },
        [&](StructType const& struct_) -> TypeSection::Type::CompositeType {
            Vector<FieldType> fields;
            for (auto& field : struct_.fields())
                fields.append(FieldType { field.is_mutable(), close(field.type()) });
            return StructType { move(fields) };
        },
        [&](ArrayType const& array) -> TypeSection::Type::CompositeType {
            return ArrayType { FieldType { array.type().is_mutable(), close(array.type().type()) } };
        });

    closed_types.append(TypeSection::Type { move(composite), move(supertypes), type.is_final() });
    closed_types.first().set_rec_group({ 0, 1 });

    auto result = TypeRegistry::the().intern_group(closed_types.span(), { 0, 1 }, {});
    VERIFY(!result.is_error());
    return result.release_value().first();
}

ValueType canonicalized(ValueType type, TypeContext const& context)
{
    if (!type.is_typeuse() || context.types.is_empty())
        return type;
    auto const* resolved = context.resolve(type.unsafe_typeindex());
    VERIFY(resolved);
    return ValueType(ValueType::TypeUseReference, TypeIndex(resolved->registry_index()), type.is_nullable());
}

// https://webassembly.github.io/spec/core/valid/matching.html#defined-types
bool matches_defined_type(DefinedType const& defined_type1, DefinedType const& defined_type2)
{
    if (&defined_type1 == &defined_type2)
        return true;
    if (defined_type2.subtyping_depth() >= defined_type1.subtyping_depth())
        return false;
    return defined_type1.ancestors()[defined_type2.subtyping_depth()] == &defined_type2;
}

// https://webassembly.github.io/spec/core/syntax/types.html#heap-types
ValueType::Kind top_of_heap_type(ValueType const& type, TypeContext const& context)
{
    switch (type.kind()) {
    case ValueType::AnyReference:
    case ValueType::EqReference:
    case ValueType::I31Reference:
    case ValueType::StructReference:
    case ValueType::ArrayReference:
    case ValueType::NoneReference:
        return ValueType::AnyReference;
    case ValueType::FunctionReference:
    case ValueType::NoFunctionReference:
        return ValueType::FunctionReference;
    case ValueType::ExternReference:
    case ValueType::NoExternReference:
        return ValueType::ExternReference;
    case ValueType::ExceptionReference:
    case ValueType::NoExceptionReference:
        return ValueType::ExceptionReference;
    case ValueType::TypeUseReference: {
        auto const* defined_type = context.resolve(type.unsafe_typeindex());
        VERIFY(defined_type);
        return defined_type->expansion().visit(
            [](FunctionType const&) { return ValueType::FunctionReference; },
            [](StructType const&) { return ValueType::AnyReference; },
            [](ArrayType const&) { return ValueType::AnyReference; });
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://webassembly.github.io/spec/core/valid/matching.html#heap-types
bool matches_heap_type(ValueType const& heap_type1, ValueType const& heap_type2, TypeContext const& context)
{
    VERIFY(heap_type1.is_reference() && heap_type2.is_reference());

    // The heap type heaptype_1 matches the heap type heaptype_2 if:
    // Either: The heap type heaptype_2 is of the form heaptype_1.
    if (heap_type1.kind() == heap_type2.kind()) {
        if (!heap_type1.is_typeuse())
            return true;
        auto const* defined_type1 = context.resolve(heap_type1.unsafe_typeindex());
        auto const* defined_type2 = context.resolve(heap_type2.unsafe_typeindex());
        VERIFY(defined_type1 && defined_type2);
        // Or: deftype_1 matches deftype_2.
        return matches_defined_type(*defined_type1, *defined_type2);
    }

    switch (heap_type1.kind()) {
    // Or: heaptype_1 is eq and heaptype_2 is any.
    case ValueType::EqReference:
        return heap_type2.kind() == ValueType::AnyReference;
    // Or: heaptype_1 is i31/struct/array and heaptype_2 is eq (or any, transitively).
    case ValueType::I31Reference:
    case ValueType::StructReference:
    case ValueType::ArrayReference:
        return heap_type2.kind() == ValueType::EqReference || heap_type2.kind() == ValueType::AnyReference;
    // Or: heaptype_1 is a defined type.
    case ValueType::TypeUseReference: {
        auto const* defined_type = context.resolve(heap_type1.unsafe_typeindex());
        VERIFY(defined_type);
        switch (heap_type2.kind()) {
        // ... and heaptype_2 is func and the expansion of deftype is (func t1* -> t2*).
        case ValueType::FunctionReference:
            return defined_type->expansion().has<FunctionType>();
        // ... and heaptype_2 is struct and the expansion of deftype is (struct fieldtype*).
        case ValueType::StructReference:
            return defined_type->expansion().has<StructType>();
        // ... and heaptype_2 is array and the expansion of deftype is (array fieldtype).
        case ValueType::ArrayReference:
            return defined_type->expansion().has<ArrayType>();
        // Transitively: deftype <= struct/array <= eq <= any.
        case ValueType::EqReference:
        case ValueType::AnyReference:
            return defined_type->expansion().has<StructType>() || defined_type->expansion().has<ArrayType>();
        default:
            return false;
        }
    }
    // Or: heaptype_1 is none and heaptype_2 matches any.
    case ValueType::NoneReference:
        return top_of_heap_type(heap_type2, context) == ValueType::AnyReference;
    // Or: heaptype_1 is nofunc and heaptype_2 matches func.
    case ValueType::NoFunctionReference:
        return top_of_heap_type(heap_type2, context) == ValueType::FunctionReference;
    // Or: heaptype_1 is noexn and heaptype_2 matches exn.
    case ValueType::NoExceptionReference:
        return top_of_heap_type(heap_type2, context) == ValueType::ExceptionReference;
    // Or: heaptype_1 is noextern and heaptype_2 matches extern.
    case ValueType::NoExternReference:
        return top_of_heap_type(heap_type2, context) == ValueType::ExternReference;
    default:
        return false;
    }
}

// https://webassembly.github.io/spec/core/valid/matching.html#reference-types
bool matches_reference_type(ValueType const& reference_type1, ValueType const& reference_type2, TypeContext const& context)
{
    if (reference_type1.is_nullable() && !reference_type2.is_nullable())
        return false;
    return matches_heap_type(reference_type1, reference_type2, context);
}

// https://webassembly.github.io/spec/core/valid/matching.html#value-types
bool matches_value_type(ValueType const& value_type1, ValueType const& value_type2, TypeContext const& context)
{
    if (value_type1.is_reference() && value_type2.is_reference())
        return matches_reference_type(value_type1, value_type2, context);
    return !value_type1.is_reference() && !value_type2.is_reference() && value_type1.kind() == value_type2.kind();
}

// https://webassembly.github.io/spec/core/valid/matching.html#result-types
bool matches_result_types(ReadonlySpan<ValueType> result_types1, ReadonlySpan<ValueType> result_types2, TypeContext const& context)
{
    if (result_types1.size() != result_types2.size())
        return false;
    for (size_t i = 0; i < result_types1.size(); ++i) {
        if (!matches_value_type(result_types1[i], result_types2[i], context))
            return false;
    }
    return true;
}

// https://webassembly.github.io/spec/core/valid/matching.html#field-types
static bool matches_storage_type(ValueType const& storage_type1, ValueType const& storage_type2, TypeContext const& context)
{
    if (storage_type1.is_packed() || storage_type2.is_packed())
        return storage_type1.kind() == storage_type2.kind();
    return matches_value_type(storage_type1, storage_type2, context);
}

// https://webassembly.github.io/spec/core/valid/matching.html#field-types
bool matches_field_type(FieldType const& field_type1, FieldType const& field_type2, TypeContext const& context)
{
    if (field_type1.is_mutable() != field_type2.is_mutable())
        return false;
    if (!matches_storage_type(field_type1.type(), field_type2.type(), context))
        return false;
    if (field_type1.is_mutable() && !matches_storage_type(field_type2.type(), field_type1.type(), context))
        return false;
    return true;
}

// https://webassembly.github.io/spec/core/valid/matching.html#composite-types
bool matches_composite_type(TypeSection::Type::CompositeType const& composite_type1, TypeSection::Type::CompositeType const& composite_type2, TypeContext const& context)
{
    return composite_type2.visit(
        [&](FunctionType const& function2) {
            auto const* function1 = composite_type1.get_pointer<FunctionType>();
            if (!function1)
                return false;
            return matches_result_types(function2.parameters(), function1->parameters(), context)
                && matches_result_types(function1->results(), function2.results(), context);
        },
        [&](StructType const& struct2) {
            auto const* struct1 = composite_type1.get_pointer<StructType>();
            if (!struct1 || struct1->fields().size() < struct2.fields().size())
                return false;
            for (size_t i = 0; i < struct2.fields().size(); ++i) {
                if (!matches_field_type(struct1->fields()[i], struct2.fields()[i], context))
                    return false;
            }
            return true;
        },
        [&](ArrayType const& array2) {
            auto const* array1 = composite_type1.get_pointer<ArrayType>();
            if (!array1)
                return false;
            return matches_field_type(array1->type(), array2.type(), context);
        });
}

// https://webassembly.github.io/spec/core/valid/matching.html#limits
bool matches_limits(Limits const& limits1, Limits const& limits2)
{
    if (limits1.min() < limits2.min())
        return false;
    if (!limits2.max().has_value())
        return true;
    return limits1.max().has_value() && *limits1.max() <= *limits2.max();
}

// https://webassembly.github.io/spec/core/valid/matching.html#memory-types
bool matches_memory_type(MemoryType const& memory_type1, MemoryType const& memory_type2)
{
    if (memory_type1.limits().address_type() != memory_type2.limits().address_type())
        return false;
    return matches_limits(memory_type1.limits(), memory_type2.limits());
}

// https://webassembly.github.io/spec/core/valid/matching.html#table-types
bool matches_table_type(TableType const& table_type1, TableType const& table_type2, TypeContext const& context1, TypeContext const& context2)
{
    if (table_type1.limits().address_type() != table_type2.limits().address_type())
        return false;
    if (!matches_limits(table_type1.limits(), table_type2.limits()))
        return false;
    auto element_type1 = canonicalized(table_type1.element_type(), context1);
    auto element_type2 = canonicalized(table_type2.element_type(), context2);
    return matches_reference_type(element_type1, element_type2, TypeContext {})
        && matches_reference_type(element_type2, element_type1, TypeContext {});
}

// https://webassembly.github.io/spec/core/valid/matching.html#global-types
bool matches_global_type(GlobalType const& global_type1, GlobalType const& global_type2, TypeContext const& context1, TypeContext const& context2)
{
    if (global_type1.is_mutable() != global_type2.is_mutable())
        return false;
    auto value_type1 = canonicalized(global_type1.type(), context1);
    auto value_type2 = canonicalized(global_type2.type(), context2);
    if (!matches_value_type(value_type1, value_type2, TypeContext {}))
        return false;
    if (global_type1.is_mutable() && !matches_value_type(value_type2, value_type1, TypeContext {}))
        return false;
    return true;
}

// https://webassembly.github.io/spec/core/valid/matching.html#tag-types
bool matches_tag_type(DefinedType const& defined_type1, DefinedType const& defined_type2)
{
    return matches_defined_type(defined_type1, defined_type2) && matches_defined_type(defined_type2, defined_type1);
}

}
