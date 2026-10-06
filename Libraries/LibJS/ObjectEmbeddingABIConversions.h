/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibGC/RootVector.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/Realm.h>

// Conversions of the object model between the facade and the Rust runtime's embedding ABI: property descriptors, lists
// of keys, lookup phases, inline cache metadata and native functions. Like EmbeddingABIConversions.h, which it builds
// on, only the facade's own .cpp files may include it.

namespace JS::EmbeddingABI {

static_assert(to_underlying(Object::PropertyLookupPhase::OwnProperty) == JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY);
static_assert(to_underlying(Object::PropertyLookupPhase::PrototypeChain) == JS_PROPERTY_LOOKUP_PHASE_PROTOTYPE_CHAIN);

inline JSObject* optional_object_to_abi(Object const* object)
{
    return object ? object_to_abi(*object) : nullptr;
}

inline u8 lookup_phase_to_abi(Object::PropertyLookupPhase phase)
{
    return to_underlying(phase);
}

// The runtime's own inline cache metadata, which passes through the facade unread.
inline JSGetCacheMetadata* get_cache_metadata_to_abi(CacheableGetPropertyMetadata* metadata)
{
    return reinterpret_cast<JSGetCacheMetadata*>(metadata);
}

inline JSSetCacheMetadata* set_cache_metadata_to_abi(CacheableSetPropertyMetadata* metadata)
{
    return reinterpret_cast<JSSetCacheMetadata*>(metadata);
}

inline JSPropertyDescriptor property_descriptor_to_abi(PropertyDescriptor const& descriptor)
{
    JSPropertyDescriptor abi_descriptor {};
    u16 flags = JS_PD_PRESENT;
    if (descriptor.value.has_value()) {
        flags |= JS_PD_HAS_VALUE;
        abi_descriptor.value = value_to_abi(*descriptor.value);
    }
    if (descriptor.get.has_value()) {
        flags |= JS_PD_HAS_GET;
        abi_descriptor.get = optional_object_to_abi(descriptor.get->ptr());
    }
    if (descriptor.set.has_value()) {
        flags |= JS_PD_HAS_SET;
        abi_descriptor.set = optional_object_to_abi(descriptor.set->ptr());
    }
    if (descriptor.writable.has_value())
        flags |= JS_PD_HAS_WRITABLE | (*descriptor.writable ? JS_PD_WRITABLE : 0);
    if (descriptor.enumerable.has_value())
        flags |= JS_PD_HAS_ENUMERABLE | (*descriptor.enumerable ? JS_PD_ENUMERABLE : 0);
    if (descriptor.configurable.has_value())
        flags |= JS_PD_HAS_CONFIGURABLE | (*descriptor.configurable ? JS_PD_CONFIGURABLE : 0);
    if (descriptor.property_offset.has_value()) {
        flags |= JS_PD_HAS_PROPERTY_OFFSET;
        abi_descriptor.property_offset = *descriptor.property_offset;
    }
    abi_descriptor.flags = flags;
    return abi_descriptor;
}

// An absent descriptor is a zeroed one, without JS_PD_PRESENT.
inline JSPropertyDescriptor optional_property_descriptor_to_abi(Optional<PropertyDescriptor> const& descriptor)
{
    if (!descriptor.has_value())
        return {};
    return property_descriptor_to_abi(*descriptor);
}

inline Optional<PropertyDescriptor> property_descriptor_from_abi(JSPropertyDescriptor const& abi_descriptor)
{
    auto flags = abi_descriptor.flags;
    if (!(flags & JS_PD_PRESENT))
        return {};
    // The runtime only ever puts functions in a descriptor's getter and setter.
    auto function_from_abi = [](JSObject* function) -> GC::Ptr<FunctionObject> {
        return reinterpret_cast<FunctionObject*>(function);
    };
    PropertyDescriptor descriptor;
    if (flags & JS_PD_HAS_VALUE)
        descriptor.value = value_from_abi(abi_descriptor.value);
    if (flags & JS_PD_HAS_GET)
        descriptor.get = function_from_abi(abi_descriptor.get);
    if (flags & JS_PD_HAS_SET)
        descriptor.set = function_from_abi(abi_descriptor.set);
    if (flags & JS_PD_HAS_WRITABLE)
        descriptor.writable = (flags & JS_PD_WRITABLE) != 0;
    if (flags & JS_PD_HAS_ENUMERABLE)
        descriptor.enumerable = (flags & JS_PD_ENUMERABLE) != 0;
    if (flags & JS_PD_HAS_CONFIGURABLE)
        descriptor.configurable = (flags & JS_PD_CONFIGURABLE) != 0;
    if (flags & JS_PD_HAS_PROPERTY_OFFSET)
        descriptor.property_offset = abi_descriptor.property_offset;
    return descriptor;
}

// A descriptor that the runtime writes back after defining a property with it, as it does to report the storage offset
// of a new property.
inline void update_property_descriptor_from_abi(PropertyDescriptor& descriptor, JSPropertyDescriptor const& abi_descriptor)
{
    if (auto written_back_descriptor = property_descriptor_from_abi(abi_descriptor); written_back_descriptor.has_value())
        descriptor = written_back_descriptor.release_value();
}

// A [[GetOwnProperty]] result that a caller already has: null for none, and a zeroed descriptor for an absent property.
struct PrecomputedOwnPropertyForABI {
    explicit PrecomputedOwnPropertyForABI(Optional<PropertyDescriptor> const* precomputed_get_own_property)
    {
        if (precomputed_get_own_property)
            abi_descriptor = optional_property_descriptor_to_abi(*precomputed_get_own_property);
        is_present = precomputed_get_own_property != nullptr;
    }

