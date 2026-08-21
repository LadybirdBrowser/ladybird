/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Weak.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Symbol.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Window.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/BarProp.h>
#include <LibWeb/HTML/CrossOrigin/CrossOriginPropertyDescriptorMap.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/Platform/Timer.h>
#include <LibWeb/ResizeObserver/ResizeObserver.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace {

bool s_main_thread_vm_was_initialized_by_an_earlier_wrapper_test = false;

class TestWrapperObject;

class TestPageClient final : public Web::PageClient {
    GC_CELL(TestPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(TestPageClient);

public:
    virtual u64 id() const override { return 1; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { VERIFY_NOT_REACHED(); }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1; }
    virtual double device_pixel_ratio() const override { return 1; }
    virtual double device_pixels_per_css_pixel() const override { return 1; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event(u64, Web::EventResult) override { }
    virtual Web::HTML::CrossProcessId allocate_cross_process_id() override { return { 1, m_next_cross_process_id++ }; }
    virtual void request_frame() override { }
    virtual void request_file(Web::FileRequest) override { }
    virtual bool is_headless() const override { return true; }
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ptr<Web::Page> m_page;
    u64 m_next_cross_process_id { 1 };
};

class TestWrappable final : public Web::Bindings::GCAllocatedWrappable {
    WEB_NON_IDL_WRAPPABLE(TestWrappable, Web::Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(TestWrappable);

public:
    explicit TestWrappable(JS::Realm&)
        : Web::Bindings::GCAllocatedWrappable()
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
    void set_relevant_global_impl(GC::Ptr<Web::Bindings::Wrappable> relevant_global_impl) { m_relevant_global_impl = relevant_global_impl; }
    void record_setter_realm(JS::Realm& realm) { m_last_setter_realm = &realm; }
    JS::Realm* last_setter_realm() const { return m_last_setter_realm.ptr(); }

    virtual Optional<URL::Origin> extract_an_origin() const override { return m_origin; }
    virtual GC::Ptr<Web::Bindings::Wrappable> relevant_global_impl() const override { return m_relevant_global_impl; }

    virtual Vector<Utf16FlyString> supported_property_names() const override
    {
        if (!m_named_value)
            return {};
        return { "child"_utf16_fly_string };
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
        visitor.visit(m_relevant_global_impl);
    }

private:
    GC::Ptr<TestWrappable> m_indexed_value;
    GC::Ptr<TestWrappable> m_named_value;
    GC::Ptr<JS::Realm> m_last_setter_realm;
    GC::Ptr<Web::Bindings::Wrappable> m_relevant_global_impl;
    Optional<URL::Origin> m_origin;
};

class TestWrapperObject final : public Web::Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(TestWrapperObject, Web::Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TestWrapperObject);

public:
    using PlatformObject::set_value_of_named_property;
    using PlatformObject::set_value_of_new_indexed_property;

    TestWrapperObject(JS::Realm& realm, GC::Ref<Web::Bindings::Wrappable> impl)
        : PlatformObject(realm, impl)
    {
        m_legacy_platform_object_flags = LegacyPlatformObjectFlags {};
        m_legacy_platform_object_flags->supports_indexed_properties = true;
        m_legacy_platform_object_flags->supports_named_properties = true;
    }

    virtual Web::WebIDL::ExceptionOr<void> set_value_of_named_property(JS::Realm& realm, Utf16FlyString const&, JS::Value) override
    {
        static_cast<TestWrappable&>(*wrappable_impl()).record_setter_realm(realm);
        return {};
    }

    virtual Web::WebIDL::ExceptionOr<void> set_value_of_new_indexed_property(JS::Realm& realm, u32, JS::Value) override
    {
        static_cast<TestWrappable&>(*wrappable_impl()).record_setter_realm(realm);
        return {};
    }

protected:
    virtual Optional<JS::Value> item_value(Web::Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const override
    {
        auto const& impl = static_cast<TestWrappable const&>(*wrappable_impl());
        if (index != 0 || !impl.indexed_value())
            return {};
        return Web::Bindings::wrap(wrapper_world, realm, GC::Ref { *impl.indexed_value() }).ptr();
    }

    virtual JS::Value named_item_value(Web::Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, Utf16FlyString const& name) const override
    {
        auto const& impl = static_cast<TestWrappable const&>(*wrappable_impl());
        if (name != "child"_utf16_fly_string || !impl.named_value())
            return JS::js_undefined();
        return Web::Bindings::wrap(wrapper_world, realm, GC::Ref { *impl.named_value() }).ptr();
    }
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
GC_DEFINE_ALLOCATOR(TestPageClient);

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

void ensure_test_vm_is_web_vm(JS::VM& vm)
{
    if (!vm.agent())
        vm.set_agent(Web::HTML::SimilarOriginWindowAgent::create(vm.heap()));
}

GC::Ref<JS::Realm> create_test_principal_realm(JS::VM& vm)
{
    ensure_test_vm_is_web_vm(vm);

    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();

    GC::Ptr<Web::HTML::Window> window;
    GC::Ptr<Web::Bindings::PlatformObject> global_this;
    auto execution_context = Web::Bindings::create_a_new_javascript_realm(
        vm,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            window = Web::HTML::Window::create();
            global_this = Web::Bindings::create_global_object_wrapper(realm, GC::Ref { *window });
            return global_this.as_nonnull();
        },
        [&](JS::Realm&) -> GC::Ref<JS::Object> {
            return global_this.as_nonnull();
        });

    auto realm = execution_context->realm;
    URL::Origin::OpaqueData::Nonce nonce {};
    auto origin = URL::Origin { URL::Origin::OpaqueData { nonce, URL::Origin::OpaqueData::Type::Standard } };
    Web::HTML::WindowEnvironmentSettingsObject::setup(
        *page,
        URL::about_blank(),
        move(execution_context),
        nullptr,
        URL::about_blank(),
        origin);
    return *realm;
}

void install_test_host_defined(JS::Realm& realm, Web::Bindings::WrapperWorld::Type wrapper_world_type, JS::Realm& principal_realm)
{
    auto intrinsics = realm.create<Web::Bindings::Intrinsics>(realm);
    auto wrapper_world = realm.heap().allocate<Web::Bindings::WrapperWorld>(wrapper_world_type);
    realm.set_host_defined(make<Web::Bindings::HostDefined>(intrinsics, *wrapper_world, principal_realm));
}

void install_test_host_defined(JS::Realm& realm, GC::Ref<Web::Bindings::WrapperWorld> wrapper_world, JS::Realm& principal_realm)
{
    auto intrinsics = realm.create<Web::Bindings::Intrinsics>(realm);
    realm.set_host_defined(make<Web::Bindings::HostDefined>(intrinsics, wrapper_world, principal_realm));
}

struct TestRealm {
    explicit TestRealm(JS::VM& vm, Web::Bindings::WrapperWorld::Type wrapper_world_type = Web::Bindings::WrapperWorld::Type::Main)
        : principal_realm(create_test_principal_realm(vm))
        , execution_context(MUST(JS::Realm::initialize_host_defined_realm(vm, nullptr, nullptr)))
    {
        ensure_test_vm_is_web_vm(vm);
        install_test_host_defined(realm(), wrapper_world_type, *principal_realm);
    }

    explicit TestRealm(JS::VM& vm, GC::Ref<Web::Bindings::WrapperWorld> wrapper_world)
        : principal_realm(create_test_principal_realm(vm))
        , execution_context(MUST(JS::Realm::initialize_host_defined_realm(vm, nullptr, nullptr)))
    {
        ensure_test_vm_is_web_vm(vm);
        install_test_host_defined(realm(), wrapper_world, *principal_realm);
    }

    ~TestRealm()
    {
        realm().vm().pop_execution_context();
    }

    JS::Realm& realm() { return *execution_context->realm; }

    GC::Root<JS::Realm> principal_realm;
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

TEST_CASE(legacy_platform_object_hides_engine_private_properties)
{
    auto vm = JS::VM::create();
    TestRealm realm { *vm };
    auto wrappable = realm.realm().create<TestWrappable>(realm.realm());
    auto wrapper = realm.realm().create<TestWrapperObject>(realm.realm(), wrappable);
    auto public_symbol = JS::Symbol::create(*vm, "public"_utf16);

    wrapper->define_direct_property(public_symbol, JS::js_undefined(), {});
    wrapper->set_engine_private_property(JS::Symbol::create_private(*vm), JS::js_undefined());

    auto keys = MUST(wrapper->internal_own_property_keys());
    EXPECT_EQ(keys.size(), 1u);
    EXPECT(keys[0].is_symbol());
    EXPECT(&keys[0].as_symbol() == public_symbol.ptr());
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
    auto main_world = vm->heap().allocate<Web::Bindings::WrapperWorld>(Web::Bindings::WrapperWorld::Type::Main);
    TestRealm allocation_realm { *vm, main_world };
    TestRealm caller_realm { *vm, main_world };
    auto wrappable = allocation_realm.realm().create<TestWrappable>(allocation_realm.realm());

    auto wrapper = wrap_test_wrappable(caller_realm.realm(), wrappable);

    EXPECT(&wrapper->realm() == &caller_realm.realm());
    EXPECT(wrapper.ptr() == cached_wrapper_for(caller_realm.realm(), *wrappable).ptr());
    EXPECT(wrapper.ptr() == cached_wrapper_for(allocation_realm.realm(), *wrappable).ptr());
    EXPECT(wrapper.ptr() == wrap_test_wrappable(allocation_realm.realm(), wrappable).ptr());
}

TEST_CASE(relevant_global_impl_selects_main_world_wrapper_realm)
{
    auto vm = JS::VM::create();
    auto relevant_realm = create_test_principal_realm(*vm);
    auto preferred_realm = create_test_principal_realm(*vm);
    auto wrappable = relevant_realm->create<TestWrappable>(*relevant_realm);
    auto* relevant_global_impl = Web::Bindings::wrappable_impl_from(&relevant_realm->global_object());
    VERIFY(relevant_global_impl);
    wrappable->set_relevant_global_impl(relevant_global_impl);

    auto& wrapper_world = Web::Bindings::host_defined_wrapper_world(*preferred_realm);
    auto wrapper = Web::Bindings::wrap(wrapper_world, *preferred_realm, wrappable);

    EXPECT(&wrapper->realm() == relevant_realm.ptr());
    EXPECT(&wrapper->realm() != preferred_realm.ptr());
    EXPECT(wrapper.ptr() == cached_wrapper_for(*preferred_realm, *wrappable).ptr());
}

TEST_CASE(relevant_global_impl_reselects_realm_after_cache_clear)
{
    auto vm = JS::VM::create();
    auto relevant_realm = create_test_principal_realm(*vm);
    auto first_preferred_realm = create_test_principal_realm(*vm);
    auto second_preferred_realm = create_test_principal_realm(*vm);
    auto wrappable = relevant_realm->create<TestWrappable>(*relevant_realm);
    auto* relevant_global_impl = Web::Bindings::wrappable_impl_from(&relevant_realm->global_object());
    VERIFY(relevant_global_impl);
    wrappable->set_relevant_global_impl(relevant_global_impl);

    auto& wrapper_world = Web::Bindings::host_defined_wrapper_world(*first_preferred_realm);
    auto first_wrapper = Web::Bindings::wrap(wrapper_world, *first_preferred_realm, wrappable);
    wrapper_world.clear_wrapper(*wrappable, *first_wrapper);

    auto second_wrapper = Web::Bindings::wrap(wrapper_world, *second_preferred_realm, wrappable);

    EXPECT(&second_wrapper->realm() == relevant_realm.ptr());
    EXPECT(second_wrapper.ptr() == cached_wrapper_for(*second_preferred_realm, *wrappable).ptr());
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
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return Web::Bindings::create_global_object_wrapper(realm, wrappable);
        },
        nullptr));
    auto& extension_realm = *extension_execution_context->realm;
    install_test_host_defined(extension_realm, Web::Bindings::WrapperWorld::Type::Extension, *main_world.principal_realm);
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

TEST_CASE(agent_main_world_cell_is_shared_by_same_agent_pages)
{
    auto vm = JS::VM::create();
    ensure_test_vm_is_web_vm(*vm);

    auto first_client = vm->heap().allocate<TestPageClient>();
    auto first_page = Web::Page::create(first_client);
    first_client->m_page = first_page.ptr();

    auto second_client = vm->heap().allocate<TestPageClient>();
    auto second_page = Web::Page::create(second_client);
    second_client->m_page = second_page.ptr();

    auto& agent_main_world = static_cast<Web::HTML::Agent&>(*vm->agent()).main_world();

    TestRealm first_realm { *vm, agent_main_world };
    TestRealm second_realm { *vm, agent_main_world };
    EXPECT(&Web::Bindings::host_defined_wrapper_world(first_realm.realm()) == &agent_main_world);
    EXPECT(&Web::Bindings::host_defined_wrapper_world(second_realm.realm()) == &agent_main_world);

    auto wrappable = first_realm.realm().create<TestWrappable>(first_realm.realm());
    auto first_wrapper = wrap_test_wrappable(first_realm.realm(), wrappable);
    auto second_wrapper = wrap_test_wrappable(second_realm.realm(), wrappable);

    EXPECT(first_wrapper.ptr() == second_wrapper.ptr());
    EXPECT(cached_wrapper_for(first_realm.realm(), *wrappable).ptr() == first_wrapper.ptr());
    EXPECT(cached_wrapper_for(second_realm.realm(), *wrappable).ptr() == first_wrapper.ptr());
}

TEST_CASE(principal_realms_in_same_agent_share_main_world_wrapper_identity)
{
    auto vm = JS::VM::create();
    auto first_realm = create_test_principal_realm(*vm);
    auto second_realm = create_test_principal_realm(*vm);

    auto& first_world = Web::Bindings::host_defined_wrapper_world(*first_realm);
    auto& second_world = Web::Bindings::host_defined_wrapper_world(*second_realm);
    EXPECT(&first_world == &second_world);

    auto wrappable = first_realm->create<TestWrappable>(*first_realm);
    auto first_wrapper = Web::Bindings::wrap(first_world, *first_realm, wrappable);
    auto second_wrapper = Web::Bindings::wrap(second_world, *second_realm, wrappable);

    EXPECT(second_wrapper.ptr() == first_wrapper.ptr());
    EXPECT(&second_wrapper->realm() == &first_wrapper->realm());
    EXPECT(first_world.wrapper_for(*wrappable, *first_realm).ptr() == first_wrapper.ptr());
    EXPECT(second_world.wrapper_for(*wrappable, *second_realm).ptr() == first_wrapper.ptr());
}

TEST_CASE(internal_world_does_not_share_main_world_slots)
{
    auto vm = JS::VM::create();
    auto main_world = vm->heap().allocate<Web::Bindings::WrapperWorld>(Web::Bindings::WrapperWorld::Type::Main);
    TestRealm main_realm { *vm, main_world };
    TestRealm internal_realm { *vm, Web::Bindings::WrapperWorld::Type::Internal };
    auto wrappable = main_realm.realm().create<TestWrappable>(main_realm.realm());

    auto main_wrapper = wrap_test_wrappable(main_realm.realm(), wrappable);
    auto internal_wrapper = wrap_test_wrappable(internal_realm.realm(), wrappable);

    EXPECT(internal_wrapper.ptr() != main_wrapper.ptr());
    EXPECT(&internal_wrapper->realm() == &internal_realm.realm());
    EXPECT(cached_wrapper_for(main_realm.realm(), *wrappable).ptr() == main_wrapper.ptr());
    EXPECT(cached_wrapper_for(internal_realm.realm(), *wrappable).ptr() == internal_wrapper.ptr());

    Web::Bindings::WrapperWorldWeakValueCache<TestWrapperObject> cache;
    auto main_value = main_realm.realm().create<TestWrapperObject>(main_realm.realm(), wrappable);
    auto internal_value = internal_realm.realm().create<TestWrapperObject>(internal_realm.realm(), wrappable);

    cache.set(Web::Bindings::host_defined_wrapper_world(main_realm.realm()), main_value);
    cache.set(Web::Bindings::host_defined_wrapper_world(internal_realm.realm()), internal_value);

    EXPECT(cache.get(Web::Bindings::host_defined_wrapper_world(main_realm.realm())).ptr() == main_value.ptr());
    EXPECT(cache.get(Web::Bindings::host_defined_wrapper_world(internal_realm.realm())).ptr() == internal_value.ptr());
}

TEST_CASE(extension_wrapper_world_cells_are_realm_local)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    TestRealm first_extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    TestRealm second_extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());

