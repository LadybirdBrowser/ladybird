/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace {

class TestWrapperObject;

class TestWrappable final : public Web::Bindings::Wrappable {
    WEB_NON_IDL_WRAPPABLE(TestWrappable, Web::Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TestWrappable);

public:
    explicit TestWrappable(JS::Realm&)
        : Web::Bindings::Wrappable()
    {
    }

    virtual Web::Bindings::InterfaceName interface_name() const override { return Web::Bindings::InterfaceName::EventTarget; }
    virtual bool implements_interface(String const& interface) const override
    {
        if (interface == "TestWrappable"_string)
            return true;
        return Base::implements_interface(interface);
    }

    void set_indexed_value(GC::Ref<TestWrappable> indexed_value) { m_indexed_value = indexed_value; }
    void set_named_value(GC::Ref<TestWrappable> named_value) { m_named_value = named_value; }
    void set_origin(URL::Origin const& origin) { m_origin = origin; }
    void record_setter_realm(JS::Realm& realm) { m_last_setter_realm = &realm; }
    JS::Realm* last_setter_realm() const { return m_last_setter_realm.ptr(); }

    virtual Optional<URL::Origin> extract_an_origin() const override { return m_origin; }

    virtual Vector<FlyString> supported_property_names() const override
    {
        if (!m_named_value)
            return {};
        return { "child"_fly_string };
    }

    GC::Ptr<TestWrappable> indexed_value() const { return m_indexed_value; }
    GC::Ptr<TestWrappable> named_value() const { return m_named_value; }

protected:
    virtual GC::Ref<Web::Bindings::PlatformObject> create_wrapper(JS::Realm&) override;

    virtual void visit_edges(GC::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_indexed_value);
        visitor.visit(m_named_value);
        visitor.visit(m_last_setter_realm);
    }

private:
    GC::Ptr<TestWrappable> m_indexed_value;
    GC::Ptr<TestWrappable> m_named_value;
    GC::Ptr<JS::Realm> m_last_setter_realm;
    Optional<URL::Origin> m_origin;
};

class TestWrapperObject final : public Web::Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(TestWrapperObject, Web::Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TestWrapperObject);

public:
    using PlatformObject::set_value_of_named_property;
    using PlatformObject::set_value_of_new_indexed_property;

    TestWrapperObject(JS::Realm& realm, GC::Ref<Web::Bindings::Wrappable> impl)
        : PlatformObject(realm)
        , m_impl(impl)
    {
        m_legacy_platform_object_flags = LegacyPlatformObjectFlags {};
        m_legacy_platform_object_flags->supports_indexed_properties = true;
        m_legacy_platform_object_flags->supports_named_properties = true;
    }

    virtual Web::WebIDL::ExceptionOr<void> set_value_of_named_property(JS::Realm& realm, String const&, JS::Value) override
    {
        static_cast<TestWrappable&>(*m_impl).record_setter_realm(realm);
        return {};
    }

    virtual Web::WebIDL::ExceptionOr<void> set_value_of_new_indexed_property(JS::Realm& realm, u32, JS::Value) override
    {
        static_cast<TestWrappable&>(*m_impl).record_setter_realm(realm);
        return {};
    }

protected:
    virtual Web::Bindings::Wrappable* wrappable_impl() override { return m_impl.ptr(); }
    virtual Web::Bindings::Wrappable const* wrappable_impl() const override { return m_impl.ptr(); }

    virtual Optional<JS::Value> item_value(Web::Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const override
    {
        auto const& impl = static_cast<TestWrappable const&>(*m_impl);
        if (index != 0 || !impl.indexed_value())
            return {};
        return Web::Bindings::wrap(wrapper_world, realm, GC::Ref { *impl.indexed_value() }).ptr();
    }

    virtual JS::Value named_item_value(Web::Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, FlyString const& name) const override
    {
        auto const& impl = static_cast<TestWrappable const&>(*m_impl);
        if (name != "child"_fly_string || !impl.named_value())
            return JS::js_undefined();
        return Web::Bindings::wrap(wrapper_world, realm, GC::Ref { *impl.named_value() }).ptr();
    }

private:
    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_impl);
    }

    GC::Ref<Web::Bindings::Wrappable> m_impl;
};

