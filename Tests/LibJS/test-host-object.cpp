/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapBlock.h>
#include <LibGC/Root.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ErrorConstructor.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/HostArray.h>
#include <LibJS/Runtime/HostFunction.h>
#include <LibJS/Runtime/HostModule.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/ModuleEnvironment.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>

namespace {

class TestEnvironment {
public:
    TestEnvironment()
        : m_vm(JS::VM::create())
        , m_execution_context(JS::create_simple_execution_context<JS::GlobalObject>(*m_vm))
    {
    }

    ~TestEnvironment()
    {
        m_vm->pop_execution_context();
    }

    JS::VM& vm() { return *m_vm; }
    JS::Realm& realm() { return *m_execution_context->realm; }

    void define_global(StringView name, JS::Value value)
    {
        realm().global_object().define_direct_property(Utf16FlyString::from_utf8(name), value, JS::default_attributes);
    }

    // Returns the completion value as a string, or "uncaught <error>" for an exception that escaped the script.
    String evaluate(StringView source)
    {
        auto script = JS::Script::parse(source, realm());
        VERIFY(!script.is_error());
        auto result = m_vm->run(*script.value());
        if (result.is_error())
            return MUST(String::formatted("uncaught {}", result.error_value().to_utf16_string_without_side_effects()));
        return result.value().to_utf16_string_without_side_effects().to_utf8();
    }

    // Runs the statements in a function and returns "<name>: <message>" of what they throw, or "no exception". The
    // result comes from a return value, as a catch block's completion value can be wrong after an exception from a
    // nested call.
    String exception_from(StringView statements)
    {
        return evaluate(MUST(String::formatted("(() => {{ try {{ {}; }} catch (error) {{ return `${{error.name}}: ${{error.message}}`; }} return 'no exception'; }})()", statements)));
    }

private:
    NonnullRefPtr<JS::VM> m_vm;
    NonnullOwnPtr<JS::ExecutionContext> m_execution_context;
};

class TestHostData final : public GC::Cell {
    GC_CELL(TestHostData, GC::Cell);
    GC_DECLARE_ALLOCATOR(TestHostData);

public:
    u64 payload { 0 };
};

GC_DEFINE_ALLOCATOR(TestHostData);

class OtherTestHostData final : public GC::Cell {
    GC_CELL(OtherTestHostData, GC::Cell);
    GC_DECLARE_ALLOCATOR(OtherTestHostData);

public:
    u64 payload { 0 };
};

GC_DEFINE_ALLOCATOR(OtherTestHostData);

bool key_is(JS::PropertyKey const& property_key, StringView name)
{
    return property_key.is_string() && property_key.as_string() == Utf16FlyString::from_utf8(name);
}

JS::Completion hook_error(JS::Cell const& cell, StringView hook)
{
    return cell.vm().throw_completion<JS::TypeError>(Utf16String::formatted("{} threw", hook));
}

bool s_keyless_hooks_throw = false;
Optional<JS::PropertyDescriptor> s_last_defined_descriptor;
// Not given, given and empty, or given with a descriptor.
Optional<Optional<JS::PropertyDescriptor>> s_last_precomputed_get_own_property;
JS::Value s_last_intercepted_value;

// Implements every object hook. Hooks taking a key answer some keys themselves, throw for "throwing", and leave the
// rest to the ordinary internal method, the way bindings do.
struct InterceptingTraits {
    static JS::ThrowCompletionOr<JS::Object*> get_prototype_of(JS::HostObject const& object)
    {
        if (s_keyless_hooks_throw)
            return hook_error(object, "get_prototype_of"sv);
        return object.JS::Object::internal_get_prototype_of();
    }

    static JS::ThrowCompletionOr<bool> set_prototype_of(JS::HostObject& object, JS::Object* prototype)
    {
        if (s_keyless_hooks_throw)
            return hook_error(object, "set_prototype_of"sv);
        return object.JS::Object::internal_set_prototype_of(prototype);
    }

    static JS::ThrowCompletionOr<bool> is_extensible(JS::HostObject const& object)
    {
        if (s_keyless_hooks_throw)
            return hook_error(object, "is_extensible"sv);
        return object.JS::Object::internal_is_extensible();
    }

    static JS::ThrowCompletionOr<bool> prevent_extensions(JS::HostObject& object)
    {
        if (s_keyless_hooks_throw)
            return hook_error(object, "prevent_extensions"sv);
        return false;
    }

    static JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> get_own_property(JS::HostObject const& object, JS::PropertyKey const& property_key)
    {
        if (key_is(property_key, "throwing"sv))
            return hook_error(object, "get_own_property"sv);
        if (key_is(property_key, "virtual"sv)) {
            return JS::PropertyDescriptor {
                .value = JS::Value(JS::PrimitiveString::create(object.vm(), "virtual value"_utf16)),
                .writable = false,
                .enumerable = true,
                .configurable = true,
            };
        }
        return object.JS::Object::internal_get_own_property(property_key);
    }

    static JS::ThrowCompletionOr<bool> define_own_property(JS::HostObject& object, JS::PropertyKey const& property_key, JS::PropertyDescriptor& descriptor, Optional<JS::PropertyDescriptor>* precomputed_get_own_property)
    {
        if (key_is(property_key, "throwing"sv))
            return hook_error(object, "define_own_property"sv);
        if (key_is(property_key, "rejected"sv))
            return false;
        s_last_defined_descriptor = descriptor;
        s_last_precomputed_get_own_property.clear();
        if (precomputed_get_own_property)
            s_last_precomputed_get_own_property = *precomputed_get_own_property;
        return object.JS::Object::internal_define_own_property(property_key, descriptor, precomputed_get_own_property);
    }

    static JS::ThrowCompletionOr<bool> has_property(JS::HostObject const& object, JS::PropertyKey const& property_key)
    {
        if (key_is(property_key, "throwing"sv))
            return hook_error(object, "has_property"sv);
        if (key_is(property_key, "magic"sv))
            return true;
        return object.JS::Object::internal_has_property(property_key);
    }

    static JS::ThrowCompletionOr<JS::Value> get(JS::HostObject const& object, JS::PropertyKey const& property_key, JS::Value receiver, JS::CacheableGetPropertyMetadata* metadata, JS::Object::PropertyLookupPhase phase)
    {
        if (key_is(property_key, "throwing"sv))
            return hook_error(object, "get"sv);
        if (key_is(property_key, "answer"sv))
            return JS::Value(42);
        return object.JS::Object::internal_get(property_key, receiver, metadata, phase);
    }