    auto& first_world = Web::Bindings::host_defined_wrapper_world(first_extension_realm.realm());
    auto& second_world = Web::Bindings::host_defined_wrapper_world(second_extension_realm.realm());
    EXPECT(&first_world != &second_world);

    auto first_wrapper = wrap_test_wrappable(first_extension_realm.realm(), wrappable);
    auto second_wrapper = wrap_test_wrappable(second_extension_realm.realm(), wrappable);

    EXPECT(first_wrapper.ptr() != second_wrapper.ptr());
    EXPECT(&first_wrapper->realm() == &first_extension_realm.realm());
    EXPECT(&second_wrapper->realm() == &second_extension_realm.realm());
    EXPECT(first_world.wrapper_for(*wrappable, first_extension_realm.realm()).ptr() == first_wrapper.ptr());
    EXPECT(second_world.wrapper_for(*wrappable, second_extension_realm.realm()).ptr() == second_wrapper.ptr());
}

TEST_CASE(detaching_extension_world_stops_preservation)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());
    TestRealm extension_world { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto wrapper = wrap_test_wrappable(extension_world.realm(), wrappable);
    auto& wrapper_world = Web::Bindings::host_defined_wrapper_world(extension_world.realm());

    Web::Bindings::preserve_wrapper(*wrappable, *wrapper);
    EXPECT(Web::Bindings::wrapper_is_preserved(*wrapper));
    EXPECT(!wrapper_world.is_detached());
    EXPECT(wrapper_world.wrapper_for(*wrappable, extension_world.realm()).ptr() == wrapper.ptr());

    wrapper_world.detach();
    EXPECT(wrapper_world.is_detached());
    EXPECT(!Web::Bindings::wrapper_is_preserved(*wrapper));
    EXPECT(!wrapper_world.wrapper_for(*wrappable, extension_world.realm()));

    // Script can still reach the wrapper after detach (e.g. a pending microtask adding an
    // expando). Preservation must be a harmless no-op, not a crash.
    Web::Bindings::preserve_wrapper(*wrappable, *wrapper);
    EXPECT(!Web::Bindings::wrapper_is_preserved(*wrapper));
}