#define EXPECT_NOT_CONSTRUCTIBLE_FROM_WRAPPABLE(Target)              \
    static_assert(!IsConstructible<Target, TestWrappable*>);         \
    static_assert(!IsConstructible<Target, TestWrappable const*>);   \
    static_assert(!IsConstructible<Target, GC::Ptr<TestWrappable>>); \
    static_assert(!IsConstructible<Target, GC::Ref<TestWrappable>>); \
    static_assert(!IsConstructible<Target, GC::Root<TestWrappable> const&>)

#define EXPECT_CONSTRUCTIBLE_FROM_WRAPPER(Target)               \
    static_assert(IsConstructible<Target, TestWrapperObject*>); \
    static_assert(IsConstructible<Target, GC::Ref<TestWrapperObject>>)

EXPECT_NOT_CONSTRUCTIBLE_FROM_WRAPPABLE(JS::Value);
EXPECT_CONSTRUCTIBLE_FROM_WRAPPER(JS::Value);
EXPECT_NOT_CONSTRUCTIBLE_FROM_WRAPPABLE(JS::Completion);
EXPECT_NOT_CONSTRUCTIBLE_FROM_WRAPPABLE(JS::ThrowCompletionOr<JS::Value>);
EXPECT_CONSTRUCTIBLE_FROM_WRAPPER(JS::ThrowCompletionOr<JS::Value>);
EXPECT_NOT_CONSTRUCTIBLE_FROM_WRAPPABLE(Web::WebIDL::ExceptionOr<JS::Value>);
EXPECT_CONSTRUCTIBLE_FROM_WRAPPER(Web::WebIDL::ExceptionOr<JS::Value>);

#undef EXPECT_CONSTRUCTIBLE_FROM_WRAPPER
#undef EXPECT_NOT_CONSTRUCTIBLE_FROM_WRAPPABLE

GC_DEFINE_ALLOCATOR(TestWrappable);
GC_DEFINE_ALLOCATOR(TestWrapperObject);

GC::Ref<Web::Bindings::PlatformObject> TestWrappable::create_wrapper(JS::Realm& realm)
{
    return realm.create<TestWrapperObject>(realm, *this);
}

GC::Ref<Web::Bindings::PlatformObject> wrap_test_wrappable(JS::Realm& realm, GC::Ref<TestWrappable> wrappable)
{
    return Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(realm), realm, wrappable);
}

GC::Ptr<Web::Bindings::PlatformObject> cached_wrapper_for(JS::Realm& realm, Web::Bindings::Wrappable const& wrappable)
{
    return Web::Bindings::host_defined_wrapper_world(realm).wrapper_for(wrappable, realm);
}

void install_test_host_defined(JS::Realm& realm, Web::Bindings::WrapperWorld::Type wrapper_world_type)
{
    auto intrinsics = realm.create<Web::Bindings::Intrinsics>(realm);
    auto wrapper_world = realm.heap().allocate<Web::Bindings::WrapperWorld>(wrapper_world_type);
    realm.set_host_defined(make<Web::Bindings::HostDefined>(intrinsics, *wrapper_world));
}

struct TestRealm {
    explicit TestRealm(JS::VM& vm, Web::Bindings::WrapperWorld::Type wrapper_world_type = Web::Bindings::WrapperWorld::Type::Main)
        : execution_context(MUST(JS::Realm::initialize_host_defined_realm(vm, nullptr, nullptr)))
    {
        install_test_host_defined(realm(), wrapper_world_type);
    }

    ~TestRealm()
    {
        realm().vm().pop_execution_context();
    }

    JS::Realm& realm() { return *execution_context->realm; }

    NonnullOwnPtr<JS::ExecutionContext> execution_context;
};

}