    static JS::ThrowCompletionOr<bool> set(JS::HostObject& object, JS::PropertyKey const& property_key, JS::Value value, JS::Value receiver, JS::CacheableSetPropertyMetadata* metadata, JS::Object::PropertyLookupPhase phase)
    {
        if (key_is(property_key, "throwing"sv))
            return hook_error(object, "set"sv);
        if (key_is(property_key, "intercepted"sv)) {
            s_last_intercepted_value = value;
            return true;
        }
        return object.JS::Object::internal_set(property_key, value, receiver, metadata, phase);
    }

    static JS::ThrowCompletionOr<bool> delete_property(JS::HostObject& object, JS::PropertyKey const& property_key)
    {
        if (key_is(property_key, "throwing"sv))
            return hook_error(object, "delete_property"sv);
        if (key_is(property_key, "undeletable"sv))
            return false;
        return object.JS::Object::internal_delete(property_key);
    }

    static JS::ThrowCompletionOr<GC::RootVector<JS::Value>> own_property_keys(JS::HostObject const& object)
    {
        if (s_keyless_hooks_throw)
            return hook_error(object, "own_property_keys"sv);
        auto keys = TRY(object.JS::Object::internal_own_property_keys());
        keys.append(JS::PrimitiveString::create(object.vm(), "virtual"_utf16));
        return keys;
    }
};

constexpr JSHostObjectHooks intercepting_hooks = JS::make_host_object_hooks<InterceptingTraits>();

}

// Declared and defined apart, as an embedder's header and source file do.
extern JSHostClass const intercepting_host_class;
constexpr JSHostClass intercepting_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "InterceptingHostObject"sv, nullptr, &intercepting_hooks, nullptr, 0);

namespace {

size_t s_finalized_host_objects = 0;

struct CountingFinalizerTraits {
    static void finalize(JS::HostObject&) { ++s_finalized_host_objects; }
};

constexpr JSHostObjectHooks counting_finalizer_hooks = JS::make_host_object_hooks<CountingFinalizerTraits>();
constexpr JSHostClass counting_finalizer_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "CountingFinalizer"sv, nullptr, &counting_finalizer_hooks, nullptr, 0);

struct ErrorDataTraits {
    static JS::ErrorData* error_data(JS::HostObject& object)
    {
        return &static_cast<JS::Error&>(static_cast<JS::Object&>(*object.host_data()));
    }
};

constexpr JSHostObjectHooks error_data_hooks = JS::make_host_object_hooks<ErrorDataTraits>();
constexpr JSHostClass error_data_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "ErrorDataHostObject"sv, nullptr, &error_data_hooks, nullptr, 0);

bool s_inherited_property_is_cacheable = true;

struct InheritedCacheabilityTraits {
    static bool is_cacheable_for_inherited_property(JS::HostObject const&) { return s_inherited_property_is_cacheable; }
};

constexpr JSHostObjectHooks inherited_cacheability_hooks = JS::make_host_object_hooks<InheritedCacheabilityTraits>();
constexpr JSHostClass inherited_cacheability_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "InheritedCacheability"sv, nullptr, &inherited_cacheability_hooks, nullptr, 0);

// Only a [[Get]] hook, which doubles the numbers that the ordinary [[Get]] finds. It passes no cache metadata on, so that
// inline caches never answer for it.
struct NumberDoublingTraits {
    static JS::ThrowCompletionOr<JS::Value> get(JS::HostObject const& object, JS::PropertyKey const& property_key, JS::Value receiver, JS::CacheableGetPropertyMetadata*, JS::Object::PropertyLookupPhase phase)
    {
        auto value = TRY(object.JS::Object::internal_get(property_key, receiver, nullptr, phase));
        if (value.is_number())
            return JS::Value(value.as_double() * 2);
        return value;
    }
};

constexpr JSHostObjectHooks number_doubling_hooks = JS::make_host_object_hooks<NumberDoublingTraits>();
constexpr JSHostClass number_doubling_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "NumberDoubling"sv, nullptr, &number_doubling_hooks, nullptr, 0);

// Hooks written against HostObjectABI.h alone, the way a table not made by HostClassBuilder.h would be. The first leaves
// the descriptor it is given zeroed, and the second accepts every definition but zeroes the descriptor it writes back.
JSCompletion report_every_property_absent(JSObject*, JSPropertyKey, JSPropertyDescriptor*)
{
    return { .payload = 0, .variant = JS_COMPLETION_NORMAL };
}

JSCompletion accept_definition_and_zero_descriptor(JSObject*, JSPropertyKey, JSPropertyDescriptor* descriptor, JSPropertyDescriptor const*)
{
    *descriptor = {};
    return { .payload = 1, .variant = JS_COMPLETION_NORMAL };
}

constexpr JSHostObjectHooks hand_written_hooks = [] {
    JSHostObjectHooks hooks {};
    hooks.get_own_property = report_every_property_absent;
    hooks.define_own_property = accept_definition_and_zero_descriptor;
    return hooks;
}();
constexpr JSHostClass hand_written_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "HandWritten"sv, nullptr, &hand_written_hooks, nullptr, 0);

constexpr u32 all_object_flags = JS_HOST_CLASS_IS_PLATFORM_OBJECT
    | JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY
    | JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS
    | JS_HOST_CLASS_IS_HTMLDDA
    | JS_HOST_CLASS_IS_GLOBAL_OBJECT
    | JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE
    | JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH;

constexpr JSHostClass all_flags_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "AllFlags"sv, nullptr, nullptr, nullptr, all_object_flags);
constexpr JSHostClass no_flags_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "NoFlags"sv, nullptr, nullptr, nullptr, 0);
constexpr JSHostClass immutable_prototype_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "ImmutablePrototype"sv, nullptr, nullptr, nullptr, JS_HOST_CLASS_IMMUTABLE_PROTOTYPE);
constexpr JSHostClass base_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "Base"sv, nullptr, nullptr, nullptr, 0);
constexpr JSHostClass derived_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "Derived"sv, &base_class, nullptr, nullptr, 0);
constexpr JSHostClass allocator_a_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "AllocatorA"sv, nullptr, nullptr, nullptr, 0);
constexpr JSHostClass allocator_b_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "AllocatorB"sv, nullptr, nullptr, nullptr, 0);

GC::Ref<JS::HostObject> create_intercepting_object(TestEnvironment& environment)
{
    auto& realm = environment.realm();
    auto object = JS::HostObject::create(realm, intercepting_host_class, realm.intrinsics().object_prototype());
    environment.define_global("host"sv, object);
    return object;
}

NEVER_INLINE void allocate_unreachable_host_objects(JS::Realm& realm, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        (void)JS::HostObject::create(realm, counting_finalizer_class, nullptr);
}

NEVER_INLINE GC::Root<JS::HostObject> allocate_host_object_owning_its_cells(JS::Realm& realm, GC::Weak<TestHostData>& wrappable, GC::Weak<TestHostData>& host_data)
{
    auto new_wrappable = realm.heap().allocate<TestHostData>();
    auto new_host_data = realm.heap().allocate<TestHostData>();
    wrappable = new_wrappable;
    host_data = new_host_data;
    return GC::make_root(JS::HostObject::create(realm, no_flags_class, nullptr, new_wrappable, new_host_data));
}