TEST_CASE(main_and_non_main_world_preservation_state_is_independent)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    TestRealm extension_world { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());
    auto main_wrapper = wrap_test_wrappable(main_world.realm(), wrappable);
    auto extension_wrapper = wrap_test_wrappable(extension_world.realm(), wrappable);
    auto& extension_wrapper_world = Web::Bindings::host_defined_wrapper_world(extension_world.realm());

    EXPECT(!Web::Bindings::wrapper_is_preserved(*main_wrapper));
    EXPECT(!Web::Bindings::wrapper_is_preserved(*extension_wrapper));

    Web::Bindings::preserve_wrapper(*wrappable, *main_wrapper);
    EXPECT(Web::Bindings::wrapper_is_preserved(*main_wrapper));
    EXPECT(!Web::Bindings::wrapper_is_preserved(*extension_wrapper));

    Web::Bindings::preserve_wrapper(*wrappable, *extension_wrapper);
    EXPECT(Web::Bindings::wrapper_is_preserved(*main_wrapper));
    EXPECT(Web::Bindings::wrapper_is_preserved(*extension_wrapper));

    extension_wrapper_world.detach();
    EXPECT(Web::Bindings::wrapper_is_preserved(*main_wrapper));
    EXPECT(!Web::Bindings::wrapper_is_preserved(*extension_wrapper));
}

