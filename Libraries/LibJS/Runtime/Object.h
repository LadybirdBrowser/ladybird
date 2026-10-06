/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Concepts.h>
#include <AK/Function.h>
#include <AK/StringView.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/RootVector.h>
#include <LibJS/Embedding/Layout.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/IndexedProperties.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// The runtime's inline cache metadata, which a [[Get]] or [[Set]] hook only passes on to the internal method it defers
// to. The runtime fills it in, so C++ never creates or reads one.
struct CacheableGetPropertyMetadata;
struct CacheableSetPropertyMetadata;

// An object of the Rust runtime. Its internal methods are those of its class, so the internal_*() functions dispatch
// the way the virtual functions of the C++ runtime's objects do, and the ordinary_*() functions never dispatch.
//
// The facade type of a class of objects derives from Object and recognizes the objects of its class with
// `static bool is_engine_class_of(Object const&)`, which is<T>(), as<T>() and as_if<T>() go through: an object flag
// where the runtime keeps one, the class id for a class that no other class extends, and otherwise whether the object's
// class is that class or extends it.
class JS_API Object : public EngineCell {
public:
    static GC::Ref<Object> create(Realm&, GC::Ptr<Object> prototype);

    enum class PropertyKind {
        Key,
        Value,
        KeyAndValue,
    };

    enum class IntegrityLevel {
        Sealed,
        Frozen,
    };

    enum class ShouldThrowExceptions {
        No,
        Yes,
    };

    enum class PropertyLookupPhase {
        OwnProperty,
        PrototypeChain,
    };

    // 7.3 Operations on Objects, https://tc39.es/ecma262/#sec-operations-on-objects

    ThrowCompletionOr<Value> get(PropertyKey const&) const;
    ThrowCompletionOr<void> set(PropertyKey const&, Value, ShouldThrowExceptions);
    ThrowCompletionOr<bool> create_data_property(PropertyKey const&, Value, Optional<u32>* new_property_offset = nullptr, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr);
    ThrowCompletionOr<bool> create_data_property_or_throw(PropertyKey const&, Value);
    void create_non_enumerable_data_property_or_throw(PropertyKey const&, Value);
    ThrowCompletionOr<void> define_property_or_throw(PropertyKey const&, PropertyDescriptor&);
    ThrowCompletionOr<bool> has_property(PropertyKey const&) const;
    ThrowCompletionOr<bool> has_own_property(PropertyKey const&) const;
    ThrowCompletionOr<bool> set_integrity_level(IntegrityLevel);
    ThrowCompletionOr<GC::RootVector<Value>> enumerable_own_property_names(PropertyKind kind) const;

    // 10.1 Ordinary Object Internal Methods and Internal Slots, https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots

    ThrowCompletionOr<Object*> internal_get_prototype_of() const;
    ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype);
    ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const&) const;
    ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const&, PropertyDescriptor&, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr);
    ThrowCompletionOr<Value> internal_get(PropertyKey const&, Value receiver, CacheableGetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty) const;
    ThrowCompletionOr<bool> internal_set(PropertyKey const&, Value value, Value receiver, CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty);
    ThrowCompletionOr<bool> internal_delete(PropertyKey const&);
    ThrowCompletionOr<GC::RootVector<Value>> internal_own_property_keys() const;

    // Runs [[Get]] on this object as one found in the prototype chain of the lookup that metadata_for_caller belongs to,
    // and fills that metadata only for a hit that an inline cache can keep. An object that forwards its lookups to
    // another one can then cache them without reading the metadata itself.
    ThrowCompletionOr<Value> internal_get_as_prototype_of(PropertyKey const&, Value receiver, CacheableGetPropertyMetadata* metadata_for_caller) const;

    // OrdinaryGetPrototypeOf ( O ) through OrdinaryOwnPropertyKeys ( O ), for exotic objects whose internal methods
    // defer to the ordinary ones. These never dispatch to the object's class.
    ThrowCompletionOr<Object*> ordinary_get_prototype_of() const;
    ThrowCompletionOr<bool> ordinary_set_prototype_of(Object* prototype);
    ThrowCompletionOr<bool> ordinary_prevent_extensions();
    ThrowCompletionOr<Optional<PropertyDescriptor>> ordinary_get_own_property(PropertyKey const& property_key) const;
    ThrowCompletionOr<bool> ordinary_define_own_property(PropertyKey const& property_key, PropertyDescriptor& property_descriptor, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr);
    ThrowCompletionOr<Value> ordinary_get(PropertyKey const& property_key, Value receiver, CacheableGetPropertyMetadata* cacheable_metadata = nullptr, PropertyLookupPhase phase = PropertyLookupPhase::OwnProperty) const;
    ThrowCompletionOr<bool> ordinary_set(PropertyKey const& property_key, Value value, Value receiver, CacheableSetPropertyMetadata* cacheable_metadata = nullptr, PropertyLookupPhase phase = PropertyLookupPhase::OwnProperty);
    ThrowCompletionOr<bool> ordinary_delete(PropertyKey const& property_key);
    ThrowCompletionOr<GC::RootVector<Value>> ordinary_own_property_keys() const;

    [[nodiscard]] bool extensible() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_EXTENSIBLE); }
    [[nodiscard]] bool may_interfere_with_indexed_property_access() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS); }
    [[nodiscard]] bool requires_slow_add_own_property() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_REQUIRES_SLOW_ADD_OWN_PROPERTY); }
    void clear_requires_slow_add_own_property();
    [[nodiscard]] bool is_platform_object() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_PLATFORM_OBJECT); }

    ThrowCompletionOr<bool> ordinary_set_with_own_descriptor(PropertyKey const&, Value, Value, Optional<PropertyDescriptor>, CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty);

    // 10.4.7 Immutable Prototype Exotic Objects, https://tc39.es/ecma262/#sec-immutable-prototype-exotic-objects

    ThrowCompletionOr<bool> set_immutable_prototype(Object* prototype);

    // 14.7.5 The for-in, for-of, and for-await-of Statements

    Optional<Completion> enumerate_object_properties(Function<Optional<Completion>(Value)>) const;

    // Implementation-specific storage abstractions

    bool storage_has(PropertyKey const&) const;
    void storage_delete(PropertyKey const&);

    // Non-standard methods

    // The value of the property in the storage of the object or of its prototype chain, without running any internal
    // method or getter: undefined if there is none, and the accessor itself for an accessor property.
    Value get_without_side_effects(PropertyKey const&) const;

    void define_direct_property(PropertyKey const& property_key, Value value, PropertyAttributes attributes);
    void define_unimplemented_property(Utf16FlyString const& property_name);
    void define_direct_accessor(PropertyKey const&, GC::Ptr<FunctionObject> getter, GC::Ptr<FunctionObject> setter, PropertyAttributes attributes);
    // Cache the getter's result in an engine-private property on this object.
    void define_direct_cached_accessor(PropertyKey const&, GC::Ptr<FunctionObject> getter, GC::Ptr<FunctionObject> setter, PropertyAttributes attributes);
    void clear_cached_accessor_value(PropertyKey const&);

    using IntrinsicAccessor = Value (*)(Realm&);
    void define_intrinsic_accessor(PropertyKey const&, PropertyAttributes attributes, IntrinsicAccessor accessor);

    void define_native_function(Realm&, PropertyKey const&, NativeFunctionPointer, i32 length, PropertyAttributes attributes);
    void define_native_function(Realm&, PropertyKey const&, ESCAPING Function<ThrowCompletionOr<Value>(VM&)>, i32 length, PropertyAttributes attributes);
    void define_native_accessor(Realm&, PropertyKey const&, NativeFunctionPointer getter, NativeFunctionPointer setter, PropertyAttributes attributes);
    void define_native_accessor(Realm&, PropertyKey const&, ESCAPING Function<ThrowCompletionOr<Value>(VM&)> getter, ESCAPING Function<ThrowCompletionOr<Value>(VM&)> setter, PropertyAttributes attributes);

    [[nodiscard]] bool is_function() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_FUNCTION); }
    bool is_date() const;
    [[nodiscard]] bool is_raw_native_function() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_RAW_NATIVE_FUNCTION); }
    [[nodiscard]] bool is_direct_getter_function() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_DIRECT_GETTER_FUNCTION); }
    [[nodiscard]] bool has_global_object_flag() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_GLOBAL_OBJECT); }
    [[nodiscard]] bool is_ecmascript_function_object() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_ECMASCRIPT_FUNCTION_OBJECT); }

    // The [[ErrorData]] of an Error object, or of a host object whose class exposes some.
    ErrorData* error_data();
    ErrorData const* error_data() const;
    bool has_error_data() const { return error_data(); }

    bool eligible_for_own_property_enumeration_fast_path() const;

    // B.3.7 The [[IsHTMLDDA]] Internal Slot, https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot
    [[nodiscard]] bool is_htmldda() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_HTMLDDA); }

    bool has_parameter_map() const;

    // Indexed property storage
    u32 indexed_array_like_size() const;
    // The runtime appends elements with the default attributes, the only ones LibJS's users append with.
    void indexed_append(Value value, PropertyAttributes attributes = default_attributes);
    // The element taken has the default attributes, whatever the ones it was stored with.
    ValueAndAttributes indexed_take_first();

    Shape& shape() { return *engine_field<Shape*>(JS_LAYOUT_OBJECT_SHAPE_OFFSET); }
    Shape const& shape() const { return *engine_field<Shape*>(JS_LAYOUT_OBJECT_SHAPE_OFFSET); }

    void convert_to_prototype_if_needed();
    void invalidate_property_lookup_caches();

    void set_prototype(GC::Ptr<Object>);

    [[nodiscard]] bool has_magical_length_property() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_HAS_MAGICAL_LENGTH_PROPERTY); }
    [[nodiscard]] bool is_typed_array() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_TYPED_ARRAY); }
    [[nodiscard]] bool has_intrinsic_accessors() const { return has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_HAS_INTRINSIC_ACCESSORS); }

    Object const* prototype() const { return shape().prototype(); }

    // The name of the object's class, which for an object of a host class is the name in its JSHostClass.
    StringView class_name() const;

    template<typename T>
    requires(IsBaseOf<Object, T> && requires(Object const& object) { { T::is_engine_class_of(object) } -> SameAs<bool>; })
    bool fast_is() const
    {
        return T::is_engine_class_of(*this);
    }

    bool has_engine_object_flag(u16 layout_object_flag) const
    {
        static_assert(JS_LAYOUT_OBJECT_FLAGS_SIZE == sizeof(u16));
        return (engine_field<u16>(JS_LAYOUT_OBJECT_FLAGS_OFFSET) & layout_object_flag) != 0;
    }

    // A JS_LAYOUT_CLASS_ID_* value. A class that the runtime derives at run time, as it does for each host class, has
    // the id of the class it extends.
    u16 engine_class_id() const { return engine_class_id_of(engine_class()); }

    bool is_of_engine_class_or_subclass(u16 layout_class_id) const
    {
        for (auto const* engine_class = this->engine_class(); engine_class; engine_class = engine_parent_class_of(engine_class)) {
            if (engine_class_id_of(engine_class) == layout_class_id)
                return true;
        }
        return false;
    }