NEVER_INLINE void scrub_stack()
{
    u8 volatile filler[8 * KiB];
    for (size_t i = 0; i < sizeof(filler); ++i)
        filler[i] = 0;
}

JS::ThrowCompletionOr<JS::Value> return_undefined(JS::VM&)
{
    return JS::js_undefined();
}

void expect_same_descriptor(JS::PropertyDescriptor const& actual, JS::PropertyDescriptor const& expected)
{
    EXPECT_EQ(actual.value.has_value(), expected.value.has_value());
    if (actual.value.has_value() && expected.value.has_value())
        EXPECT_EQ(actual.value->encoded(), expected.value->encoded());
    EXPECT(actual.get == expected.get);
    EXPECT(actual.set == expected.set);
    EXPECT_EQ(actual.writable, expected.writable);
    EXPECT_EQ(actual.enumerable, expected.enumerable);
    EXPECT_EQ(actual.configurable, expected.configurable);
    EXPECT_EQ(actual.property_offset, expected.property_offset);
}

}

TEST_CASE(object_hooks_answer_scripts)
{
    TestEnvironment environment;
    create_intercepting_object(environment);

    EXPECT_EQ(environment.evaluate("host.answer"sv), "42"sv);
    EXPECT_EQ(environment.evaluate("'magic' in host"sv), "true"sv);
    EXPECT_EQ(environment.evaluate("JSON.stringify(Object.getOwnPropertyDescriptor(host, 'virtual'))"sv),
        R"({"value":"virtual value","writable":false,"enumerable":true,"configurable":true})"sv);
    EXPECT_EQ(environment.evaluate("host.intercepted = 7; host.intercepted"sv), "undefined"sv);
    EXPECT_EQ(s_last_intercepted_value, JS::Value(7));
    EXPECT_EQ(environment.evaluate("host.undeletable = 1; delete host.undeletable"sv), "false"sv);
    EXPECT_EQ(environment.evaluate("Reflect.defineProperty(host, 'rejected', { value: 1 })"sv), "false"sv);
    EXPECT_EQ(environment.evaluate("Object.defineProperty(host, 'defined', { value: 1, enumerable: true }); host.defined"sv), "1"sv);
    EXPECT_EQ(environment.evaluate("host.expando = 2; delete host.expando"sv), "true"sv);
    EXPECT_EQ(environment.evaluate("Reflect.ownKeys(host).join()"sv), "undeletable,defined,virtual"sv);
    EXPECT_EQ(environment.evaluate("Reflect.preventExtensions(host)"sv), "false"sv);
    EXPECT_EQ(environment.evaluate("Object.isExtensible(host)"sv), "true"sv);
    EXPECT_EQ(environment.evaluate("Object.getPrototypeOf(host) === Object.prototype"sv), "true"sv);
    EXPECT_EQ(environment.evaluate("const proto = { inherited: 3 }; Reflect.setPrototypeOf(host, proto) && host.inherited"sv), "3"sv);
}

TEST_CASE(object_hooks_throw_into_scripts)
{
    TestEnvironment environment;
    create_intercepting_object(environment);

    EXPECT_EQ(environment.evaluate(R"(
        [
            () => host.throwing,
            () => "throwing" in host,
            () => Object.getOwnPropertyDescriptor(host, "throwing"),
            () => { host.throwing = 1; },
            () => delete host.throwing,
            () => Object.defineProperty(host, "throwing", { value: 1 }),
        ].map(operation => {
            try {
                operation();
                return "no exception";
            } catch (error) {
                return error.message;
            }
        }).join()
    )"sv),
        "get threw,has_property threw,get_own_property threw,set threw,delete_property threw,define_own_property threw"sv);

    s_keyless_hooks_throw = true;
    auto result = environment.evaluate(R"(
        [
            () => Object.getPrototypeOf(host),
            () => Reflect.setPrototypeOf(host, null),
            () => Reflect.isExtensible(host),
            () => Reflect.preventExtensions(host),
            () => Reflect.ownKeys(host),
        ].map(operation => {
            try {
                operation();
                return "no exception";
            } catch (error) {
                return error.message;
            }
        }).join()
    )"sv);
    s_keyless_hooks_throw = false;
    EXPECT_EQ(result, "get_prototype_of threw,set_prototype_of threw,is_extensible threw,prevent_extensions threw,own_property_keys threw"sv);
}

TEST_CASE(enumeration_goes_through_the_hooks)
{
    TestEnvironment environment;
    auto& realm = environment.realm();

    auto intercepting = create_intercepting_object(environment);
    EXPECT(!intercepting->eligible_for_own_property_enumeration_fast_path());
    EXPECT_EQ(environment.evaluate("host.plain = 1"sv), "1"sv);
    EXPECT_EQ(environment.evaluate("Object.keys(host).join()"sv), "plain,virtual"sv);
    EXPECT_EQ(environment.evaluate("(() => { const keys = []; for (const key in host) keys.push(key); return keys.join(); })()"sv), "plain,virtual"sv);
    EXPECT_EQ(environment.evaluate("JSON.stringify(host)"sv), R"({"plain":1,"virtual":"virtual value"})"sv);
    EXPECT_EQ(environment.evaluate("Object.keys(Object.assign({}, host)).join()"sv), "plain,virtual"sv);
    EXPECT_EQ(environment.evaluate("Object.keys({ ...host }).join()"sv), "plain,virtual"sv);

    auto doubling = JS::HostObject::create(realm, number_doubling_class, realm.intrinsics().object_prototype());
    EXPECT(!doubling->eligible_for_own_property_enumeration_fast_path());
    environment.define_global("doubling"sv, doubling);
    EXPECT_EQ(environment.evaluate("doubling.number = 2; doubling.number"sv), "4"sv);
    EXPECT_EQ(environment.evaluate("JSON.stringify(doubling)"sv), R"({"number":4})"sv);
    EXPECT_EQ(environment.evaluate("Object.values(doubling).join()"sv), "4"sv);
    EXPECT_EQ(environment.evaluate("Object.assign({}, doubling).number"sv), "4"sv);
    EXPECT_EQ(environment.evaluate("({ ...doubling }).number"sv), "4"sv);
}