TEST_CASE(preserved_main_world_wrapper_keeps_expando_across_collection)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());
    auto wrappable_root = GC::make_root(wrappable);
    GC::Weak<Web::Bindings::PlatformObject> original_wrapper;
    auto const expando_name = JS::PropertyKey { "diet-marker"_utf16_fly_string };

    {
        auto wrapper = wrap_test_wrappable(main_world.realm(), wrappable);
        original_wrapper = wrapper;

        JS::PropertyDescriptor descriptor;
        descriptor.value = JS::Value(42);
        descriptor.writable = true;
        descriptor.enumerable = true;
        descriptor.configurable = true;
        EXPECT(MUST(wrapper->internal_define_own_property(expando_name, descriptor)));
        EXPECT(Web::Bindings::wrapper_is_preserved(*wrapper));
    }

    vm->heap().collect_garbage(GC::Heap::CollectionType::CollectGarbage);
    EXPECT(original_wrapper);

    auto replacement_wrapper = wrap_test_wrappable(main_world.realm(), *wrappable_root);
    EXPECT(replacement_wrapper == original_wrapper.ptr());
    auto descriptor = MUST(replacement_wrapper->internal_get_own_property(expando_name));
    EXPECT(descriptor.has_value());
    EXPECT(descriptor->value.has_value());
    EXPECT_EQ(descriptor->value->as_double(), 42);
}

