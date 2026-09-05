/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/IdentifierTable.h>
#include <LibJS/Bytecode/PutKind.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
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

enum class CachePropertyAbsence {
    No,
    Yes,
};

ALWAYS_INLINE ThrowCompletionOr<Value> get_cached_property_value(VM& vm, Value value, Value this_value)
{
    if (!value.is_accessor())
        return value;

    // https://tc39.es/ecma262/#sec-ordinaryget
    // If _getter_ is *undefined*, return *undefined*.
    auto* getter = value.as_accessor().getter();
    if (!getter)
        return js_undefined();
    return TRY(call(vm, *getter, this_value));
}

ALWAYS_INLINE ThrowCompletionOr<Value> get_by_value_with_keyed_cache(VM& vm, Object& base_object, Value this_value, PropertyKey const& property_key)
{
    if (!property_key.is_string())
        return base_object.internal_get(property_key, this_value);

    auto const& property_name = property_key.as_string();
    auto& shape = base_object.shape();
    auto& entry = vm.keyed_property_lookup_cache().entry_for(shape, property_name);
    if (entry.shape.ptr() == &shape && entry.property_name == property_name
        && (!shape.is_dictionary() || shape.dictionary_generation() == entry.shape_dictionary_generation)) {
        switch (entry.type) {
        case PropertyLookupCache::Entry::Type::GetOwnProperty:
            return get_cached_property_value(vm, base_object.get_direct(entry.property_offset), this_value);
        case PropertyLookupCache::Entry::Type::GetPropertyInPrototypeChain:
            if (auto* prototype_chain_validity = entry.prototype_chain_validity.ptr(); prototype_chain_validity && prototype_chain_validity->is_valid())
                return get_cached_property_value(vm, entry.prototype->get_direct(entry.property_offset), this_value);
            break;
        case PropertyLookupCache::Entry::Type::GetMissingProperty:
            if (base_object.is_cacheable_for_property_absence()) {
                if (!shape.prototype())
                    return js_undefined();
                if (auto* prototype_chain_validity = entry.prototype_chain_validity.ptr(); prototype_chain_validity && prototype_chain_validity->is_valid())
                    return js_undefined();
            }
            break;
        default:
            break;
        }
    }

    GC::Ptr<PrototypeChainValidity> prototype_chain_validity;
    if (shape.prototype())
        prototype_chain_validity = shape.prototype()->shape().prototype_chain_validity();

    auto dictionary_generation = shape.dictionary_generation();
    CacheableGetPropertyMetadata cacheable_metadata;
    cacheable_metadata.property_absence_is_cacheable = base_object.is_cacheable_for_property_absence();
    auto value = TRY(base_object.internal_get(property_key, this_value, &cacheable_metadata));

    // A getter may have changed the object's shape or the property storage of a dictionary shape, which
    // leaves the metadata describing a lookup that no longer applies.
    if (&shape != &base_object.shape() || shape.dictionary_generation() != dictionary_generation
        || cacheable_metadata.type == CacheableGetPropertyMetadata::Type::NotCacheable)
        return value;

    entry = {};
    entry.shape = &shape;
    entry.property_name = property_name;
    if (shape.is_dictionary())
        entry.shape_dictionary_generation = shape.dictionary_generation();
    switch (cacheable_metadata.type) {
    case CacheableGetPropertyMetadata::Type::GetOwnProperty:
        entry.type = PropertyLookupCache::Entry::Type::GetOwnProperty;
        entry.property_offset = cacheable_metadata.property_offset.value();
        break;
    case CacheableGetPropertyMetadata::Type::GetPropertyInPrototypeChain:
        entry.type = PropertyLookupCache::Entry::Type::GetPropertyInPrototypeChain;
        entry.property_offset = cacheable_metadata.property_offset.value();
        entry.prototype = const_cast<Object*>(cacheable_metadata.prototype.ptr());
        entry.prototype_chain_validity = prototype_chain_validity;
        break;
    case CacheableGetPropertyMetadata::Type::GetMissingProperty:
        entry.type = PropertyLookupCache::Entry::Type::GetMissingProperty;
        entry.prototype_chain_validity = prototype_chain_validity;
        break;
    case CacheableGetPropertyMetadata::Type::NotCacheable:
        VERIFY_NOT_REACHED();
    }
    return value;
}