TEST_CASE(hand_written_descriptor_hooks)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    environment.define_global("handWritten"sv, JS::HostObject::create(realm, hand_written_class, realm.intrinsics().object_prototype()));

    EXPECT_EQ(environment.evaluate("Reflect.defineProperty(handWritten, 'defined', { value: 1 })"sv), "true"sv);
    EXPECT_EQ(environment.evaluate("handWritten.assigned = 2"sv), "2"sv);
    EXPECT_EQ(environment.evaluate("Object.getOwnPropertyDescriptor(handWritten, 'defined')"sv), "undefined"sv);
    EXPECT_EQ(environment.evaluate("'assigned' in handWritten"sv), "false"sv);
    EXPECT_EQ(environment.evaluate("handWritten.toString === Object.prototype.toString"sv), "true"sv);
}

TEST_CASE(property_descriptors_round_trip_through_the_abi)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto getter = JS::NativeFunction::create(realm, return_undefined, 0);

    auto round_trip = [](JS::PropertyDescriptor const& descriptor) {
        return JS::HostABI::property_descriptor_from_abi(JS::HostABI::property_descriptor_to_abi(descriptor)).release_value();
    };

    JS::PropertyDescriptor data_descriptor {
        .value = JS::Value(1.5),
        .writable = true,
        .enumerable = false,
        .configurable = true,
        .property_offset = 5,
    };
    expect_same_descriptor(round_trip(data_descriptor), data_descriptor);
    EXPECT_EQ(JS::HostABI::property_descriptor_to_abi(data_descriptor).flags,
        JS_PD_PRESENT | JS_PD_HAS_VALUE | JS_PD_HAS_WRITABLE | JS_PD_WRITABLE | JS_PD_HAS_ENUMERABLE | JS_PD_HAS_CONFIGURABLE | JS_PD_CONFIGURABLE | JS_PD_HAS_PROPERTY_OFFSET);

    JS::PropertyDescriptor accessor_descriptor {
        .get = GC::Ptr<JS::FunctionObject> { getter },
        .set = GC::Ptr<JS::FunctionObject> {},
        .enumerable = true,
    };
    auto accessor_round_trip = round_trip(accessor_descriptor);
    expect_same_descriptor(accessor_round_trip, accessor_descriptor);
    EXPECT(accessor_round_trip.set.has_value());

    JS::PropertyDescriptor empty_descriptor;
    expect_same_descriptor(round_trip(empty_descriptor), empty_descriptor);

    EXPECT_EQ(JS::HostABI::property_descriptor_to_abi(Optional<JS::PropertyDescriptor> {}).flags, 0);
    EXPECT(!JS::HostABI::property_descriptor_from_abi(JSPropertyDescriptor {}).has_value());
}

TEST_CASE(property_offsets_and_cache_metadata_pass_through_hooks)
{
    TestEnvironment environment;
    auto object = create_intercepting_object(environment);
    auto key = JS::PropertyKey { "fresh"_utf16_fly_string };

    s_last_defined_descriptor.clear();
    Optional<u32> new_property_offset;
    EXPECT(MUST(object->create_data_property(key, JS::Value(3), &new_property_offset)));
    auto storage_entry = object->storage_get(key);
    VERIFY(storage_entry.has_value());
    EXPECT(new_property_offset.has_value());
    EXPECT_EQ(new_property_offset, storage_entry->property_offset);
    VERIFY(s_last_defined_descriptor.has_value());
    EXPECT_EQ(s_last_defined_descriptor->value->encoded(), JS::Value(3).encoded());
    EXPECT_EQ(s_last_defined_descriptor->writable, true);
    EXPECT(!s_last_precomputed_get_own_property.has_value());

    // An empty precomputed [[GetOwnProperty]] result must stay distinct from none at all.
    EXPECT_EQ(environment.evaluate("host.assigned = 1"sv), "1"sv);
    VERIFY(s_last_precomputed_get_own_property.has_value());
    EXPECT(!s_last_precomputed_get_own_property->has_value());

    Optional<JS::PropertyDescriptor> precomputed_get_own_property = MUST(object->internal_get_own_property(key));
    JS::PropertyDescriptor redefinition { .value = JS::Value(5) };
    EXPECT(MUST(object->internal_define_own_property(key, redefinition, &precomputed_get_own_property)));
    VERIFY(s_last_precomputed_get_own_property.has_value());
    VERIFY(s_last_precomputed_get_own_property->has_value());
    expect_same_descriptor(**s_last_precomputed_get_own_property, *precomputed_get_own_property);
    EXPECT_EQ(MUST(object->get(key)), JS::Value(5));
    MUST(object->set(key, JS::Value(3), JS::Object::ShouldThrowExceptions::Yes));

    auto own_descriptor = MUST(object->internal_get_own_property(key));
    VERIFY(own_descriptor.has_value());
    EXPECT_EQ(own_descriptor->property_offset, new_property_offset);

    JS::CacheableGetPropertyMetadata get_metadata;
    EXPECT_EQ(MUST(object->internal_get(key, object, &get_metadata)), JS::Value(3));
    EXPECT(get_metadata.type == JS::CacheableGetPropertyMetadata::Type::GetOwnProperty);
    EXPECT_EQ(get_metadata.property_offset, new_property_offset);

    JS::CacheableSetPropertyMetadata set_metadata;
    EXPECT(MUST(object->internal_set(key, JS::Value(4), object, &set_metadata)));
    EXPECT(set_metadata.type == JS::CacheableSetPropertyMetadata::Type::ChangeOwnProperty);
    EXPECT_EQ(set_metadata.property_offset, new_property_offset);
    EXPECT_EQ(MUST(object->get(key)), JS::Value(4));

    JS::CacheableGetPropertyMetadata answer_metadata;
    EXPECT_EQ(MUST(object->internal_get(JS::PropertyKey { "answer"_utf16_fly_string }, object, &answer_metadata)), JS::Value(42));
    EXPECT(answer_metadata.type == JS::CacheableGetPropertyMetadata::Type::NotCacheable);
}

TEST_CASE(table_flags_become_object_flags)
{
    TestEnvironment environment;
    auto& realm = environment.realm();

    auto flagged = JS::HostObject::create(realm, all_flags_class, realm.intrinsics().object_prototype());
    EXPECT(flagged->is_platform_object());
    EXPECT(flagged->requires_slow_add_own_property());
    EXPECT(flagged->may_interfere_with_indexed_property_access());
    EXPECT(flagged->is_htmldda());
    EXPECT(flagged->has_global_object_flag());
    EXPECT(!flagged->is_cacheable_for_property_absence());
    EXPECT(!flagged->eligible_for_own_property_enumeration_fast_path());

    auto plain = JS::HostObject::create(realm, no_flags_class, realm.intrinsics().object_prototype());
    EXPECT(!plain->is_platform_object());
    EXPECT(!plain->requires_slow_add_own_property());
    EXPECT(!plain->may_interfere_with_indexed_property_access());
    EXPECT(!plain->is_htmldda());
    EXPECT(!plain->has_global_object_flag());
    EXPECT(plain->is_cacheable_for_property_absence());
    EXPECT(plain->eligible_for_own_property_enumeration_fast_path());
    EXPECT(plain->extensible());

    environment.define_global("flagged"sv, flagged);
    EXPECT_EQ(environment.evaluate("typeof flagged"sv), "undefined"sv);
    EXPECT_EQ(environment.evaluate("flagged == null"sv), "true"sv);
}