TEST_CASE(main_world_uses_inline_wrapper_cache)
{
    auto vm = JS::VM::create();
    TestRealm realm { *vm };
    auto* wrapper_world = &Web::Bindings::host_defined_wrapper_world(realm.realm());
    auto wrappable = realm.realm().create<TestWrappable>(realm.realm());
    auto wrapper = realm.realm().create<TestWrapperObject>(realm.realm(), wrappable);

    EXPECT(!cached_wrapper_for(realm.realm(), *wrappable));

    wrapper_world->set_wrapper(*wrappable, *wrapper);
    EXPECT(wrapper.ptr() == cached_wrapper_for(realm.realm(), *wrappable).ptr());

    wrapper_world->clear_wrapper(*wrappable, *wrapper);
    EXPECT(!cached_wrapper_for(realm.realm(), *wrappable));
}

TEST_CASE(wrap_uses_main_world_inline_cache)
{
    auto vm = JS::VM::create();
    TestRealm realm { *vm };
    auto wrappable = realm.realm().create<TestWrappable>(realm.realm());

    auto wrapper = wrap_test_wrappable(realm.realm(), wrappable);

    EXPECT(wrapper.ptr() == wrap_test_wrappable(realm.realm(), wrappable).ptr());
    EXPECT(&wrapper->realm() == &realm.realm());
    EXPECT(wrapper.ptr() == cached_wrapper_for(realm.realm(), *wrappable).ptr());
}

TEST_CASE(wrapper_forwards_identity_and_origin_to_wrappable)
{
    auto vm = JS::VM::create();
    TestRealm realm { *vm };
    auto wrappable = realm.realm().create<TestWrappable>(realm.realm());
    URL::Origin::OpaqueData::Nonce nonce {};
    nonce[0] = 42;
    auto origin = URL::Origin { URL::Origin::OpaqueData { nonce, URL::Origin::OpaqueData::Type::Standard } };
    wrappable->set_origin(origin);

    auto wrapper = wrap_test_wrappable(realm.realm(), wrappable);

    EXPECT(wrapper->interface_name() == Web::Bindings::InterfaceName::EventTarget);
    EXPECT(wrapper->implements_interface("TestWrappable"_string));
    EXPECT(!wrapper->implements_interface("DefinitelyNotTestWrappable"_string));

    auto extracted_origin = wrapper->extract_an_origin();
    EXPECT(extracted_origin.has_value());
    EXPECT(extracted_origin->is_opaque());
    EXPECT(extracted_origin->opaque_data().nonce == nonce);
    EXPECT(extracted_origin->opaque_data().type == URL::Origin::OpaqueData::Type::Standard);
}

TEST_CASE(first_main_world_wrap_chooses_wrapper_realm)
{
    auto vm = JS::VM::create();
    TestRealm allocation_realm { *vm };
    TestRealm caller_realm { *vm };
    auto wrappable = allocation_realm.realm().create<TestWrappable>(allocation_realm.realm());

    auto wrapper = wrap_test_wrappable(caller_realm.realm(), wrappable);

    EXPECT(&wrapper->realm() == &caller_realm.realm());
    EXPECT(wrapper.ptr() == cached_wrapper_for(caller_realm.realm(), *wrappable).ptr());
    EXPECT(wrapper.ptr() == cached_wrapper_for(allocation_realm.realm(), *wrappable).ptr());
    EXPECT(wrapper.ptr() == wrap_test_wrappable(allocation_realm.realm(), wrappable).ptr());
}

TEST_CASE(main_world_reuses_first_wrapper_across_realms)
{
    auto vm = JS::VM::create();
    TestRealm allocation_realm { *vm };
    TestRealm first_caller_realm { *vm };
    TestRealm second_caller_realm { *vm };
    auto wrappable = allocation_realm.realm().create<TestWrappable>(allocation_realm.realm());

    auto first_wrapper = wrap_test_wrappable(first_caller_realm.realm(), wrappable);
    auto second_wrapper = wrap_test_wrappable(second_caller_realm.realm(), wrappable);

    EXPECT(second_wrapper.ptr() == first_wrapper.ptr());
    EXPECT(&second_wrapper->realm() == &first_caller_realm.realm());
    EXPECT(first_wrapper.ptr() == cached_wrapper_for(allocation_realm.realm(), *wrappable).ptr());
    EXPECT(first_wrapper.ptr() == cached_wrapper_for(first_caller_realm.realm(), *wrappable).ptr());
    EXPECT(first_wrapper.ptr() == cached_wrapper_for(second_caller_realm.realm(), *wrappable).ptr());
}

