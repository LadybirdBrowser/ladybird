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
    explicit TestWrappable(JS::Realm& realm)
        : Wrappable(realm)
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

    virtual Optional<URL::Origin> extract_an_origin() const override { return m_origin; }

    virtual Optional<JS::Value> item_value(JS::Realm& realm, size_t index) const override
    {
        if (index != 0 || !m_indexed_value)
            return {};
        return Web::Bindings::wrap(realm, GC::Ref { *m_indexed_value }).ptr();
    }

    virtual Vector<FlyString> supported_property_names() const override
    {
        if (!m_named_value)
            return {};
        return { "child"_fly_string };
    }

    virtual JS::Value named_item_value(JS::Realm& realm, FlyString const& name) const override
    {
        if (name != "child"_fly_string || !m_named_value)
            return JS::js_undefined();
        return Web::Bindings::wrap(realm, GC::Ref { *m_named_value }).ptr();
    }

protected:
    virtual GC::Ref<Web::Bindings::PlatformObject> create_wrapper(JS::Realm&) override;

    virtual void visit_edges(GC::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_indexed_value);
        visitor.visit(m_named_value);
    }

private:
    GC::Ptr<TestWrappable> m_indexed_value;
    GC::Ptr<TestWrappable> m_named_value;
    Optional<URL::Origin> m_origin;
};

class TestWrapperObject final : public Web::Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(TestWrapperObject, Web::Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TestWrapperObject);

public:
    TestWrapperObject(JS::Realm& realm, GC::Ref<TestWrappable> impl)
        : PlatformObject(realm)
        , m_impl(impl)
    {
        m_legacy_platform_object_flags = LegacyPlatformObjectFlags {};
        m_legacy_platform_object_flags->supports_indexed_properties = true;
        m_legacy_platform_object_flags->supports_named_properties = true;
    }

protected:
    virtual Web::Bindings::Wrappable* wrappable_impl() override { return m_impl.ptr(); }
    virtual Web::Bindings::Wrappable const* wrappable_impl() const override { return m_impl.ptr(); }

private:
    virtual void visit_edges(JS::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_impl);
    }

    GC::Ref<TestWrappable> m_impl;
};

static_assert(!IsConstructible<JS::Value, TestWrappable*>);
static_assert(!IsConstructible<JS::Value, TestWrappable const*>);
static_assert(!IsConstructible<JS::Value, GC::Ptr<TestWrappable>>);
static_assert(!IsConstructible<JS::Value, GC::Ref<TestWrappable>>);
static_assert(!IsConstructible<JS::Value, GC::Root<TestWrappable> const&>);
static_assert(IsConstructible<JS::Value, TestWrapperObject*>);
static_assert(IsConstructible<JS::Value, GC::Ref<TestWrapperObject>>);
static_assert(!IsConstructible<JS::Completion, TestWrappable*>);
static_assert(!IsConstructible<JS::Completion, TestWrappable const*>);
static_assert(!IsConstructible<JS::Completion, GC::Ptr<TestWrappable>>);
static_assert(!IsConstructible<JS::Completion, GC::Ref<TestWrappable>>);
static_assert(!IsConstructible<JS::Completion, GC::Root<TestWrappable> const&>);
static_assert(!IsConstructible<JS::ThrowCompletionOr<JS::Value>, TestWrappable*>);
static_assert(!IsConstructible<JS::ThrowCompletionOr<JS::Value>, TestWrappable const*>);
static_assert(!IsConstructible<JS::ThrowCompletionOr<JS::Value>, GC::Ptr<TestWrappable>>);
static_assert(!IsConstructible<JS::ThrowCompletionOr<JS::Value>, GC::Ref<TestWrappable>>);
static_assert(!IsConstructible<JS::ThrowCompletionOr<JS::Value>, GC::Root<TestWrappable> const&>);
static_assert(IsConstructible<JS::ThrowCompletionOr<JS::Value>, TestWrapperObject*>);
static_assert(IsConstructible<JS::ThrowCompletionOr<JS::Value>, GC::Ref<TestWrapperObject>>);
static_assert(!IsConstructible<Web::WebIDL::ExceptionOr<JS::Value>, TestWrappable*>);
static_assert(!IsConstructible<Web::WebIDL::ExceptionOr<JS::Value>, TestWrappable const*>);
static_assert(!IsConstructible<Web::WebIDL::ExceptionOr<JS::Value>, GC::Ptr<TestWrappable>>);
static_assert(!IsConstructible<Web::WebIDL::ExceptionOr<JS::Value>, GC::Ref<TestWrappable>>);
static_assert(!IsConstructible<Web::WebIDL::ExceptionOr<JS::Value>, GC::Root<TestWrappable> const&>);
static_assert(IsConstructible<Web::WebIDL::ExceptionOr<JS::Value>, TestWrapperObject*>);
static_assert(IsConstructible<Web::WebIDL::ExceptionOr<JS::Value>, GC::Ref<TestWrapperObject>>);

