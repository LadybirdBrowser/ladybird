/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/StringView.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/RootVector.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/IndexedProperties.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PrivateEnvironment.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

#define JS_OBJECT(class_, base_class) GC_CELL(class_, base_class)

struct PrivateElement {
    enum class Kind {
        Field,
        Method,
        Accessor
    };

    PrivateName key;
    Kind kind { Kind::Field };
    Value value;

    void visit_edges(Cell::Visitor& visitor)
    {
        visitor.visit(value);
    }
};

// Non-standard: This is information optionally returned by object property access functions.
//               It can be used to implement inline caches for property lookup.
struct CacheableGetPropertyMetadata {
    enum class Type {
        NotCacheable,
        GetOwnProperty,
        GetPropertyInPrototypeChain,
    };
    Type type { Type::NotCacheable };
    Optional<u32> property_offset;
    GC::Ptr<Object const> prototype;
};

struct CacheableSetPropertyMetadata {
    enum class Type {
        NotCacheable,
        AddOwnProperty,
        ChangeOwnProperty,
        ChangePropertyInPrototypeChain,
    };
    Type type { Type::NotCacheable };
    Optional<u32> property_offset;
    GC::Ptr<Object const> prototype;
};

class JS_API Object : public Cell {
    GC_CELL(Object, Cell);
    GC_DECLARE_ALLOCATOR(Object);

public:
    static GC::Ref<Object> create_prototype(Realm&, Object* prototype);
    static GC::Ref<Object> create(Realm&, Object* prototype);
    static GC::Ref<Object> create_with_premade_shape(Shape&);

    virtual void initialize(Realm&) override;
    GC_ALLOW_CELL_DESTRUCTOR virtual ~Object();

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

    enum class MayInterfereWithIndexedPropertyAccess {
        No,
        Yes,
    };

    // Please DO NOT make up your own non-standard methods unless you
    // have a very good reason to do so. If any object abstract
    // operation from the spec is missing, add it instead.
    // Functionality for implementation details like shapes and
    // property storage are obviously exempt from this rule :^)
    //
    // Methods named [[Foo]]() in the spec are named internal_foo()
    // here, as they are "The [[Foo]] internal method of a ... object".
    // They must be virtual and may be overridden. All other methods
    // follow the regular PascalCase name converted to camel_case
    // naming convention and must not be virtual.

    // 7.1 Type Conversion, https://tc39.es/ecma262/#sec-type-conversion

    ThrowCompletionOr<Value> ordinary_to_primitive(Value::PreferredType preferred_type) const;

    // 7.2 Testing and Comparison Operations, https://tc39.es/ecma262/#sec-testing-and-comparison-operations

    ThrowCompletionOr<bool> is_extensible() const;

    // 7.3 Operations on Objects, https://tc39.es/ecma262/#sec-operations-on-objects

    ThrowCompletionOr<Value> get(PropertyKey const&) const;
    ThrowCompletionOr<Value> get(PropertyKey const&, Bytecode::PropertyLookupCache&) const;
    ThrowCompletionOr<void> set(PropertyKey const&, Value, ShouldThrowExceptions);
    ThrowCompletionOr<void> set(PropertyKey const&, Value, Bytecode::PropertyLookupCache&);
    ThrowCompletionOr<bool> create_data_property(PropertyKey const&, Value, Optional<u32>* new_property_offset = nullptr);
    void create_method_property(PropertyKey const&, Value);
    ThrowCompletionOr<bool> create_data_property_or_throw(PropertyKey const&, Value);
    void create_non_enumerable_data_property_or_throw(PropertyKey const&, Value);
    ThrowCompletionOr<void> define_property_or_throw(PropertyKey const&, PropertyDescriptor&);
    ThrowCompletionOr<void> delete_property_or_throw(PropertyKey const&);
    ThrowCompletionOr<bool> has_property(PropertyKey const&) const;
    ThrowCompletionOr<bool> has_own_property(PropertyKey const&) const;
    ThrowCompletionOr<bool> set_integrity_level(IntegrityLevel);
    ThrowCompletionOr<bool> test_integrity_level(IntegrityLevel) const;
    ThrowCompletionOr<GC::RootVector<Value>> enumerable_own_property_names(PropertyKind kind) const;
    ThrowCompletionOr<void> copy_data_properties(VM&, Value source, HashTable<PropertyKey> const& excluded_keys, HashTable<JS::Value> const& excluded_values = {});
    ThrowCompletionOr<GC::Ref<Object>> snapshot_own_properties(VM&, GC::Ptr<Object> prototype, HashTable<PropertyKey> const& excluded_keys = {}, HashTable<Value> const& excluded_values = {});