protected:
    // A field of the runtime's object, which a facade type reads at its Layout.h offset.
    template<typename Field>
    Field engine_field(size_t offset) const
    {
        Field field;
        __builtin_memcpy(&field, reinterpret_cast<u8 const*>(this) + offset, sizeof(field));
        return field;
    }

private:
    static_assert(JS_LAYOUT_OBJECT_SHAPE_SIZE == sizeof(void*));
    static_assert(JS_LAYOUT_CELL_CLASS_SIZE == sizeof(void*));
    static_assert(JS_LAYOUT_CLASS_ID_SIZE == sizeof(u16));
    static_assert(JS_LAYOUT_CLASS_PARENT_SIZE == sizeof(void*));

    // The runtime's class of the object, which the first word of every cell points to.
    u8 const* engine_class() const { return engine_field<u8 const*>(JS_LAYOUT_CELL_CLASS_OFFSET); }

    static u16 engine_class_id_of(u8 const* engine_class)
    {
        u16 class_id;
        __builtin_memcpy(&class_id, engine_class + JS_LAYOUT_CLASS_ID_OFFSET, sizeof(class_id));
        return class_id;
    }

    static u8 const* engine_parent_class_of(u8 const* engine_class)
    {
        u8 const* parent_class;
        __builtin_memcpy(&parent_class, engine_class + JS_LAYOUT_CLASS_PARENT_OFFSET, sizeof(parent_class));
        return parent_class;
    }
};

// The table that implements the object's internal methods (see LibJS/HostObjectABI.h), or null for an object that the
// engine implements.
JS_API JSHostClass const* host_class_of(Object const&);

}