TEST_CASE(immutable_prototype_flag)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    environment.define_global("immutable"sv, JS::HostObject::create(realm, immutable_prototype_class, realm.intrinsics().object_prototype()));

    EXPECT_EQ(environment.evaluate("Reflect.setPrototypeOf(immutable, {})"sv), "false"sv);
    EXPECT_EQ(environment.evaluate("Reflect.setPrototypeOf(immutable, Object.prototype)"sv), "true"sv);
    EXPECT(environment.exception_from("Object.setPrototypeOf(immutable, null)"sv).starts_with_bytes("TypeError: "sv));
    EXPECT_EQ(environment.evaluate("Object.getPrototypeOf(immutable) === Object.prototype"sv), "true"sv);
}

TEST_CASE(inherited_property_cacheability_comes_from_the_hook)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto key = JS::PropertyKey { "inherited"_utf16_fly_string };
    auto prototype = JS::Object::create(realm, realm.intrinsics().object_prototype());
    prototype->define_direct_property(key, JS::Value(1), JS::default_attributes);
    auto object = JS::HostObject::create(realm, inherited_cacheability_class, prototype);

    s_inherited_property_is_cacheable = true;
    JS::CacheableGetPropertyMetadata cacheable_metadata;
    EXPECT_EQ(MUST(object->internal_get(key, object, &cacheable_metadata)), JS::Value(1));
    EXPECT(cacheable_metadata.type == JS::CacheableGetPropertyMetadata::Type::GetPropertyInPrototypeChain);

    s_inherited_property_is_cacheable = false;
    JS::CacheableGetPropertyMetadata uncacheable_metadata;
    EXPECT_EQ(MUST(object->internal_get(key, object, &uncacheable_metadata)), JS::Value(1));
    EXPECT(uncacheable_metadata.type == JS::CacheableGetPropertyMetadata::Type::NotCacheable);
    s_inherited_property_is_cacheable = true;
}

TEST_CASE(error_data_hook)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto error = JS::Error::create(realm, "boom"_utf16);
    auto object = JS::HostObject::create(realm, error_data_class, realm.intrinsics().object_prototype(), nullptr, error);

    EXPECT(object->has_error_data());
    EXPECT_EQ(object->error_data(), static_cast<JS::ErrorData*>(error.ptr()));
    EXPECT(!JS::HostObject::create(realm, no_flags_class, nullptr)->has_error_data());

    environment.define_global("errorish"sv, object);
    EXPECT_EQ(environment.evaluate("Object.prototype.toString.call(errorish)"sv), "[object Error]"sv);
}

TEST_CASE(finalize_hook_runs_for_collected_objects)
{
    TestEnvironment environment;
    environment.vm().heap().set_incremental_sweep_enabled(false);

    s_finalized_host_objects = 0;
    allocate_unreachable_host_objects(environment.realm(), 32);
    scrub_stack();
    environment.vm().heap().collect_garbage();
    EXPECT(s_finalized_host_objects > 0);
}

TEST_CASE(host_objects_keep_their_cells_alive)
{
    TestEnvironment environment;
    environment.vm().heap().set_incremental_sweep_enabled(false);

    GC::Weak<TestHostData> wrappable;
    GC::Weak<TestHostData> host_data;
    auto object = allocate_host_object_owning_its_cells(environment.realm(), wrappable, host_data);
    scrub_stack();
    environment.vm().heap().collect_garbage();

    EXPECT(wrappable.ptr());
    EXPECT(host_data.ptr());
    EXPECT_EQ(object->wrappable().ptr(), wrappable.ptr().ptr());
    EXPECT_EQ(object->host_data().ptr(), host_data.ptr().ptr());
}

// JS::Object has a different size under the Windows ABI, so a host object's fields are not at the offsets the ABI
// header gives there.
#if !defined(AK_OS_WINDOWS)
TEST_CASE(host_object_layout)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto wrappable = realm.heap().allocate<TestHostData>();
    auto host_data = realm.heap().allocate<TestHostData>();
    auto object = JS::HostObject::create(realm, no_flags_class, nullptr, wrappable, host_data);

    static_assert(JS::HostObject::wrappable_offset() == JS_HOST_OBJECT_WRAPPABLE_OFFSET);
    auto const* bytes = reinterpret_cast<u8 const*>(object.ptr());
    EXPECT_EQ(*reinterpret_cast<JSHostClass const* const*>(bytes + JS_HOST_OBJECT_HOST_CLASS_OFFSET), &no_flags_class);
    EXPECT_EQ(*reinterpret_cast<GC::Cell* const*>(bytes + JS_HOST_OBJECT_WRAPPABLE_OFFSET), static_cast<GC::Cell*>(wrappable.ptr()));
    EXPECT_EQ(*reinterpret_cast<GC::Cell* const*>(bytes + JS_HOST_OBJECT_HOST_DATA_OFFSET), static_cast<GC::Cell*>(host_data.ptr()));
    EXPECT_EQ(sizeof(JS::HostObject), static_cast<size_t>(JS_HOST_OBJECT_SIZE));
}
#endif

TEST_CASE(host_class_identity)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto base = JS::HostObject::create(realm, base_class, nullptr);
    auto derived = JS::HostObject::create(realm, derived_class, nullptr);
    auto ordinary = JS::Object::create(realm, nullptr);

    EXPECT_EQ(base->class_name(), "Base"sv);
    EXPECT_EQ(derived->class_name(), "Derived"sv);
    EXPECT_EQ(JS::host_class_of(*derived), &derived_class);
    EXPECT_EQ(JS::host_class_of(*ordinary), nullptr);
    EXPECT(&derived->host_class() == &derived_class);

    EXPECT(JS::is_host_instance_of(*derived, derived_class));
    EXPECT(JS::is_host_instance_of(*derived, base_class));
    EXPECT(!JS::is_host_instance_of(*base, derived_class));
    EXPECT(!JS::is_host_instance_of(*ordinary, base_class));

    EXPECT(is<JS::HostObject>(static_cast<JS::Object&>(*derived)));
    EXPECT(!is<JS::HostObject>(*ordinary));
}