TEST_CASE(global_wrapper_uses_requested_realm)
{
    auto vm = JS::VM::create();
    TestRealm allocation_realm { *vm };
    auto wrappable = allocation_realm.realm().create<TestWrappable>(allocation_realm.realm());
    auto extension_execution_context = MUST(JS::Realm::initialize_host_defined_realm(*vm, nullptr, nullptr));
    auto& extension_realm = *extension_execution_context->realm;

    auto wrapper = Web::Bindings::create_global_object_wrapper(extension_realm, wrappable);

    EXPECT(&wrapper->realm() == &extension_realm);
    EXPECT(Web::Bindings::impl_from<TestWrappable>(wrapper.ptr()) == wrappable.ptr());

    vm->pop_execution_context();
}

TEST_CASE(extension_world_uses_per_world_wrapper_cache)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    TestRealm extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto* main_world_cache = &Web::Bindings::host_defined_wrapper_world(main_world.realm());
    auto* extension_world_cache = &Web::Bindings::host_defined_wrapper_world(extension_realm.realm());
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());
    auto main_wrapper = main_world.realm().create<TestWrapperObject>(main_world.realm(), wrappable);
    auto extension_wrapper = extension_realm.realm().create<TestWrapperObject>(extension_realm.realm(), wrappable);

    main_world_cache->set_wrapper(*wrappable, *main_wrapper);
    extension_world_cache->set_wrapper(*wrappable, *extension_wrapper);

    EXPECT(extension_wrapper.ptr() != main_wrapper.ptr());
    EXPECT(&extension_wrapper->realm() == &extension_realm.realm());
    EXPECT(cached_wrapper_for(main_world.realm(), *wrappable).ptr() == main_wrapper.ptr());
    EXPECT(cached_wrapper_for(extension_realm.realm(), *wrappable).ptr() == extension_wrapper.ptr());

    extension_world_cache->clear_wrapper(*wrappable, *extension_wrapper);
    EXPECT(!cached_wrapper_for(extension_realm.realm(), *wrappable));
    EXPECT(cached_wrapper_for(main_world.realm(), *wrappable).ptr() == main_wrapper.ptr());
}

TEST_CASE(extension_first_wrap_does_not_fill_main_world_cache)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    TestRealm extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());

    auto extension_wrapper = wrap_test_wrappable(extension_realm.realm(), wrappable);

    EXPECT(&extension_wrapper->realm() == &extension_realm.realm());
    EXPECT(!cached_wrapper_for(main_world.realm(), *wrappable));
    EXPECT(extension_wrapper.ptr() == wrap_test_wrappable(extension_realm.realm(), wrappable).ptr());

    auto main_wrapper = wrap_test_wrappable(main_world.realm(), wrappable);
    EXPECT(main_wrapper.ptr() != extension_wrapper.ptr());
    EXPECT(&main_wrapper->realm() == &main_world.realm());
    EXPECT(cached_wrapper_for(main_world.realm(), *wrappable).ptr() == main_wrapper.ptr());
    EXPECT(extension_wrapper.ptr() == wrap_test_wrappable(extension_realm.realm(), wrappable).ptr());
}

TEST_CASE(global_wrapper_cache_uses_realm_wrapper_world)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());

    auto extension_execution_context = MUST(JS::Realm::initialize_host_defined_realm(
        *vm,
        [&](JS::Realm& realm) -> JS::Object* {
            return Web::Bindings::create_global_object_wrapper(realm, wrappable).ptr();
        },
        nullptr));
    auto& extension_realm = *extension_execution_context->realm;
    install_test_host_defined(extension_realm, Web::Bindings::WrapperWorld::Type::Extension);
    Web::Bindings::cache_global_object_wrapper(extension_realm);

    auto* wrapper = as_if<TestWrapperObject>(&extension_realm.global_object());
    EXPECT(wrapper);
    EXPECT(&wrapper->realm() == &extension_realm);
    EXPECT(Web::Bindings::impl_from<TestWrappable>(wrapper) == wrappable.ptr());
    EXPECT(cached_wrapper_for(extension_realm, *wrappable).ptr() == wrapper);
    EXPECT(!cached_wrapper_for(main_world.realm(), *wrappable));

    vm->pop_execution_context();
}