    PrivateElement* private_element_find(PrivateName const& name);
    ThrowCompletionOr<void> private_field_add(PrivateName const& name, Value value);
    ThrowCompletionOr<void> private_method_or_accessor_add(PrivateElement element);
    ThrowCompletionOr<Value> private_get(PrivateName const& name);
    ThrowCompletionOr<void> private_set(PrivateName const& name, Value value);
    ThrowCompletionOr<void> define_field(ClassFieldDefinition const&);
    ThrowCompletionOr<void> initialize_instance_elements(ECMAScriptFunctionObject& constructor);

    // 10.1 Ordinary Object Internal Methods and Internal Slots, https://tc39.es/ecma262/#sec-ordinary-object-internal-methods-and-internal-slots

    virtual ThrowCompletionOr<Object*> internal_get_prototype_of() const;
    virtual ThrowCompletionOr<bool> internal_set_prototype_of(Object* prototype);
    virtual ThrowCompletionOr<bool> internal_is_extensible() const;
    virtual ThrowCompletionOr<bool> internal_prevent_extensions();
    virtual ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const&) const;
    virtual ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const&, PropertyDescriptor&, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr);
    virtual ThrowCompletionOr<bool> internal_has_property(PropertyKey const&) const;
    enum class PropertyLookupPhase {
        OwnProperty,
        PrototypeChain,
    };
    virtual ThrowCompletionOr<Value> internal_get(PropertyKey const&, Value receiver, CacheableGetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty) const;
    virtual ThrowCompletionOr<bool> internal_set(PropertyKey const&, Value value, Value receiver, CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty);
    virtual ThrowCompletionOr<bool> internal_delete(PropertyKey const&);
    virtual ThrowCompletionOr<GC::RootVector<Value>> internal_own_property_keys() const;

    // NOTE: Any subclass of Object that overrides property access slots ([[Get]], [[Set]] etc)
    //       to customize access to indexed properties (properties where the name is a positive integer)
    //       must return true for this, to opt out of optimizations that rely on assumptions that
    //       might not hold when property access behaves differently.
    bool may_interfere_with_indexed_property_access() const { return m_may_interfere_with_indexed_property_access; }

    ThrowCompletionOr<bool> ordinary_set_with_own_descriptor(PropertyKey const&, Value, Value, Optional<PropertyDescriptor>, CacheableSetPropertyMetadata* = nullptr, PropertyLookupPhase = PropertyLookupPhase::OwnProperty);

    // 10.4.7 Immutable Prototype Exotic Objects, https://tc39.es/ecma262/#sec-immutable-prototype-exotic-objects

    ThrowCompletionOr<bool> set_immutable_prototype(Object* prototype);

    // 20.1 Object Objects, https://tc39.es/ecma262/#sec-object-objects

    ThrowCompletionOr<Object*> define_properties(Value properties);

    // 14.7.5 The for-in, for-of, and for-await-of Statements

    Optional<Completion> enumerate_object_properties(Function<Optional<Completion>(Value)>) const;

    // Implementation-specific storage abstractions

    Optional<ValueAndAttributes> storage_get(PropertyKey const&) const;
    bool storage_has(PropertyKey const&) const;
    Optional<u32> storage_set(PropertyKey const&, ValueAndAttributes const&);
    void storage_delete(PropertyKey const&);

    // Non-standard methods

    ThrowCompletionOr<void> for_each_own_property_with_enumerability(Function<ThrowCompletionOr<void>(PropertyKey const&, bool)>&&) const;
    size_t own_properties_count() const;

    Value get_without_side_effects(PropertyKey const&) const;

    void define_direct_property(PropertyKey const& property_key, Value value, PropertyAttributes attributes) { (void)storage_set(property_key, { value, attributes }); }
    void define_direct_accessor(PropertyKey const&, FunctionObject* getter, FunctionObject* setter, PropertyAttributes attributes);

    using IntrinsicAccessor = Value (*)(Realm&);
    void define_intrinsic_accessor(PropertyKey const&, PropertyAttributes attributes, IntrinsicAccessor accessor);

    void define_native_function(Realm&, PropertyKey const&, ESCAPING Function<ThrowCompletionOr<Value>(VM&)>, i32 length, PropertyAttributes attributes, Optional<Bytecode::Builtin> builtin = {});
    void define_native_accessor(Realm&, PropertyKey const&, ESCAPING Function<ThrowCompletionOr<Value>(VM&)> getter, ESCAPING Function<ThrowCompletionOr<Value>(VM&)> setter, PropertyAttributes attributes);
    void define_native_javascript_backed_function(PropertyKey const&, GC::Ref<NativeJavaScriptBackedFunction> function, i32 length, PropertyAttributes attributes);

    virtual bool is_dom_node() const { return false; }
    virtual bool is_dom_document() const { return false; }
    virtual bool is_dom_element() const { return false; }
    virtual bool is_dom_event_target() const { return false; }
    virtual bool is_dom_event() const { return false; }
    virtual bool is_html_window() const { return false; }
    virtual bool is_html_window_proxy() const { return false; }
    virtual bool is_html_location() const { return false; }
    virtual bool is_canvas_rendering_context_2d() const { return false; }

    virtual bool is_function() const { return false; }
    virtual bool is_bound_function() const { return false; }
    virtual bool is_promise() const { return false; }
    virtual bool is_error_object() const { return false; }
    virtual bool is_date() const { return false; }
    virtual bool is_number_object() const { return false; }
    virtual bool is_boolean_object() const { return false; }
    virtual bool is_regexp_object() const { return false; }
    virtual bool is_bigint_object() const { return false; }
    virtual bool is_string_object() const { return false; }
    virtual bool is_array_buffer() const { return false; }
    virtual bool is_array_exotic_object() const { return false; }
    virtual bool is_global_object() const { return false; }
    virtual bool is_proxy_object() const { return false; }
    virtual bool is_native_function() const { return false; }
    virtual bool is_ecmascript_function_object() const { return false; }
    virtual bool is_array_iterator() const { return false; }
    virtual bool is_raw_json_object() const { return false; }
    virtual bool is_set_object() const { return false; }
    virtual bool is_map_object() const { return false; }
    virtual bool is_weak_map() const { return false; }

    virtual bool is_typed_array_base() const { return false; }
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    virtual bool is_##snake_name() const { return false; }
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    virtual bool eligible_for_own_property_enumeration_fast_path() const { return true; }

    virtual BuiltinIterator* as_builtin_iterator_if_next_is_not_redefined([[maybe_unused]] Value next_method) { return nullptr; }

    virtual bool is_array_iterator_prototype() const { return false; }
    virtual bool is_map_iterator_prototype() const { return false; }
    virtual bool is_set_iterator_prototype() const { return false; }
    virtual bool is_string_iterator_prototype() const { return false; }

    // B.3.7 The [[IsHTMLDDA]] Internal Slot, https://tc39.es/ecma262/#sec-IsHTMLDDA-internal-slot
    virtual bool is_htmldda() const { return false; }

    bool has_parameter_map() const { return m_has_parameter_map; }
    void set_has_parameter_map() { m_has_parameter_map = true; }

    virtual void visit_edges(Cell::Visitor&) override;

    Value get_direct(size_t index) const { return m_storage[index]; }
    void put_direct(size_t index, Value value) { m_storage[index] = value; }

    IndexedProperties const& indexed_properties() const { return m_indexed_properties; }
    IndexedProperties& indexed_properties() { return m_indexed_properties; }
    void set_indexed_property_elements(Vector<Value>&& values) { m_indexed_properties = IndexedProperties(move(values)); }

    Shape& shape() { return *m_shape; }
    Shape const& shape() const { return *m_shape; }
    void unsafe_set_shape(Shape&);

    void convert_to_prototype_if_needed();

    template<typename T>
    bool fast_is() const = delete;

    void set_prototype(Object*);

    [[nodiscard]] bool has_magical_length_property() const { return m_has_magical_length_property; }

    [[nodiscard]] bool is_typed_array() const { return m_is_typed_array; }
    void set_is_typed_array() { m_is_typed_array = true; }

    Object const* prototype() const { return shape().prototype(); }