TEST_CASE(preserved_extension_world_wrapper_survives_collection_without_detach)
{
    auto vm = JS::VM::create();
    GC::Weak<Web::Bindings::PlatformObject> live_wrapper;
    GC::Weak<TestWrappable> live_wrappable;
    TestRealm main_world { *vm };
    TestRealm extension_world { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());
    live_wrappable = GC::Weak<TestWrappable> { wrappable };
    auto wrappable_root = GC::make_root(wrappable);
    auto& wrapper_world = Web::Bindings::host_defined_wrapper_world(extension_world.realm());
    {
        auto wrapper = wrap_test_wrappable(extension_world.realm(), wrappable);

        live_wrapper = GC::Weak<Web::Bindings::PlatformObject> { wrapper };
        Web::Bindings::preserve_wrapper(*wrappable, *wrapper);
        EXPECT(!wrapper_world.is_detached());
        EXPECT(wrapper_world.wrapper_for(*wrappable, extension_world.realm()) == wrapper);
    }

    vm->heap().collect_garbage(GC::Heap::CollectionType::CollectGarbage);
    EXPECT(live_wrappable);
    EXPECT(live_wrapper);
    EXPECT(wrapper_world.wrapper_for(*wrappable_root, extension_world.realm()) == live_wrapper.ptr());

    wrapper_world.detach();
}