TEST_CASE(host_data_is_identified_by_its_type)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto host_data = realm.heap().allocate<TestHostData>();
    auto object = JS::HostObject::create(realm, no_flags_class, nullptr, nullptr, host_data);

    EXPECT_EQ(JS::host_data_if<TestHostData>(*object), host_data.ptr());
    EXPECT_EQ(JS::host_data_if<OtherTestHostData>(*object), nullptr);
    EXPECT_EQ(JS::host_data_if<TestHostData>(*JS::HostObject::create(realm, no_flags_class, nullptr)), nullptr);
    EXPECT_EQ(JS::host_data_if<TestHostData>(*JS::Object::create(realm, nullptr)), nullptr);

    auto other_host_data = realm.heap().allocate<OtherTestHostData>();
    object->set_host_data(other_host_data);
    EXPECT_EQ(JS::host_data_if<TestHostData>(*object), nullptr);
    EXPECT_EQ(JS::host_data_if<OtherTestHostData>(*object), other_host_data.ptr());
}

TEST_CASE(each_host_class_has_its_own_allocator)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto first_a = JS::HostObject::create(realm, allocator_a_class, nullptr);
    auto second_a = JS::HostObject::create(realm, allocator_a_class, nullptr);
    auto b = JS::HostObject::create(realm, allocator_b_class, nullptr);
    auto ordinary = JS::Object::create(realm, nullptr);

    auto allocator_of = [](JS::Object const& object) -> GC::CellAllocator& {
        return GC::HeapBlock::from_cell(&object)->cell_allocator();
    };
    EXPECT_EQ(&allocator_of(*first_a), &allocator_of(*second_a));
    EXPECT_NE(&allocator_of(*first_a), &allocator_of(*b));
    EXPECT_NE(&allocator_of(*first_a), &allocator_of(*ordinary));
    EXPECT_EQ(allocator_of(*first_a).class_name(), "AllocatorA"sv);
    EXPECT_EQ(allocator_of(*b).class_name(), "AllocatorB"sv);
}

TEST_CASE(get_as_prototype_of_passes_on_only_cacheable_prototype_chain_hits)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto own_key = JS::PropertyKey { "own"_utf16_fly_string };
    auto inherited_key = JS::PropertyKey { "inherited"_utf16_fly_string };
    auto missing_key = JS::PropertyKey { "missing"_utf16_fly_string };

    auto prototype = JS::Object::create(realm, realm.intrinsics().object_prototype());
    prototype->define_direct_property(inherited_key, JS::Value(1), JS::default_attributes);
    auto holder = JS::Object::create(realm, prototype);
    holder->define_direct_property(own_key, JS::Value(2), JS::default_attributes);
    holder->convert_to_prototype_if_needed();
    auto receiver = JS::Object::create(realm, nullptr);

    JS::CacheableGetPropertyMetadata own_metadata;
    EXPECT_EQ(MUST(holder->internal_get_as_prototype_of(own_key, receiver, &own_metadata)), JS::Value(2));
    EXPECT(own_metadata.type == JS::CacheableGetPropertyMetadata::Type::GetPropertyInPrototypeChain);
    EXPECT_EQ(own_metadata.prototype.ptr(), holder.ptr());
    EXPECT_EQ(own_metadata.property_offset, holder->storage_get(own_key)->property_offset);

    JS::CacheableGetPropertyMetadata inherited_metadata;
    EXPECT_EQ(MUST(holder->internal_get_as_prototype_of(inherited_key, receiver, &inherited_metadata)), JS::Value(1));
    EXPECT(inherited_metadata.type == JS::CacheableGetPropertyMetadata::Type::GetPropertyInPrototypeChain);
    EXPECT_EQ(inherited_metadata.prototype.ptr(), prototype.ptr());

    // A miss is cacheable on its own terms, but not as a hit to pass on, so the caller's metadata stays as it was.
    JS::CacheableGetPropertyMetadata untouched_metadata {
        .type = JS::CacheableGetPropertyMetadata::Type::GetOwnProperty,
        .property_offset = 7,
        .prototype = nullptr,
    };
    EXPECT_EQ(MUST(holder->internal_get_as_prototype_of(missing_key, receiver, &untouched_metadata)), JS::js_undefined());
    EXPECT(untouched_metadata.type == JS::CacheableGetPropertyMetadata::Type::GetOwnProperty);
    EXPECT_EQ(untouched_metadata.property_offset, 7u);

    // So does a value that a host hook produced without filling the metadata.
    auto host = create_intercepting_object(environment);
    JS::CacheableGetPropertyMetadata host_metadata;
    EXPECT_EQ(MUST(host->internal_get_as_prototype_of(JS::PropertyKey { "answer"_utf16_fly_string }, receiver, &host_metadata)), JS::Value(42));
    EXPECT(host_metadata.type == JS::CacheableGetPropertyMetadata::Type::NotCacheable);

    EXPECT_EQ(MUST(holder->internal_get_as_prototype_of(own_key, receiver, nullptr)), JS::Value(2));
}

namespace {

size_t s_finalized_host_functions = 0;

// Sums its arguments, and throws when there are none.
struct AdderTraits {
    static JS::ThrowCompletionOr<JS::Value> call(JS::HostFunction& function, JS::VM& vm)
    {
        if (vm.argument_count() == 0)
            return hook_error(function, "call"sv);
        double sum = 0;
        for (size_t i = 0; i < vm.argument_count(); ++i)
            sum += TRY(vm.argument(i).to_double(vm));
        return JS::Value(sum);
    }

    static void finalize(JS::HostFunction&) { ++s_finalized_host_functions; }
};

// Makes { value } objects from new.target's prototype, and throws without an argument.
struct MakerTraits {
    static JS::ThrowCompletionOr<JS::Value> call(JS::HostFunction& function, JS::VM&)
    {
        return hook_error(function, "call"sv);
    }

    static JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct(JS::HostFunction& function, JS::VM& vm, JS::FunctionObject& new_target)
    {
        if (vm.argument_count() == 0)
            return hook_error(function, "construct"sv);
        auto& realm = *vm.current_realm();
        auto prototype = TRY(new_target.get(vm.names.prototype));
        auto object = JS::Object::create(realm, prototype.is_object() ? &prototype.as_object() : realm.intrinsics().object_prototype().ptr());
        object->define_direct_property("value"_utf16_fly_string, vm.argument(0), JS::default_attributes);
        return object;
    }
};

constexpr JSHostFunctionHooks adder_hooks = JS::make_host_function_hooks<AdderTraits>();
constexpr JSHostClass adder_class = JS::make_host_class(JS_HOST_CLASS_FUNCTION, "Adder"sv, nullptr, &adder_hooks, nullptr, 0);
constexpr JSHostFunctionHooks maker_hooks = JS::make_host_function_hooks<MakerTraits>();
constexpr JSHostClass maker_class = JS::make_host_class(JS_HOST_CLASS_FUNCTION, "Maker"sv, nullptr, &maker_hooks, nullptr, JS_HOST_CLASS_HAS_CONSTRUCTOR);

Optional<JS::Value> s_last_deleted_element;

// Doubles numbers stored at indices and rejects negative ones, then lets the array store them.
struct DoublingArrayTraits {
    static JS::ThrowCompletionOr<bool> set(JS::HostArray& array, JS::PropertyKey const& property_key, JS::Value value, JS::Value receiver, JS::CacheableSetPropertyMetadata* metadata, JS::Object::PropertyLookupPhase phase)
    {
        if (property_key.is_number() && value.is_number()) {
            if (value.as_double() < 0)
                return hook_error(array, "set"sv);
            value = JS::Value(value.as_double() * 2);
        }
        return array.array_set(property_key, value, receiver, metadata, phase);
    }