GC_DEFINE_ALLOCATOR(TestWrappable);
GC_DEFINE_ALLOCATOR(TestWrapperObject);

GC::Ref<Web::Bindings::PlatformObject> TestWrappable::create_wrapper(JS::Realm& realm)
{
    return realm.create<TestWrapperObject>(realm, *this);
}

GC::Ref<Web::Bindings::PlatformObject> wrap_test_wrappable(JS::Realm& realm, GC::Ref<TestWrappable> wrappable)
{
    return Web::Bindings::wrap(realm, wrappable);
}

GC::Ptr<Web::Bindings::PlatformObject> cached_wrapper_for(JS::Realm& realm, TestWrappable const& wrappable)
{
    return Web::Bindings::host_defined_wrapper_world(realm).wrapper_for(wrappable);
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

    EXPECT(!wrapper_world->wrapper_for(*wrappable));

    wrapper_world->set_wrapper(*wrappable, *wrapper);
    EXPECT(wrapper.ptr() == wrapper_world->wrapper_for(*wrappable).ptr());

    wrapper_world->clear_wrapper(*wrappable, *wrapper);
    EXPECT(!wrapper_world->wrapper_for(*wrappable));

    vm->pop_execution_context();
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

    vm->pop_execution_context();
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

    vm->pop_execution_context();
}

TEST_CASE(main_world_wrap_uses_wrappable_realm)
{
    auto vm = JS::VM::create();
    TestRealm implementation_realm { *vm };
    TestRealm caller_realm { *vm };
    auto wrappable = implementation_realm.realm().create<TestWrappable>(implementation_realm.realm());

    auto wrapper = wrap_test_wrappable(caller_realm.realm(), wrappable);

    EXPECT(&wrapper->realm() == &implementation_realm.realm());
    EXPECT(wrapper.ptr() == cached_wrapper_for(implementation_realm.realm(), *wrappable).ptr());
    EXPECT(wrapper.ptr() == wrap_test_wrappable(implementation_realm.realm(), wrappable).ptr());

    vm->pop_execution_context();
    vm->pop_execution_context();
}

TEST_CASE(non_main_global_wrapper_can_use_a_different_wrappable_realm)
{
    auto vm = JS::VM::create();
    TestRealm implementation_realm { *vm };
    auto wrappable = implementation_realm.realm().create<TestWrappable>(implementation_realm.realm());
    auto extension_execution_context = MUST(JS::Realm::initialize_host_defined_realm(*vm, nullptr, nullptr));
    auto& extension_realm = *extension_execution_context->realm;

    auto wrapper = Web::Bindings::create_global_object_wrapper(extension_realm, wrappable, Web::Bindings::WrapperWorld::Type::Extension);

    EXPECT(&wrapper->realm() == &extension_realm);
    EXPECT(Web::Bindings::impl_from<TestWrappable>(wrapper.ptr()) == wrappable.ptr());

    vm->pop_execution_context();
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
    EXPECT(main_world_cache->wrapper_for(*wrappable).ptr() == main_wrapper.ptr());
    EXPECT(extension_world_cache->wrapper_for(*wrappable).ptr() == extension_wrapper.ptr());

    extension_world_cache->clear_wrapper(*wrappable, *extension_wrapper);
    EXPECT(!extension_world_cache->wrapper_for(*wrappable));
    EXPECT(main_world_cache->wrapper_for(*wrappable).ptr() == main_wrapper.ptr());

    vm->pop_execution_context();
    vm->pop_execution_context();
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

    vm->pop_execution_context();
    vm->pop_execution_context();
}

TEST_CASE(extension_global_wrapper_can_use_existing_impl)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());

    auto extension_execution_context = MUST(JS::Realm::initialize_host_defined_realm(
        *vm,
        [&](JS::Realm& realm) -> JS::Object* {
            return Web::Bindings::create_global_object_wrapper(realm, wrappable, Web::Bindings::WrapperWorld::Type::Extension).ptr();
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

    vm->pop_execution_context();
    vm->pop_execution_context();
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

    vm->pop_execution_context();
    vm->pop_execution_context();
}