TEST_CASE(extension_custom_element_prototype_does_not_mutate_main_wrapper)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    TestRealm extension_world { *vm, Web::Bindings::WrapperWorld::Type::Extension };

    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());
    auto main_wrapper = wrap_test_wrappable(main_world.realm(), wrappable);
    auto original_prototype = main_wrapper->shape().prototype();
    auto extension_prototype = JS::Object::create(extension_world.realm(), nullptr);

    auto result = Web::Bindings::set_prototype_of_cached_main_world_wrapper(*wrappable, *extension_prototype);
    EXPECT(!result.is_error());
    EXPECT(main_wrapper->shape().prototype() == original_prototype);
}

TEST_CASE(custom_element_prototype_helper_denies_realm_without_host_defined)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };

    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());
    auto main_wrapper = wrap_test_wrappable(main_world.realm(), wrappable);
    auto original_prototype = main_wrapper->shape().prototype();

    auto bare_execution_context = MUST(JS::Realm::initialize_host_defined_realm(*vm, nullptr, nullptr));
    auto& bare_realm = *bare_execution_context->realm;
    auto bare_prototype = JS::Object::create(bare_realm, nullptr);

    auto result = Web::Bindings::set_prototype_of_cached_main_world_wrapper(*wrappable, *bare_prototype);
    EXPECT(!result.is_error());
    EXPECT(main_wrapper->shape().prototype() == original_prototype);

    auto* popped_execution_context = vm->pop_execution_context();
    EXPECT(popped_execution_context == bare_execution_context.ptr());
}

TEST_CASE(window_cross_origin_descriptor_cache_lives_on_wrapper)
{
    auto vm = JS::VM::create();
    TestRealm extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto& wrapper_world = Web::Bindings::host_defined_wrapper_world(extension_realm.realm());
    auto window = Web::HTML::Window::create();
    auto& wrapper = static_cast<Web::Bindings::WindowWrapper&>(*Web::Bindings::wrap(wrapper_world, extension_realm.realm(), window));
    auto key = Web::HTML::CrossOriginKey {
        .current_settings_object = 1,
        .relevant_settings_object = 2,
        .property_key = JS::PropertyKey { "closed"_utf16_fly_string },
    };

    JS::PropertyDescriptor descriptor;
    descriptor.value = JS::Value(false);
    wrapper.cross_origin_property_descriptor_map().set(key, Web::HTML::CrossOriginCachedPropertyDescriptor { descriptor });

    EXPECT(wrapper.cross_origin_property_descriptor_map().contains(key));
    EXPECT(wrapper_world.wrapper_for(*window, extension_realm.realm()).ptr() == &wrapper);
    wrapper_world.detach();
    EXPECT(!wrapper_world.wrapper_for(*window, extension_realm.realm()));
}

TEST_CASE(cross_origin_descriptor_cache_does_not_root_realm_after_wrapper_dies)
{
    auto vm = JS::VM::create();
    GC::Weak<JS::Realm> weak_realm;

    {
        TestRealm realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
        auto& wrapper_world = Web::Bindings::host_defined_wrapper_world(realm.realm());
        auto window = Web::HTML::Window::create();
        auto& wrapper = static_cast<Web::Bindings::WindowWrapper&>(*Web::Bindings::wrap(wrapper_world, realm.realm(), window));
        auto accessor = JS::NativeFunction::create(realm.realm(), [](JS::VM&) { return JS::js_undefined(); }, 0);
        auto key = Web::HTML::CrossOriginKey {
            .current_settings_object = 1,
            .relevant_settings_object = 2,
            .property_key = JS::PropertyKey { "closed"_utf16_fly_string },
        };

        JS::PropertyDescriptor descriptor;
        descriptor.get = accessor;
        wrapper.cross_origin_property_descriptor_map().set(key, Web::HTML::CrossOriginCachedPropertyDescriptor { descriptor });
        weak_realm = GC::Weak<JS::Realm> { realm.realm() };
    }

    // CollectEverything invalidates every cell, including the agent's main-world
    // root. Tear down the agent first so VM destruction does not later release a
    // root whose cell has already been poisoned.
    vm->set_agent(nullptr);
    vm->heap().collect_garbage(GC::Heap::CollectionType::CollectEverything);
    EXPECT(!weak_realm);
}