// Non-standard
ALWAYS_INLINE Value get_own_property_without_side_effects(Object& object, PropertyKey const& property_key, StaticPropertyLookupCache& cache)
{
    auto& shape = object.shape();

    if (auto const* cache_entry = cache.first_entry(); cache_entry
        && (cache_entry->type == PropertyLookupCache::Entry::Type::GetOwnProperty
            || cache_entry->type == PropertyLookupCache::Entry::Type::GetMissingProperty)
        && &shape == cache_entry->shape.ptr()
        && (!shape.is_dictionary() || shape.dictionary_generation() == cache_entry->shape_dictionary_generation)) {
        if (cache_entry->type == PropertyLookupCache::Entry::Type::GetMissingProperty)
            return {};
        return object.get_direct(cache_entry->property_offset);
    }

    auto metadata = shape.lookup(property_key);
    auto cache_type = metadata.has_value()
        ? PropertyLookupCache::Entry::Type::GetOwnProperty
        : PropertyLookupCache::Entry::Type::GetMissingProperty;
    cache.update(cache_type, [&](auto& entry) {
        entry.shape = shape;
        if (metadata.has_value())
            entry.property_offset = metadata->offset;
        if (shape.is_dictionary())
            entry.shape_dictionary_generation = shape.dictionary_generation();
    });

    if (!metadata.has_value())
        return {};
    return object.get_direct(metadata->offset);
}

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
ALWAYS_INLINE ThrowCompletionOr<Value> get_by_id(VM& vm, GetBaseIdentifier get_base_identifier, GetPropertyName get_property_name, Value base_value, Value this_value, PropertyLookupCache& cache, CachePropertyAbsence cache_property_absence = CachePropertyAbsence::No)
{
    if constexpr (mode == GetByIdMode::Length) {
        if (base_value.is_string()) {
            return Value(base_value.as_string().length_in_utf16_code_units());
        }
    }

    auto const& property_name = get_property_name();
    if (base_value.is_string()) {
        // https://tc39.es/ecma262/#sec-stringgetownproperty
        // String exotic objects expose virtual own properties for canonical string indexes.
        auto string_value = TRY(base_value.as_string().get(vm, property_name));
        if (string_value.has_value())
            return *string_value;
    }

    auto base_obj = TRY(base_object_for_get(vm, base_value, get_base_identifier, get_property_name));

    if constexpr (mode == GetByIdMode::Length) {
        // OPTIMIZATION: Fast path for the magical "length" property on Array objects.
        if (base_obj->has_magical_length_property()) {
            return Value { base_obj->indexed_array_like_size() };
        }
    }

    auto& shape = base_obj->shape();

    for (auto& cache_entry : cache.entries_for_shape(shape)) {
        if (cache_entry.type == PropertyLookupCache::Entry::Type::GetMissingProperty) {
            if (cache_property_absence == CachePropertyAbsence::No)
                continue;
            if (!base_obj->is_cacheable_for_property_absence()) [[unlikely]]
                continue;
            if (&shape != cache_entry.shape.ptr()) [[unlikely]]
                continue;
            if (shape.is_dictionary() && shape.dictionary_generation() != cache_entry.shape_dictionary_generation) [[unlikely]]
                continue;
            if (shape.prototype()) {
                auto prototype_chain_validity = cache_entry.prototype_chain_validity.ptr();
                if (!prototype_chain_validity || !prototype_chain_validity->is_valid()) [[unlikely]]
                    continue;
            }
            return js_undefined();
        }

        if (cache_entry.type != PropertyLookupCache::Entry::Type::GetOwnProperty
            && cache_entry.type != PropertyLookupCache::Entry::Type::GetPropertyInPrototypeChain) {
            continue;
        }

        auto cached_prototype = cache_entry.prototype.ptr();
        if (cached_prototype) {
            // OPTIMIZATION: If the prototype chain hasn't been mutated in a way that would invalidate the cache, we can use it.
            bool can_use_cache = [&]() -> bool {
                if (&shape != cache_entry.shape.ptr()) [[unlikely]]
                    return false;

                if (shape.is_dictionary()) {
                    if (shape.dictionary_generation() != cache_entry.shape_dictionary_generation) [[unlikely]] {
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
                auto value = cached_prototype->get_direct(cache_entry.property_offset);
                return TRY(get_cached_property_value(vm, value, this_value));
            }
        } else if (&shape == cache_entry.shape.ptr()) {
            // OPTIMIZATION: If the shape of the object hasn't changed, we can use the cached property offset.
            bool can_use_cache = true;
            if (shape.is_dictionary()) {
                if (shape.dictionary_generation() != cache_entry.shape_dictionary_generation) [[unlikely]] {
                    can_use_cache = false;
                }
            }

            if (can_use_cache) [[likely]] {
                auto value = base_obj->get_direct(cache_entry.property_offset);
                return TRY(get_cached_property_value(vm, value, this_value));
            }
        }
    }
    GC::Ptr<PrototypeChainValidity> prototype_chain_validity;
    if (shape.prototype())
        prototype_chain_validity = shape.prototype()->shape().prototype_chain_validity();

    auto dictionary_generation = shape.dictionary_generation();
    CacheableGetPropertyMetadata cacheable_metadata;
    cacheable_metadata.property_absence_is_cacheable = base_obj->is_cacheable_for_property_absence();
    auto value = TRY(base_obj->internal_get(property_name, this_value, &cacheable_metadata));

    // If internal_get() caused object's shape change, we can no longer be sure
    // that collected metadata is valid, e.g. if getter in prototype chain added
    // property with the same name into the object itself. The same applies when
    // a getter changed the property storage of a dictionary shape.
    if (&shape == &base_obj->shape() && shape.dictionary_generation() == dictionary_generation) {
        if (cacheable_metadata.type == CacheableGetPropertyMetadata::Type::GetOwnProperty) {
            cache.update(PropertyLookupCache::Entry::Type::GetOwnProperty, [&](auto& entry) {
                entry.shape = shape;
                entry.property_offset = cacheable_metadata.property_offset.value();

                if (shape.is_dictionary()) {
                    entry.shape_dictionary_generation = shape.dictionary_generation();
                }
            });
        } else if (cacheable_metadata.type == CacheableGetPropertyMetadata::Type::GetPropertyInPrototypeChain) {
            cache.update(PropertyLookupCache::Entry::Type::GetPropertyInPrototypeChain, [&](auto& entry) {
                entry.shape = &base_obj->shape();
                entry.property_offset = cacheable_metadata.property_offset.value();
                entry.prototype = const_cast<Object*>(cacheable_metadata.prototype.ptr());
                entry.prototype_chain_validity = prototype_chain_validity;

                if (shape.is_dictionary()) {
                    entry.shape_dictionary_generation = shape.dictionary_generation();
                }
            });
        } else if (cache_property_absence == CachePropertyAbsence::Yes
            && cacheable_metadata.type == CacheableGetPropertyMetadata::Type::GetMissingProperty) {
            cache.update(PropertyLookupCache::Entry::Type::GetMissingProperty, [&](auto& entry) {
                entry.shape = shape;
                entry.prototype_chain_validity = prototype_chain_validity;

                if (shape.is_dictionary())
                    entry.shape_dictionary_generation = shape.dictionary_generation();
            });
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

inline ThrowCompletionOr<void> put_by_property_key(VM& vm, Value base, Value this_value, Value value, Optional<Utf16FlyString const&> const base_identifier, PropertyKey const& name, PutKind kind, Strict strict, PropertyLookupCache* caches = nullptr)
{
    // Better error message than to_object would give
    if (strict == Strict::Yes && base.is_nullish()) [[unlikely]]
        return vm.throw_completion<TypeError>(ErrorType::ReferenceNullishSetProperty, name, base);

    // a. Let baseObj be ? ToObject(V.[[Base]]).
    auto maybe_object = base.to_object(vm);
    if (maybe_object.is_error()) [[unlikely]]
        return throw_null_or_undefined_property_access(vm, base, base_identifier, name);
    auto object = maybe_object.release_value();

    if (kind == PutKind::Getter || kind == PutKind::Setter) {
        // The generator should only pass us functions for getters and setters.
        VERIFY(value.is_function());
    }
    switch (kind) {
    case PutKind::Getter: {
        auto& function = value.as_function();
        if (is<ECMAScriptFunctionObject>(function) && static_cast<ECMAScriptFunctionObject const&>(function).name().is_empty())
            static_cast<ECMAScriptFunctionObject*>(&function)->set_inferred_name(Variant<PropertyKey, PrivateName> { name }, "get"sv);
        object->define_direct_accessor(name, &function, nullptr, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case PutKind::Setter: {
        auto& function = value.as_function();
        if (is<ECMAScriptFunctionObject>(function) && static_cast<ECMAScriptFunctionObject const&>(function).name().is_empty())
            static_cast<ECMAScriptFunctionObject*>(&function)->set_inferred_name(Variant<PropertyKey, PrivateName> { name }, "set"sv);
        object->define_direct_accessor(name, nullptr, &function, Attribute::Configurable | Attribute::Enumerable);
        break;
    }
    case PutKind::Normal: {
        auto this_value_object = MUST(this_value.to_object(vm));
        auto& from_shape = this_value_object->shape();
        auto from_shape_dictionary_generation = from_shape.dictionary_generation();
        if (caches) [[likely]] {
            for (auto& cache : caches->entries_for_shape(object->shape())) {
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
                            if (object->shape().dictionary_generation() != cache.shape_dictionary_generation) [[unlikely]]
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
                        auto value_in_prototype = cached_prototype->get_direct(cache.property_offset);
                        if (value_in_prototype.is_accessor()) [[unlikely]] {
                            auto* setter = value_in_prototype.as_accessor().setter();
                            if (!setter)
                                break;
                            (void)TRY(call(vm, *setter, this_value, value));
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
                        if (cached_shape->dictionary_generation() != cache.shape_dictionary_generation)
                            break;
                    }

                    auto value_in_object = object->get_direct(cache.property_offset);
                    if (value_in_object.is_accessor()) [[unlikely]] {
                        auto* setter = value_in_object.as_accessor().setter();
                        if (!setter)
                            break;
                        (void)TRY(call(vm, *setter, this_value, value));
                    } else {
                        object->put_direct(cache.property_offset, value);
                    }
                    return {};
                }
                case PropertyLookupCache::Entry::Type::AddOwnProperty: {
                    // OPTIMIZATION: If the object's shape is the same as the one cached before adding the new property, we can
                    //               reuse the resulting shape from the cache.
                    if (cache.from_shape.ptr() != &object->shape()) [[unlikely]]
                        break;
                    if (object->requires_slow_add_own_property()) [[unlikely]]
                        break;
                    auto cached_shape = cache.shape.ptr();
                    if (!cached_shape) [[unlikely]]
                        break;

                    // Cannot add properties to non-extensible objects (frozen, sealed, or preventExtensions).
                    if (!TRY(object->internal_is_extensible())) [[unlikely]]
                        break;

                    if (cached_shape->is_dictionary()) {
                        if (object->shape().dictionary_generation() != cache.shape_dictionary_generation)
                            break;
                    }

                    // The cache is invalid if the prototype chain has been mutated, since such a mutation could have added a setter for the property.
                    auto cached_prototype_chain_validity = cache.prototype_chain_validity.ptr();
                    if (cached_prototype_chain_validity && !cached_prototype_chain_validity->is_valid()) [[unlikely]]
                        break;
                    object->unsafe_set_shape(*cached_shape);
                    object->put_direct(cache.property_offset, value);
                    return {};
                }
                case PropertyLookupCache::Entry::Type::GetOwnProperty:
                case PropertyLookupCache::Entry::Type::GetPropertyInPrototypeChain:
                case PropertyLookupCache::Entry::Type::GetMissingProperty:
                    break;
                }
            }
        }

        CacheableSetPropertyMetadata cacheable_metadata;
        bool succeeded = TRY(object->internal_set(name, value, this_value, &cacheable_metadata));

        if (succeeded && caches && cacheable_metadata.type == CacheableSetPropertyMetadata::Type::AddOwnProperty) {
            caches->update(PropertyLookupCache::Entry::Type::AddOwnProperty, [&](auto& cache) {
                cache.from_shape = from_shape;
                cache.property_offset = cacheable_metadata.property_offset.value();
                cache.shape = &object->shape();
                if (cacheable_metadata.prototype) {
                    cache.prototype_chain_validity = cacheable_metadata.prototype->shape().prototype_chain_validity();
                }
                if (object->shape().is_dictionary()) {
                    cache.shape_dictionary_generation = object->shape().dictionary_generation();
                }
            });
        }

        // If internal_set() caused object's shape change, we can no longer be sure
        // that collected metadata is valid, e.g. if setter in prototype chain added
        // property with the same name into the object itself. The same applies when
        // a setter changed the property storage of a dictionary shape.
        if (succeeded && caches && &from_shape == &object->shape() && from_shape.dictionary_generation() == from_shape_dictionary_generation) {
            switch (cacheable_metadata.type) {
            case CacheableSetPropertyMetadata::Type::AddOwnProperty:
                // Something went wrong if we ended up here, because cacheable addition of a new property should've changed the shape.
                VERIFY_NOT_REACHED();
                break;
            case CacheableSetPropertyMetadata::Type::ChangeOwnProperty:
                caches->update(PropertyLookupCache::Entry::Type::ChangeOwnProperty, [&](auto& cache) {
                    cache.shape = &object->shape();
                    cache.property_offset = cacheable_metadata.property_offset.value();

                    if (object->shape().is_dictionary()) {
                        cache.shape_dictionary_generation = object->shape().dictionary_generation();
                    }
                });
                break;
            case CacheableSetPropertyMetadata::Type::ChangePropertyInPrototypeChain:
                caches->update(PropertyLookupCache::Entry::Type::ChangePropertyInPrototypeChain, [&](auto& cache) {
                    cache.shape = &object->shape();
                    cache.property_offset = cacheable_metadata.property_offset.value();
                    cache.prototype = const_cast<Object*>(cacheable_metadata.prototype.ptr());
                    cache.prototype_chain_validity = cacheable_metadata.prototype->shape().prototype_chain_validity();

                    if (object->shape().is_dictionary()) {
                        cache.shape_dictionary_generation = object->shape().dictionary_generation();
                    }
                });
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
            return vm.throw_completion<TypeError>(ErrorType::ReferencePrimitiveSetProperty, name, base.typeof_(vm)->utf16_string_view(), base);
        }
        break;
    }
    case PutKind::Own: {
        if (caches) [[likely]] {
            for (auto& cache : caches->entries_for_shape(object->shape())) {
                if (cache.type == PropertyLookupCache::Entry::Type::AddOwnProperty) {
                    // PutKind::Own is not currently emitted for platform
                    // objects, but keep this aligned with the normal PutById
                    // AddOwnProperty cache hit so a future bytecode path cannot
                    // bypass subclass hooks for objects that require them.
                    if (cache.from_shape.ptr() != &object->shape()) [[unlikely]]
                        continue;
                    if (object->requires_slow_add_own_property()) [[unlikely]]
                        continue;
                    auto cached_shape = cache.shape.ptr();
                    if (!cached_shape) [[unlikely]]
                        continue;
                    if (cached_shape->is_dictionary()) {
                        if (object->shape().dictionary_generation() != cache.shape_dictionary_generation)
                            continue;
                    }
                    object->unsafe_set_shape(*cached_shape);
                    object->put_direct(cache.property_offset, value);
                    return {};
                }
            }
        }

        auto& from_shape = object->shape();
        object->define_direct_property(name, value, Attribute::Enumerable | Attribute::Writable | Attribute::Configurable);

        if (caches && &from_shape != &object->shape()) {
            caches->update(PropertyLookupCache::Entry::Type::AddOwnProperty, [&](auto& cache) {
                cache.from_shape = from_shape;
                cache.shape = &object->shape();
                cache.property_offset = object->shape().lookup(name)->offset;
                if (object->shape().is_dictionary()) {
                    cache.shape_dictionary_generation = object->shape().dictionary_generation();
                }
            });
        }
        break;
    }
    case PutKind::Prototype:
        if (value.is_object() || value.is_null()) [[likely]]
            MUST(object->internal_set_prototype_of(value.is_object() ? &value.as_object() : nullptr));
        break;
    }

    return {};
}

}
