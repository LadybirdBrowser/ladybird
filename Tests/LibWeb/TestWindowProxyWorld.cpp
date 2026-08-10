/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace {

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

GC_DEFINE_ALLOCATOR(TestPageClient);

}

TEST_CASE(per_world_windowproxy_and_window_wrapper)
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
    auto document = GC::Ref { *traversable->active_document() };
    auto browsing_context = GC::Ref { *document->browsing_context() };
    auto window = document->window();

    auto& main_wrapper = Web::Bindings::platform_object_for_window(*window, window->principal_realm());
    auto main_proxy = browsing_context->window_proxy();

    auto run_script = [&](JS::Realm& realm, Utf16View source) {
        auto script_or_error = JS::Script::parse(source, realm);
        VERIFY(!script_or_error.is_error());
        auto result = vm.run(script_or_error.value());
        VERIFY(!result.is_throw_completion());
        return result.release_value();
    };

    auto entries_before_extension_init = run_script(window->principal_realm(), "performance.clearMarks(); performance.mark('before-extension-init'); performance.getEntriesByType('mark').length;"_utf16);
    EXPECT(entries_before_extension_init.is_number());
    EXPECT_EQ(entries_before_extension_init.as_double(), 1);

    auto extension_world = vm.heap().allocate<Web::Bindings::WrapperWorld>(Web::Bindings::WrapperWorld::Type::Extension);
    auto extension_execution_context = Web::Bindings::create_a_new_javascript_realm(
        vm,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return Web::Bindings::create_global_object_wrapper(realm, GC::Ref { *window });
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return *browsing_context->window_proxy_for(*extension_world, realm);
        });
    auto& extension_realm = *extension_execution_context->realm;
    auto intrinsics = extension_realm.create<Web::Bindings::Intrinsics>(extension_realm);
    extension_realm.set_host_defined(make<Web::Bindings::HostDefined>(intrinsics, *extension_world, window->principal_realm()));
    Web::Bindings::cache_global_object_wrapper(extension_realm);
    MUST(Web::Bindings::initialize_window_web_interfaces(*window, extension_realm));

    auto entries_after_extension_init = run_script(window->principal_realm(), "performance.getEntriesByType('mark').length;"_utf16);
    EXPECT(entries_after_extension_init.is_number());
    EXPECT_EQ(entries_after_extension_init.as_double(), 1);

    IGNORE_USE_IN_ESCAPING_LAMBDA auto extension_proxy = browsing_context->window_proxy_for(*extension_world, extension_realm);
    EXPECT(extension_proxy);
    EXPECT(extension_proxy != main_proxy);
    EXPECT(extension_proxy == browsing_context->window_proxy_for(*extension_world, extension_realm));
    EXPECT(extension_proxy->window().ptr() == window.ptr());
    EXPECT(&Web::Bindings::this_value_realm(extension_realm, extension_proxy) == &extension_realm);

    auto& extension_wrapper = Web::Bindings::platform_object_for_window(*window, extension_realm);
    EXPECT(&extension_wrapper != &main_wrapper);
    extension_wrapper.define_direct_property("extension-marker"_utf16_fly_string, JS::Value(true), JS::default_attributes);
    EXPECT(!MUST(main_wrapper.internal_get_own_property(JS::PropertyKey { "extension-marker"_utf16_fly_string })).has_value());

    vm.push_execution_context(*extension_execution_context);

    auto proxy_marker = JS::PropertyKey { "proxy-marker"_utf16_fly_string };
    EXPECT(MUST(extension_proxy->internal_set(proxy_marker, JS::Value(123), JS::Value(extension_proxy), nullptr, JS::Object::PropertyLookupPhase::OwnProperty)));
    EXPECT(MUST(extension_proxy->internal_get(proxy_marker, JS::Value(extension_proxy), nullptr, JS::Object::PropertyLookupPhase::OwnProperty)).as_i32() == 123);
    EXPECT(MUST(main_proxy->internal_get(proxy_marker, JS::Value(main_proxy), nullptr, JS::Object::PropertyLookupPhase::OwnProperty)).is_undefined());

    for (auto const& property_name : { "self"_utf16_fly_string, "top"_utf16_fly_string, "parent"_utf16_fly_string }) {
        auto value = MUST(extension_proxy->internal_get(JS::PropertyKey { property_name }, JS::Value(extension_proxy), nullptr, JS::Object::PropertyLookupPhase::OwnProperty));
        EXPECT(value.is_object());
        EXPECT(&value.as_object() == extension_proxy);
        EXPECT(&value.as_object().shape().realm() == &extension_realm);
    }

    IGNORE_USE_IN_ESCAPING_LAMBDA bool listener_was_called = false;
    auto callback_function = JS::NativeFunction::create(
        extension_realm,
        [&](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            listener_was_called = true;

            auto this_value = vm.this_value();
            EXPECT(this_value.is_object());
            EXPECT(&this_value.as_object() == extension_proxy);
            EXPECT(&this_value.as_object().shape().realm() == &extension_realm);

            auto event_value = vm.argument(0);
            EXPECT(event_value.is_object());
            EXPECT(&event_value.as_object().shape().realm() == &extension_realm);

            auto target = TRY(event_value.as_object().internal_get(JS::PropertyKey { "target"_utf16_fly_string }, event_value));
            EXPECT(target.is_object());
            EXPECT(&target.as_object().shape().realm() == &extension_realm);

            auto current_target = TRY(event_value.as_object().internal_get(JS::PropertyKey { "currentTarget"_utf16_fly_string }, event_value));
            EXPECT(current_target.is_object());
            EXPECT(&current_target.as_object() == extension_proxy);
            EXPECT(&current_target.as_object().shape().realm() == &extension_realm);

            return JS::js_undefined();
        },
        1);
    auto& principal_settings = Web::Bindings::principal_host_defined_environment_settings_object(window->principal_realm());
    auto callback = GC::Heap::the().allocate<Web::WebIDL::CallbackType>(*callback_function, principal_settings);
    window->add_event_listener_without_options("windowproxy-test"_utf16_fly_string, *Web::DOM::IDLEventListener::create(*callback));

    auto event = Web::DOM::Event::create(extension_realm.global_object(), "windowproxy-test"_fly_string);
    EXPECT(window->dispatch_event(*event));
    EXPECT(listener_was_called);

    auto* popped_extension_execution_context = vm.pop_execution_context();
    EXPECT(popped_extension_execution_context == extension_execution_context.ptr());

    auto replacement_window = Web::HTML::Window::create();
    browsing_context->set_active_window(replacement_window);
    EXPECT(main_proxy->window().ptr() == replacement_window.ptr());
    EXPECT(extension_proxy->window().ptr() == replacement_window.ptr());

    vm.pop_execution_context();
}