TEST_CASE(wrap_uses_extension_world_cache)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    TestRealm extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());

    auto main_wrapper = wrap_test_wrappable(main_world.realm(), wrappable);
    auto extension_wrapper = wrap_test_wrappable(extension_realm.realm(), wrappable);

    EXPECT(extension_wrapper.ptr() == wrap_test_wrappable(extension_realm.realm(), wrappable).ptr());
    EXPECT(extension_wrapper.ptr() != main_wrapper.ptr());
    EXPECT(&extension_wrapper->realm() == &extension_realm.realm());
    EXPECT(&main_wrapper->realm() == &main_world.realm());
    EXPECT(cached_wrapper_for(main_world.realm(), *wrappable).ptr() == main_wrapper.ptr());
}

TEST_CASE(legacy_property_getters_use_wrapper_realm)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    TestRealm extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto parent = main_world.realm().create<TestWrappable>(main_world.realm());
    auto child = main_world.realm().create<TestWrappable>(main_world.realm());
    parent->set_indexed_value(child);
    parent->set_named_value(child);

    auto main_wrapper = wrap_test_wrappable(main_world.realm(), parent);
    auto extension_wrapper = wrap_test_wrappable(extension_realm.realm(), parent);

    auto main_descriptor = MUST(main_wrapper->internal_get_own_property(JS::PropertyKey { 0u }));
    EXPECT(main_descriptor.has_value());
    EXPECT(main_descriptor->value.has_value());
    EXPECT(main_descriptor->value->is_object());
    auto child_main_wrapper = main_descriptor->value->as_if<TestWrapperObject>();
    EXPECT(child_main_wrapper);
    EXPECT(&child_main_wrapper->realm() == &main_world.realm());
    EXPECT(Web::Bindings::impl_from<TestWrappable>(child_main_wrapper.ptr()) == child.ptr());
    EXPECT(cached_wrapper_for(main_world.realm(), *child).ptr() == child_main_wrapper.ptr());

    auto extension_descriptor = MUST(extension_wrapper->internal_get_own_property(JS::PropertyKey { 0u }));
    EXPECT(extension_descriptor.has_value());
    EXPECT(extension_descriptor->value.has_value());
    EXPECT(extension_descriptor->value->is_object());
    auto child_extension_wrapper = extension_descriptor->value->as_if<TestWrapperObject>();
    EXPECT(child_extension_wrapper);
    EXPECT(&child_extension_wrapper->realm() == &extension_realm.realm());
    EXPECT(child_extension_wrapper.ptr() != child_main_wrapper.ptr());
    EXPECT(Web::Bindings::impl_from<TestWrappable>(child_extension_wrapper.ptr()) == child.ptr());

    auto named_descriptor = MUST(extension_wrapper->internal_get_own_property(JS::PropertyKey { "child"_utf16_fly_string, JS::PropertyKey::StringMayBeNumber::No }));
    EXPECT(named_descriptor.has_value());
    EXPECT(named_descriptor->value.has_value());
    EXPECT(named_descriptor->value->is_object());
    auto named_child_extension_wrapper = named_descriptor->value->as_if<TestWrapperObject>();
    EXPECT(named_child_extension_wrapper);
    EXPECT(&named_child_extension_wrapper->realm() == &extension_realm.realm());
    EXPECT(named_child_extension_wrapper.ptr() == child_extension_wrapper.ptr());

    auto* main_test_wrapper = as<TestWrapperObject>(main_wrapper.ptr());
    MUST(main_test_wrapper->set_value_of_new_indexed_property(main_world.realm(), 0, JS::js_undefined()));
    EXPECT(parent->last_setter_realm() == &main_world.realm());

    auto* extension_test_wrapper = as<TestWrapperObject>(extension_wrapper.ptr());
    MUST(extension_test_wrapper->set_value_of_named_property(extension_realm.realm(), "child"_string, JS::js_undefined()));
    EXPECT(parent->last_setter_realm() == &extension_realm.realm());
}