    JSPropertyDescriptor const* pointer() const { return is_present ? &abi_descriptor : nullptr; }

    JSPropertyDescriptor abi_descriptor {};
    bool is_present { false };
};

// A sink that appends what the runtime hands it to a list that roots it.
struct RootedValueSink {
    AK_MAKE_NONCOPYABLE(RootedValueSink);
    AK_MAKE_NONMOVABLE(RootedValueSink);

public:
    RootedValueSink()
        : sink { this, [](void* context, JSValue value) { static_cast<RootedValueSink*>(context)->values.append(value_from_abi(value)); } }
    {
    }

    GC::RootVector<Value> values;
    JSValueSink sink;
};

template<typename Collect>
ThrowCompletionOr<GC::RootVector<Value>> collect_values_from_abi(Collect collect)
{
    RootedValueSink rooted_values;
    TRY(completion_from_abi<void>(collect(&rooted_values.sink)));
    return move(rooted_values.values);
}

// The pointer of a completion whose payload is an object or null.
inline ThrowCompletionOr<Object*> optional_object_completion_from_abi(JSCompletion completion)
{
    return TRY(completion_from_abi<GC::Ptr<Object>>(completion)).ptr();
}

struct PrefixForABI {
    explicit PrefixForABI(Optional<StringView> const& prefix)
    {
        if (prefix.has_value()) {
            // A present but empty prefix still has to be told apart from no prefix, which is null.
            characters = prefix->is_empty() ? "" : prefix->characters_without_null_termination();
            length = prefix->length();
        }
    }

    char const* characters { nullptr };
    size_t length { 0 };
};

// A raw native function returns a ThrowCompletionOr<Value> in the registers where the runtime expects its
// JSCompletion, so the runtime calls a NativeFunctionPointer as a JSNativeFunction.
inline JSNativeFunction native_function_to_abi(NativeFunctionPointer native_function)
{
    return bit_cast<JSNativeFunction>(native_function);
}

// A null realm stands for the current realm, as an absent one does for CreateBuiltinFunction.
inline JSRealm* builtin_function_realm_to_abi(Optional<GC::Ptr<Realm>> const& realm)
{
    if (!realm.has_value())
        return nullptr;
    VERIFY(*realm);
    return cell_to_abi<JSRealm>(**realm);
}

}