protected:
    enum class GlobalObjectTag { Tag };
    enum class ConstructWithoutPrototypeTag { Tag };
    enum class ConstructWithPrototypeTag { Tag };

    Object(GlobalObjectTag, Realm&, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);
    Object(ConstructWithoutPrototypeTag, Realm&, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);
    Object(Realm&, Object* prototype, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);
    Object(ConstructWithPrototypeTag, Object& prototype, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);
    explicit Object(Shape&, MayInterfereWithIndexedPropertyAccess = MayInterfereWithIndexedPropertyAccess::No);

    // [[Extensible]]
    bool m_is_extensible : 1 { true };

    // [[ParameterMap]]
    bool m_has_parameter_map : 1 { false };

    bool m_has_magical_length_property : 1 { false };

    bool m_is_typed_array : 1 { false };

private:
    void set_shape(Shape& shape) { m_shape = &shape; }

    Object* prototype() { return shape().prototype(); }

    bool m_may_interfere_with_indexed_property_access : 1 { false };

    // True if this object has lazily allocated intrinsic properties.
    bool m_has_intrinsic_accessors : 1 { false };

    GC::Ptr<Shape> m_shape;
    Vector<Value> m_storage;
    IndexedProperties m_indexed_properties;
    OwnPtr<Vector<PrivateElement>> m_private_elements; // [[PrivateElements]]
};

#if !defined(AK_OS_WINDOWS)
static_assert(sizeof(Object) <= 64, "Keep the size of JS::Object down!");
#endif

}
