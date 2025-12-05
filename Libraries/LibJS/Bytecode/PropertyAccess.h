/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWasm/Opcode.h>

namespace JS::Bytecode {

enum class GetByIdMode {
    Normal,
    Length,
};

ALWAYS_INLINE GC::Ptr<Object> base_object_for_get_impl(VM& vm, Value base_value)
{
    if (base_value.is_object()) [[likely]]
        return base_value.as_object();

    // OPTIMIZATION: For various primitives we can avoid actually creating a new object for them.
    auto& realm = *vm.current_realm();
    if (base_value.is_string())
        return realm.intrinsics().string_prototype();
    if (base_value.is_number())
        return realm.intrinsics().number_prototype();
    if (base_value.is_boolean())
        return realm.intrinsics().boolean_prototype();
    if (base_value.is_bigint())
        return realm.intrinsics().bigint_prototype();
    if (base_value.is_symbol())
        return realm.intrinsics().symbol_prototype();

    return nullptr;
}

template<typename GetBaseIdentifier, typename GetPropertyName>
COLD Completion throw_null_or_undefined_property_get(VM& vm, Value base_value, GetBaseIdentifier get_base_identifier, GetPropertyName get_property_name)
{
    VERIFY(base_value.is_nullish());

    auto base_identifier = get_base_identifier();
    if (base_identifier.has_value())
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithPropertyAndName, get_property_name(), base_value, base_identifier);
    return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithProperty, get_property_name(), base_value);
}

template<typename GetBaseIdentifier, typename GetPropertyName>
ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> base_object_for_get(VM& vm, Value base_value, GetBaseIdentifier get_base_identifier, GetPropertyName get_property_name)
{
    if (auto base_object = base_object_for_get_impl(vm, base_value))
        return GC::Ref { *base_object };

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    return throw_null_or_undefined_property_get(vm, base_value, get_base_identifier, get_property_name);
}

template<typename GetPropertyName>
COLD ThrowCompletionOr<Value> get_by_id_slow_path(
    GetPropertyName get_property_name,
    GC::Ref<Object> base_obj,
    Value this_value,
    PropertyLookupCache& cache,
    Shape& shape,
    GC::Ptr<PrototypeChainValidity> prototype_chain_validity)
{
    CacheableGetPropertyMetadata cacheable_metadata;
    auto value = TRY(base_obj->internal_get(get_property_name(), this_value, &cacheable_metadata));

    // If internal_get() caused object's shape change, we can no longer be sure
    // that collected metadata is valid, e.g. if getter in prototype chain added
    // property with the same name into the object itself.
    if (&shape == &base_obj->shape()) {
        auto get_cache_slot = [&] -> PropertyLookupCache::Entry& {
            for (size_t i = cache.entries.size() - 1; i >= 1; --i) {
                cache.entries[i] = cache.entries[i - 1];
            }
            cache.entries[0] = {};
            return cache.entries[0];
        };
        if (cacheable_metadata.type == CacheableGetPropertyMetadata::Type::GetOwnProperty) {
            auto& entry = get_cache_slot();
            entry.shape = shape;
            entry.property_offset = cacheable_metadata.property_offset.value();

            if (shape.is_dictionary()) {
                entry.shape_dictionary_generation = shape.dictionary_generation();
            }
        } else if (cacheable_metadata.type == CacheableGetPropertyMetadata::Type::GetPropertyInPrototypeChain) {
            auto& entry = get_cache_slot();
            entry.shape = &base_obj->shape();
            entry.property_offset = cacheable_metadata.property_offset.value();
            entry.prototype = *cacheable_metadata.prototype;
            entry.prototype_chain_validity = *prototype_chain_validity;

            if (shape.is_dictionary()) {
                entry.shape_dictionary_generation = shape.dictionary_generation();
            }
        }
    }

    return value;
}

template<GetByIdMode mode, typename GetBaseIdentifier, typename GetPropertyName>
ALWAYS_INLINE ThrowCompletionOr<Value> get_by_id(VM& vm, GetBaseIdentifier get_base_identifier, GetPropertyName get_property_name, Value base_value, Value this_value, PropertyLookupCache& cache)
{
    if constexpr (mode == GetByIdMode::Length) {
        if (base_value.is_string()) {
            return Value(base_value.as_string().length_in_utf16_code_units());
        }
    }

    auto base_obj = TRY(base_object_for_get(vm, base_value, get_base_identifier, get_property_name));

    if constexpr (mode == GetByIdMode::Length) {
        // OPTIMIZATION: Fast path for the magical "length" property on Array objects.
        if (base_obj->has_magical_length_property()) {
            return Value { base_obj->indexed_properties().array_like_size() };
        }
    }

    auto& shape = base_obj->shape();

    GC::Ptr<PrototypeChainValidity> prototype_chain_validity;
    if (shape.prototype())
        prototype_chain_validity = shape.prototype()->shape().prototype_chain_validity();

    for (auto& cache_entry : cache.entries) {
        auto cached_prototype = cache_entry.prototype.ptr();
        if (cached_prototype) {
            // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
            bool can_use_cache = [&]() -> bool {
                if (&shape != cache_entry.shape) [[unlikely]]
                    return false;

                if (shape.is_dictionary()) {
                    VERIFY(cache_entry.shape_dictionary_generation.has_value());
                    if (shape.dictionary_generation() != cache_entry.shape_dictionary_generation.value()) [[unlikely]] {
                        return false;
                    }
                }

                auto cached_prototype_chain_validity = cache_entry.prototype_chain_validity.ptr();
                if (!cached_prototype_chain_validity) [[unlikely]]
                    return false;
                if (!cached_prototype_chain_validity->is_valid()) [[unlikely]]
                    return false;
                return true;
            }();
            if (can_use_cache) [[likely]] {
                auto value = cached_prototype->get_direct(cache_entry.property_offset.value());
                if (value.is_accessor())
                    return TRY(call(vm, value.as_accessor().getter(), this_value));
                return value;
            }
        } else if (&shape == cache_entry.shape) {
            // OPTIMIZATION: If the shape of the object hasn't changed, we can use the cached property offset.
            bool can_use_cache = true;
            if (shape.is_dictionary()) {
                VERIFY(cache_entry.shape_dictionary_generation.has_value());
                if (shape.dictionary_generation() != cache_entry.shape_dictionary_generation.value()) [[unlikely]] {
                    can_use_cache = false;
                }
            }

            if (can_use_cache) [[likely]] {
                auto value = base_obj->get_direct(cache_entry.property_offset.value());
                if (value.is_accessor()) {
                    return TRY(call(vm, value.as_accessor().getter(), this_value));
                }
                return value;
            }
        }
    }

    return get_by_id_slow_path(get_property_name, base_obj, this_value, cache, shape, prototype_chain_validity);
}

}