TEST_CASE(single_shot_timer_restarted_from_callback_keeps_activity_root)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Core::EventLoop event_loop;
    auto vm = JS::VM::create();
    IGNORE_USE_IN_ESCAPING_LAMBDA GC::Root<Web::Platform::Timer> timer;
    timer = Web::Platform::Timer::create_single_shot(GC::Heap::the(), 0, nullptr);
    timer->on_timeout = GC::create_function(GC::Heap::the(), [&] {
        timer->restart(1000);
        event_loop.quit(0);
    });

    timer->start();
    event_loop.exec();

    EXPECT(timer->is_active());
    EXPECT(timer->has_activity_root());
    timer->stop();
}

TEST_CASE(extension_world_hash_cache_scales)
{
    auto vm = JS::VM::create();
    TestRealm extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    Vector<GC::Ref<TestWrappable>> wrappables;
    for (size_t i = 0; i < 512; ++i)
        wrappables.append(extension_realm.realm().create<TestWrappable>(extension_realm.realm()));

    Vector<GC::Ref<Web::Bindings::PlatformObject>> wrappers;
    for (auto wrappable : wrappables)
        wrappers.append(wrap_test_wrappable(extension_realm.realm(), wrappable));

    for (size_t i = 0; i < wrappables.size(); ++i)
        EXPECT(wrappers[i].ptr() == wrap_test_wrappable(extension_realm.realm(), wrappables[i]).ptr());
}

TEST_CASE(wrapper_world_cache_reference_survives_growth)
{
    auto vm = JS::VM::create();
    TestRealm extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    Web::Bindings::WrapperWorldWeakValueCacheMap<TestWrappable, TestWrapperObject> caches;
    Vector<GC::Ref<TestWrappable>> keys;
    for (size_t i = 0; i < 128; ++i)
        keys.append(extension_realm.realm().create<TestWrappable>(extension_realm.realm()));

    auto first_wrapper = extension_realm.realm().create<TestWrapperObject>(extension_realm.realm(), keys[0]);
    auto& first_cache = caches.cache_for(*keys[0]);
    first_cache.set(Web::Bindings::host_defined_wrapper_world(extension_realm.realm()), first_wrapper);

    for (size_t i = 1; i < keys.size(); ++i) {
        auto wrapper = extension_realm.realm().create<TestWrapperObject>(extension_realm.realm(), keys[i]);
        caches.cache_for(*keys[i]).set(Web::Bindings::host_defined_wrapper_world(extension_realm.realm()), wrapper);
    }

    EXPECT(first_cache.get(Web::Bindings::host_defined_wrapper_world(extension_realm.realm())).ptr() == first_wrapper.ptr());
}

TEST_CASE(weak_value_cache_uses_wrapper_world_hash_map)
{
    auto vm = JS::VM::create();
    TestRealm main_world { *vm };
    TestRealm first_extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    TestRealm second_extension_realm { *vm, Web::Bindings::WrapperWorld::Type::Extension };
    auto wrappable = main_world.realm().create<TestWrappable>(main_world.realm());
    Web::Bindings::WrapperWorldWeakValueCache<TestWrapperObject> cache;

    auto main_wrapper = main_world.realm().create<TestWrapperObject>(main_world.realm(), wrappable);
    auto first_extension_wrapper = first_extension_realm.realm().create<TestWrapperObject>(first_extension_realm.realm(), wrappable);
    auto second_extension_wrapper = second_extension_realm.realm().create<TestWrapperObject>(second_extension_realm.realm(), wrappable);

    auto& main_wrapper_world = Web::Bindings::host_defined_wrapper_world(main_world.realm());
    auto& first_extension_world = Web::Bindings::host_defined_wrapper_world(first_extension_realm.realm());
    auto& second_extension_world = Web::Bindings::host_defined_wrapper_world(second_extension_realm.realm());

    cache.set(main_wrapper_world, main_wrapper);
    cache.set(first_extension_world, first_extension_wrapper);
    cache.set(second_extension_world, second_extension_wrapper);

    EXPECT(cache.get(main_wrapper_world).ptr() == main_wrapper.ptr());
    EXPECT(cache.get(first_extension_world).ptr() == first_extension_wrapper.ptr());
    EXPECT(cache.get(second_extension_world).ptr() == second_extension_wrapper.ptr());

    size_t live_values = 0;
    cache.for_each([&](auto&) {
        ++live_values;
    });
    EXPECT(live_values == 3);

    cache.set(first_extension_world, nullptr);
    EXPECT(!cache.get(first_extension_world));
    EXPECT(cache.get(second_extension_world).ptr() == second_extension_wrapper.ptr());
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
    MUST(extension_test_wrapper->set_value_of_named_property(extension_realm.realm(), "child"_utf16_fly_string, JS::js_undefined()));
    EXPECT(parent->last_setter_realm() == &extension_realm.realm());
}