    static JS::ThrowCompletionOr<bool> delete_property(JS::HostArray& array, JS::PropertyKey const& property_key)
    {
        if (key_is(property_key, "throwing"sv))
            return hook_error(array, "delete_property"sv);
        if (property_key.is_number()) {
            if (auto element = array.indexed_get(property_key.as_number()); element.has_value())
                s_last_deleted_element = element->value;
        }
        return array.array_delete(property_key);
    }
};

constexpr JSHostArrayHooks doubling_array_hooks = JS::make_host_array_hooks<DoublingArrayTraits>();
constexpr JSHostClass doubling_array_class = JS::make_host_class(JS_HOST_CLASS_ARRAY, "DoublingArray"sv, nullptr, &doubling_array_hooks, nullptr, JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS);
constexpr JSHostClass hookless_array_class = JS::make_host_class(JS_HOST_CLASS_ARRAY, "HooklessArray"sv, nullptr, nullptr, nullptr, 0);

bool s_module_environment_throws = false;
bool s_module_execution_throws = false;

Vector<Utf16FlyString> exported_names_of_test_module()
{
    return { "answer"_utf16_fly_string, Utf16FlyString::from_utf8("名前"sv) };
}

// Exports two bindings and fills them in when executed, like a WebAssembly module record.
struct ExportingModuleTraits {
    static Vector<Utf16FlyString> get_exported_names(JS::HostModule&)
    {
        return exported_names_of_test_module();
    }

    static JS::ResolvedBinding resolve_export(JS::HostModule& module, Utf16FlyString const& export_name)
    {
        if (exported_names_of_test_module().contains_slow(export_name))
            return JS::ResolvedBinding { JS::ResolvedBinding::Type::BindingName, &module, export_name };
        return JS::ResolvedBinding::null();
    }

    static JS::ThrowCompletionOr<void> initialize_environment(JS::HostModule& module)
    {
        if (s_module_environment_throws)
            return hook_error(module, "initialize_environment"sv);
        auto& vm = module.vm();
        auto environment = vm.heap().allocate<JS::ModuleEnvironment>(nullptr);
        module.set_environment(environment);
        for (auto const& name : exported_names_of_test_module())
            MUST(environment->create_immutable_binding(vm, name, true));
        return {};
    }

    static JS::ThrowCompletionOr<void> execute_module(JS::HostModule& module, GC::Ptr<JS::PromiseCapability> capability)
    {
        VERIFY(!capability);
        if (s_module_execution_throws)
            return hook_error(module, "execute_module"sv);
        auto& vm = module.vm();
        auto names = exported_names_of_test_module();
        MUST(module.environment()->initialize_binding(vm, names[0], JS::Value(42), JS::Environment::InitializeBindingHint::Normal));
        MUST(module.environment()->initialize_binding(vm, names[1], JS::PrimitiveString::create(vm, "value"_utf16), JS::Environment::InitializeBindingHint::Normal));
        return {};
    }
};

constexpr JSHostModuleHooks exporting_module_hooks = JS::make_host_module_hooks<ExportingModuleTraits>();
constexpr JSHostClass exporting_module_class = JS::make_host_class(JS_HOST_CLASS_MODULE, "ExportingModule"sv, nullptr, &exporting_module_hooks, nullptr, 0);

NEVER_INLINE void allocate_unreachable_host_functions(JS::Realm& realm, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        (void)JS::HostFunction::create(realm, adder_class, "add"_utf16_fly_string, 2);
}

String message_of(JS::Value error)
{
    return error.as_object().get_without_side_effects("message"_utf16_fly_string).to_utf16_string_without_side_effects().to_utf8();
}

}

TEST_CASE(host_function_calls)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto host_data = realm.heap().allocate<TestHostData>();
    auto adder = JS::HostFunction::create(realm, adder_class, "add"_utf16_fly_string, 2, nullptr, host_data);
    environment.define_global("add"sv, adder);

    EXPECT_EQ(environment.evaluate("add(1, 2, 3)"sv), "6"sv);
    EXPECT_EQ(environment.evaluate("typeof add"sv), "function"sv);
    EXPECT_EQ(environment.evaluate("Object.getOwnPropertyNames(add).join()"sv), "length,name"sv);
    EXPECT_EQ(environment.evaluate("add.name + add.length"sv), "add2"sv);
    EXPECT_EQ(environment.evaluate("Object.getPrototypeOf(add) === Function.prototype"sv), "true"sv);
    EXPECT_EQ(environment.exception_from("add()"sv), "TypeError: call threw"sv);
    EXPECT(environment.exception_from("new add(1)"sv).starts_with_bytes("TypeError: "sv));

    EXPECT_EQ(adder->class_name(), "Adder"sv);
    EXPECT_EQ(adder->name(), "add"_utf16_fly_string);
    EXPECT(!adder->has_constructor());
    EXPECT_EQ(adder->realm(), &realm);
    EXPECT_EQ(JS::host_class_of(*adder), &adder_class);
    EXPECT(is<JS::HostFunction>(static_cast<JS::Object&>(*adder)));
    EXPECT(!is<JS::HostObject>(static_cast<JS::Object&>(*adder)));
    EXPECT_EQ(JS::host_data_if<TestHostData>(*adder), host_data.ptr());
}

TEST_CASE(host_function_constructs)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto& vm = environment.vm();
    auto maker = JS::HostFunction::create(realm, maker_class, "Maker"_utf16_fly_string, 1);
    maker->define_direct_property(vm.names.prototype, JS::Object::create(realm, realm.intrinsics().object_prototype()), 0);
    environment.define_global("Maker"sv, maker);

    EXPECT(maker->has_constructor());
    EXPECT_EQ(environment.evaluate("const made = new Maker(5); made.value + ' ' + (made instanceof Maker)"sv), "5 true"sv);
    EXPECT_EQ(environment.evaluate("class Derived extends Maker {} const derived = new Derived(3); derived.value + ' ' + (derived instanceof Derived)"sv), "3 true"sv);
    EXPECT_EQ(environment.exception_from("new Maker()"sv), "TypeError: construct threw"sv);
    EXPECT_EQ(environment.exception_from("Maker(1)"sv), "TypeError: call threw"sv);
}

