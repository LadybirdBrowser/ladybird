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
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
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
    if (auto base_object = base_object_for_get_impl(vm, base_value)) [[likely]]
        return GC::Ref { *base_object };

    // NOTE: At this point this is guaranteed to throw (null or undefined).
    return throw_null_or_undefined_property_get(vm, base_value, get_base_identifier, get_property_name);
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

template<typename BaseType, typename PropertyType>
COLD Completion throw_null_or_undefined_property_access(VM& vm, Value base_value, BaseType const& base_identifier, PropertyType const& property_identifier)
{
    VERIFY(base_value.is_nullish());

    bool has_base_identifier = true;
    bool has_property_identifier = true;

    if constexpr (requires { base_identifier.has_value(); })
        has_base_identifier = base_identifier.has_value();
    if constexpr (requires { property_identifier.has_value(); })
        has_property_identifier = property_identifier.has_value();

    if (has_base_identifier && has_property_identifier)
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithPropertyAndName, property_identifier, base_value, base_identifier);
    if (has_property_identifier)
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithProperty, property_identifier, base_value);
    if (has_base_identifier)
        return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefinedWithName, base_identifier, base_value);
    return vm.throw_completion<TypeError>(ErrorType::ToObjectNullOrUndefined);
}

template<PutKind kind>
ThrowCompletionOr<void> put_by_property_key(VM& vm, Value base, Value this_value, Value value, Optional<Utf16FlyString const&> const base_identifier, PropertyKey const& name, Strict strict, PropertyLookupCache* caches = nullptr)
{
    // Better error message than to_object would give
    if (strict == Strict::Yes && base.is_nullish()) [[unlikely]]
        return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base);

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto maybe_object = base.to_object(vm);
    if (maybe_object.is_error()) [[unlikely]]
        return throw_null_or_undefined_property_access(vm, base, base_identifier, name);
    auto object = maybe_object.release_value();

    if constexpr (kind == PutKind::Getter || kind == PutKind::Setter) {
        // The generator should only pass us functions for getters and setters.
        VERIFY(value.is_function());
    }
    switch (kind) {
    case PutKind::Getter: {
        auto& function = value.as_function();
        if (is<ECMAScriptFunctionObject>(function) && static_cast<ECMAScriptFunctionObject const&>(function).name().is_empty())
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(Utf16String::formatted("get {}", name));
        object->define_direct_accessor(name, &function, nullptr, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case PutKind::Setter: {
        auto& function = value.as_function();
        if (is<ECMAScriptFunctionObject>(function) && static_cast<ECMAScriptFunctionObject const&>(function).name().is_empty())
            static_cast<ECMAScriptFunctionObject*>(&function)->set_name(Utf16String::formatted("set {}", name));
        object->define_direct_accessor(name, nullptr, &function, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case PutKind::Normal: {
        auto this_value_object = MUST(this_value.to_object(vm));
        auto& from_shape = this_value_object->shape();
        if (caches) [[likely]] {
            for (auto& cache : caches->entries) {
                switch (cache.type) {
                case PropertyLookupCache::Entry::Type::Empty:
                    break;
                case PropertyLookupCache::Entry::Type::ChangePropertyInPrototypeChain: {
                    auto cached_prototype = cache.prototype.ptr();
                    if (!cached_prototype) [[unlikely]]
                        break;
                    auto cached_shape = cache.shape.ptr();
                    // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
                    bool can_use_cache = [&]() -> bool {
                        if (&object->shape() != cached_shape) [[unlikely]]
                            return false;

                        if (cached_shape->is_dictionary()) {
                            VERIFY(cache.shape_dictionary_generation.has_value());
                            if (object->shape().dictionary_generation() != cache.shape_dictionary_generation.value()) [[unlikely]]
                                return false;
                        }

                        auto cached_prototype_chain_validity = cache.prototype_chain_validity.ptr();
                        if (!cached_prototype_chain_validity) [[unlikely]]
                            return false;
                        if (!cached_prototype_chain_validity->is_valid()) [[unlikely]]
                            return false;
                        return true;
                    }();
                    if (can_use_cache) [[likely]] {
                        auto value_in_prototype = cached_prototype->get_direct(cache.property_offset.value());
                        if (value_in_prototype.is_accessor()) [[unlikely]] {
                            (void)TRY(call(vm, value_in_prototype.as_accessor().setter(), this_value, value));
                            return {};
                        }
                    }
                    break;
                }
                case PropertyLookupCache::Entry::Type::ChangeOwnProperty: {
                    auto cached_shape = cache.shape.ptr();
                    if (cached_shape != &object->shape()) [[unlikely]]
                        break;

                    if (cached_shape->is_dictionary()) {
                        VERIFY(cache.shape_dictionary_generation.has_value());
                        if (cached_shape->dictionary_generation() != cache.shape_dictionary_generation.value())
                            break;
                    }

                    auto value_in_object = object->get_direct(cache.property_offset.value());
                    if (value_in_object.is_accessor()) [[unlikely]] {
                        (void)TRY(call(vm, value_in_object.as_accessor().setter(), this_value, value));
                    } else {
                        object->put_direct(*cache.property_offset, value);
                    }
                    return {};
                }
                case PropertyLookupCache::Entry::Type::AddOwnProperty: {
                    // OPTIMIZATION: If the object's shape is the same as the one cached before adding the new property, we can
                    //               reuse the resulting shape from the cache.
                    if (cache.from_shape != &object->shape()) [[unlikely]]
                        break;
                    auto cached_shape = cache.shape.ptr();
                    if (!cached_shape) [[unlikely]]
                        break;

                    if (cached_shape->is_dictionary()) {
                        VERIFY(cache.shape_dictionary_generation.has_value());
                        if (object->shape().dictionary_generation() != cache.shape_dictionary_generation.value())
                            break;
                    }

                    // The cache is invalid if the prototype chain has been mutated, since such a mutation could have added a setter for the property.
                    auto cached_prototype_chain_validity = cache.prototype_chain_validity.ptr();
                    if (cached_prototype_chain_validity && !cached_prototype_chain_validity->is_valid()) [[unlikely]]
                        break;
                    object->unsafe_set_shape(*cached_shape);
                    object->put_direct(*cache.property_offset, value);
                    return {};
                }
                default:
                    VERIFY_NOT_REACHED();
                }
            }
        }

        CacheableSetPropertyMetadata cacheable_metadata;
        bool succeeded = TRY(object->internal_set(name, value, this_value, &cacheable_metadata));

        auto get_cache_slot = [&] -> PropertyLookupCache::Entry& {
            for (size_t i = caches->entries.size() - 1; i >= 1; --i) {
                caches->entries[i] = caches->entries[i - 1];
            }
            caches->entries[0] = {};
            return caches->entries[0];
        };

        if (succeeded && caches && cacheable_metadata.type == CacheableSetPropertyMetadata::Type::AddOwnProperty) {
            auto& cache = get_cache_slot();
            cache.type = PropertyLookupCache::Entry::Type::AddOwnProperty;
            cache.from_shape = from_shape;
            cache.property_offset = cacheable_metadata.property_offset.value();
            cache.shape = &object->shape();
            if (cacheable_metadata.prototype) {
                cache.prototype_chain_validity = *cacheable_metadata.prototype->shape().prototype_chain_validity();
            }
            if (object->shape().is_dictionary()) {
                cache.shape_dictionary_generation = object->shape().dictionary_generation();
            }
        }

        // If internal_set() caused object's shape change, we can no longer be sure
        // that collected metadata is valid, e.g. if setter in prototype chain added
        // property with the same name into the object itself.
        if (succeeded && caches && &from_shape == &object->shape()) {
            auto& cache = get_cache_slot();
            switch (cacheable_metadata.type) {
            case CacheableSetPropertyMetadata::Type::AddOwnProperty:
                // Something went wrong if we ended up here, because cacheable addition of a new property should've changed the shape.
                VERIFY_NOT_REACHED();
                break;
            case CacheableSetPropertyMetadata::Type::ChangeOwnProperty:
                cache.type = PropertyLookupCache::Entry::Type::ChangeOwnProperty;
                cache.shape = object->shape();
                cache.property_offset = cacheable_metadata.property_offset.value();

                if (object->shape().is_dictionary()) {
                    cache.shape_dictionary_generation = object->shape().dictionary_generation();
                }
                break;
            case CacheableSetPropertyMetadata::Type::ChangePropertyInPrototypeChain:
                cache.type = PropertyLookupCache::Entry::Type::ChangePropertyInPrototypeChain;
                cache.shape = object->shape();
                cache.property_offset = cacheable_metadata.property_offset.value();
                cache.prototype = *cacheable_metadata.prototype;
                cache.prototype_chain_validity = *cacheable_metadata.prototype->shape().prototype_chain_validity();

                if (object->shape().is_dictionary()) {
                    cache.shape_dictionary_generation = object->shape().dictionary_generation();
                }
                break;
            case CacheableSetPropertyMetadata::Type::NotCacheable:
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }

        if (!succeeded && strict == Strict::Yes) [[unlikely]] {
            if (base.is_object())
                return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base);
            return vm.throw_completion<TypeError>(ErrorType::ReferencePrimitiveSetProperty, name, base.typeof_(vm)->utf8_string(), base);
        }
        break;
    }
    case PutKind::Own:
        object->define_direct_property(name, value, Attribute::Enumerable | Attribute::Writable | Attribute::Configurable);
        break;
    case PutKind::Prototype:
        if (value.is_object() || value.is_null()) [[likely]]
            MUST(object->internal_set_prototype_of(value.is_object() ? &value.as_object() : nullptr));
        break;
    }

    return {};
}

}