TEST_CASE(relevant_global_main_world_wrapper_ignores_preferred_realm)
{
    Web::Platform::FontPlugin font_plugin(false);
    Web::Platform::FontPlugin::install(font_plugin);

    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal_realm.ptr());
    auto& vm = Web::Bindings::main_thread_vm();

    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();

    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto window = GC::Ref { *traversable->active_document()->window() };

    auto preferred_execution_context = MUST(JS::Realm::initialize_host_defined_realm(vm, nullptr, nullptr));
    auto& preferred_realm = *preferred_execution_context->realm;
    install_test_host_defined(preferred_realm, static_cast<Web::HTML::Agent&>(*vm.agent()).main_world(), window->principal_realm());

    auto bar_prop = GC::Ref { const_cast<Web::HTML::BarProp&>(*window->locationbar()) };
    auto wrapper = Web::Bindings::wrap(Web::Bindings::host_defined_wrapper_world(preferred_realm), preferred_realm, bar_prop);

    EXPECT(&wrapper->realm() == &window->principal_realm());
    EXPECT(&wrapper->realm() != &preferred_realm);
    EXPECT(wrapper.ptr() == cached_wrapper_for(preferred_realm, *bar_prop).ptr());

    vm.pop_execution_context();
    vm.pop_execution_context();
    s_main_thread_vm_was_initialized_by_an_earlier_wrapper_test = true;
}

// This exercises the collected-document finalization path. It does not claim
// coverage of the ResizeObserver sweep callback: the document's destruction
// path releases the activity root before that callback would be relevant.
TEST_CASE(resize_observer_releases_activity_root_when_registration_document_is_collected_via_finalization)
{
    // The rest of this suite deliberately creates and retains unrelated VMs.
    // Run this fixture in isolation to exercise the main-thread initialization
    // path; avoid corrupting the shared suite state when run without a filter.
    if (s_main_thread_vm_was_initialized_by_an_earlier_wrapper_test)
        return;

    Web::Platform::FontPlugin font_plugin(false);
    Web::Platform::FontPlugin::install(font_plugin);
    Web::Bindings::initialize_main_thread_vm(Web::HTML::AgentType::SimilarOriginWindow);
    auto& vm = Web::Bindings::main_thread_vm();
    auto make_document = [&]() {
        auto client = vm.heap().allocate<TestPageClient>();
        auto page = Web::Page::create(client);
        client->m_page = page.ptr();
        auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
        page->set_top_level_traversable(traversable);
        return GC::Ref { *traversable->active_document() };
    };

    auto target_document = make_document();
    auto target = MUST(target_document->create_element("div"_utf16, Web::Bindings::ElementCreationOptions {}));
    auto observer_client = vm.heap().allocate<TestPageClient>();
    auto observer_page = Web::Page::create(observer_client);
    observer_client->m_page = observer_page.ptr();
    auto observer_page_root = GC::make_root(observer_page);
    GC::Ptr<Web::DOM::Document> observer_document = Web::HTML::HTMLDocument::create(*observer_page, GC::Ref { *target_document->window() });
    auto observer = Web::ResizeObserver::ResizeObserver::create(nullptr, *observer_document);
    auto observer_root = GC::make_root(observer);
    GC::Weak<Web::DOM::Document> observer_document_weak { observer_document };

    observer->observe(*target, {});
    EXPECT(observer->has_activity_root());

    observer_document = nullptr;
    vm.heap().collect_garbage();

    EXPECT(!observer_document_weak);
    EXPECT(!observer->has_activity_root());
}