TEST_CASE(host_function_without_own_properties)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto& vm = environment.vm();
    auto function = JS::HostFunction::create_without_own_properties(realm, adder_class, "bare"_utf16_fly_string, realm.intrinsics().error_constructor());
    environment.define_global("bare"sv, function);

    EXPECT_EQ(environment.evaluate("Object.getOwnPropertyNames(bare).length"sv), "0"sv);
    EXPECT_EQ(environment.evaluate("Object.getPrototypeOf(bare) === Error"sv), "true"sv);

    function->define_direct_property(vm.names.prototype, JS::Object::create(realm, nullptr), 0);
    function->define_direct_property(vm.names.name, JS::PrimitiveString::create(vm, "bare"_utf16), JS::Attribute::Configurable);
    function->define_direct_property(vm.names.length, JS::Value(1), JS::Attribute::Configurable);
    EXPECT_EQ(environment.evaluate("Object.getOwnPropertyNames(bare).join()"sv), "prototype,name,length"sv);
}

TEST_CASE(host_function_finalize_hook)
{
    TestEnvironment environment;
    environment.vm().heap().set_incremental_sweep_enabled(false);

    s_finalized_host_functions = 0;
    allocate_unreachable_host_functions(environment.realm(), 32);
    scrub_stack();
    environment.vm().heap().collect_garbage();
    EXPECT(s_finalized_host_functions > 0);
}

TEST_CASE(host_array_hooks)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto host_data = realm.heap().allocate<TestHostData>();
    auto array = JS::HostArray::create(realm, doubling_array_class, nullptr, host_data);
    environment.define_global("doubling"sv, array);

    EXPECT_EQ(environment.evaluate("doubling[0] = 2; doubling[1] = 5; doubling.push(7); doubling.join()"sv), "4,10,14"sv);
    EXPECT_EQ(environment.evaluate("doubling.length + ' ' + Array.isArray(doubling)"sv), "3 true"sv);
    EXPECT_EQ(environment.evaluate("Object.getPrototypeOf(doubling) === Array.prototype"sv), "true"sv);

    s_last_deleted_element.clear();
    EXPECT_EQ(environment.evaluate("delete doubling[1]"sv), "true"sv);
    EXPECT_EQ(s_last_deleted_element, JS::Value(10));
    EXPECT_EQ(environment.evaluate("doubling.length + ' ' + (1 in doubling)"sv), "3 false"sv);

    EXPECT_EQ(environment.exception_from("doubling[0] = -1"sv), "TypeError: set threw"sv);
    EXPECT_EQ(environment.exception_from("delete doubling.throwing"sv), "TypeError: delete_property threw"sv);
    EXPECT_EQ(environment.evaluate("doubling[0]"sv), "4"sv);

    EXPECT(array->may_interfere_with_indexed_property_access());
    EXPECT_EQ(array->class_name(), "DoublingArray"sv);
    EXPECT_EQ(JS::host_class_of(*array), &doubling_array_class);
    EXPECT(is<JS::HostArray>(static_cast<JS::Object&>(*array)));
    EXPECT_EQ(JS::host_data_if<TestHostData>(*array), host_data.ptr());
}

TEST_CASE(host_array_without_hooks_is_an_array)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto array = JS::HostArray::create(realm, hookless_array_class);
    environment.define_global("hookless"sv, array);

    EXPECT_EQ(environment.evaluate("hookless.push(1, 2); hookless[5] = 3; delete hookless[0]; hookless.length + ' ' + JSON.stringify(hookless)"sv), "6 [null,2,null,null,null,3]"sv);
    EXPECT(!array->may_interfere_with_indexed_property_access());
}

TEST_CASE(host_module_links_and_evaluates)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto& vm = environment.vm();
    auto host_data = realm.heap().allocate<TestHostData>();
    auto module = JS::HostModule::create(realm, exporting_module_class, "exporting.wasm"sv, {}, nullptr, host_data);

    EXPECT_EQ(module->class_name(), "ExportingModule"sv);
    EXPECT_EQ(JS::host_class_of(static_cast<JS::Module const&>(*module)), &exporting_module_class);
    EXPECT(JS::is_host_instance_of(*module, exporting_module_class));
    EXPECT_EQ(JS::host_data_if<TestHostData>(*module), host_data.ptr());
    EXPECT_EQ(JS::host_data_if<OtherTestHostData>(*module), nullptr);

    EXPECT_EQ(module->get_exported_names(vm), exported_names_of_test_module());

    auto non_ascii_name = exported_names_of_test_module()[1];
    auto binding = module->resolve_export(vm, non_ascii_name);
    EXPECT(binding.type == JS::ResolvedBinding::BindingName);
    EXPECT_EQ(binding.module.ptr(), static_cast<JS::Module*>(module.ptr()));
    EXPECT_EQ(binding.export_name, non_ascii_name);
    EXPECT(module->resolve_export(vm, "missing"_utf16_fly_string).type == JS::ResolvedBinding::Null);

    module->load_requested_modules({});
    MUST(module->link(vm));
    auto evaluation = MUST(module->evaluate(vm));
    EXPECT(static_cast<JS::Promise&>(*evaluation->promise()).state() == JS::Promise::State::Fulfilled);

    environment.define_global("namespaceObject"sv, module->get_module_namespace(vm));
    EXPECT_EQ(environment.evaluate("Object.keys(namespaceObject).length + ' ' + namespaceObject.answer"sv), "2 42"sv);
    EXPECT_EQ(environment.evaluate("namespaceObject[Object.keys(namespaceObject)[1]]"sv), "value"sv);
    EXPECT_EQ(environment.evaluate("Object.keys(namespaceObject)[1].charCodeAt(0)"sv), "21517"sv);
}

TEST_CASE(host_module_hooks_throw)
{
    TestEnvironment environment;
    auto& realm = environment.realm();
    auto& vm = environment.vm();

    auto unlinkable = JS::HostModule::create(realm, exporting_module_class, "unlinkable.wasm"sv, {});
    unlinkable->load_requested_modules({});
    s_module_environment_throws = true;
    auto link_result = unlinkable->link(vm);
    s_module_environment_throws = false;
    VERIFY(link_result.is_error());
    EXPECT_EQ(message_of(link_result.error_value()), "initialize_environment threw"sv);

    auto failing = JS::HostModule::create(realm, exporting_module_class, "failing.wasm"sv, {});
    failing->load_requested_modules({});
    MUST(failing->link(vm));
    s_module_execution_throws = true;
    auto evaluation = MUST(failing->evaluate(vm));
    s_module_execution_throws = false;
    auto& promise = static_cast<JS::Promise&>(*evaluation->promise());
    VERIFY(promise.state() == JS::Promise::State::Rejected);
    EXPECT_EQ(message_of(promise.result()), "execute_module threw"sv);
}
