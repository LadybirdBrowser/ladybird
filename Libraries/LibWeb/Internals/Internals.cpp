/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibGfx/Cursor.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/VM.h>
#include <LibUnicode/TimeZone.h>
#include <LibWeb/ARIA/AriaData.h>
#include <LibWeb/ARIA/StateAndProperties.h>
#include <LibWeb/Bindings/InternalsPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/InternalGamepad.h>
#include <LibWeb/Internals/Internals.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Internals {

static u16 s_echo_server_port { 0 };

GC_DEFINE_ALLOCATOR(Internals);

Internals::Internals(JS::Realm& realm)
    : InternalsBase(realm)
{
}

Internals::~Internals() = default;

void Internals::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Internals);
    Base::initialize(realm);
}

void Internals::signal_test_is_done(String const& text)
{
    page().client().page_did_finish_test(text);
}

void Internals::set_test_timeout(double milliseconds)
{
    page().client().page_did_set_test_timeout(milliseconds);
}

// https://web-platform-tests.org/writing-tests/reftests.html#components-of-a-reftest
WebIDL::ExceptionOr<void> Internals::load_reference_test_metadata()
{
    auto& vm = this->vm();
    auto& page = this->page();

    auto* document = page.top_level_browsing_context().active_document();
    if (!document)
        return vm.throw_completion<JS::InternalError>("No active document available"sv);

    JsonObject metadata;

    // Collect all <link rel="match"> and <link rel="mismatch"> references.
    auto collect_references = [&vm, &document](StringView type) -> WebIDL::ExceptionOr<JsonArray> {
        JsonArray references;
        auto reference_nodes = TRY(document->query_selector_all(MUST(String::formatted("link[rel={}]", type))));
        for (size_t i = 0; i < reference_nodes->length(); ++i) {
            auto const* reference_node = reference_nodes->item(i);
            auto href = as<DOM::Element>(reference_node)->get_attribute_value(HTML::AttributeNames::href);
            auto url = document->encoding_parse_url(href);
            if (!url.has_value())
                return vm.throw_completion<JS::InternalError>(MUST(String::formatted("Failed to construct URL for '{}'", href)));
            references.must_append(url->to_string());
        }
        return references;
    };
    metadata.set("match_references"sv, TRY(collect_references("match"sv)));
    metadata.set("mismatch_references"sv, TRY(collect_references("mismatch"sv)));

    // Collect all <meta name="fuzzy" content=".."> values.
    JsonArray fuzzy_configurations;
    auto fuzzy_nodes = TRY(document->query_selector_all("meta[name=fuzzy]"sv));
    for (size_t i = 0; i < fuzzy_nodes->length(); ++i) {
        auto const* fuzzy_node = fuzzy_nodes->item(i);
        auto content = as<DOM::Element>(fuzzy_node)->get_attribute_value(HTML::AttributeNames::content);

        JsonObject fuzzy_configuration;
        if (content.contains(':')) {
            auto content_parts = MUST(content.split_limit(':', 2));
            auto reference_url = document->encoding_parse_url(content_parts[0]);
            fuzzy_configuration.set("reference"sv, reference_url->to_string());
            content = content_parts[1];
        }
        fuzzy_configuration.set("content"sv, content);

        fuzzy_configurations.must_append(fuzzy_configuration);
    }
    metadata.set("fuzzy"sv, fuzzy_configurations);

    page.client().page_did_receive_reference_test_metadata(metadata);
    return {};
}

// https://web-platform-tests.org/writing-tests/testharness.html#variants
WebIDL::ExceptionOr<void> Internals::load_test_variants()
{
    auto& page = this->page();

    auto* document = page.top_level_browsing_context().active_document();
    if (!document)
        return vm().throw_completion<JS::InternalError>("No active document available"sv);

    auto variant_nodes = TRY(document->query_selector_all("meta[name=variant]"sv));

    JsonArray variants;
    for (size_t i = 0; i < variant_nodes->length(); ++i) {
        auto const* variant_node = variant_nodes->item(i);
        auto content = as<DOM::Element>(variant_node)->get_attribute_value(HTML::AttributeNames::content);
        variants.must_append(content);
    }

    // Always fire callback so test runner knows variant check is complete.
    page.client().page_did_receive_test_variant_metadata(variants);
    return {};
}

void Internals::gc()
{
    vm().heap().collect_garbage();
}

WebIDL::ExceptionOr<String> Internals::set_time_zone(StringView time_zone)
{
    auto current_time_zone = Unicode::current_time_zone();

    if (auto result = Unicode::set_current_time_zone(time_zone); result.is_error())
        return vm().throw_completion<JS::InternalError>(MUST(String::formatted("Could not set time zone: {}", result.error())));

    JS::clear_system_time_zone_cache();
    return current_time_zone;
}

JS::Object* Internals::hit_test(double x, double y)
{
    auto& active_document = window().associated_document();
    // NOTE: Force a layout update just before hit testing. This is because the current layout tree, which is required
    //       for stacking context traversal, might not exist if this call occurs between the tear_down_layout_tree()
    //       and update_layout() calls
    active_document.update_layout(DOM::UpdateLayoutReason::InternalsHitTest);
    auto result = active_document.paintable_box()->hit_test({ x, y }, Painting::HitTestType::Exact);
    if (result.has_value()) {
        auto hit_testing_result = JS::Object::create(realm(), nullptr);
        hit_testing_result->define_direct_property("node"_utf16_fly_string, result->dom_node(), JS::default_attributes);
        hit_testing_result->define_direct_property("indexInNode"_utf16_fly_string, JS::Value(result->index_in_node), JS::default_attributes);
        return hit_testing_result;
    }
    return nullptr;
}

void Internals::send_text(HTML::HTMLElement& target, String const& text, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    target.focus();

    for (auto code_point : text.code_points())
        page.handle_keydown(UIEvents::code_point_to_key_code(code_point), modifiers, code_point, false);
}

void Internals::send_key(HTML::HTMLElement& target, String const& key_name, WebIDL::UnsignedShort modifiers)
{
    auto key_code = UIEvents::key_code_from_string(key_name);
    target.focus();

    page().handle_keydown(key_code, modifiers, 0, false);
}

void Internals::paste(HTML::HTMLElement& target, Utf16String const& text)
{
    auto& page = this->page();
    target.focus();

    page.focused_navigable().paste(text);
}

void Internals::commit_text()
{
    page().handle_keydown(UIEvents::Key_Return, 0, 0x0d, false);
}

void Internals::click(double x, double y)
{
    click(x, y, UIEvents::MouseButton::Primary);
}

void Internals::doubleclick(double x, double y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_doubleclick(position, position, UIEvents::MouseButton::Primary, 0, 0);
}

void Internals::middle_click(double x, double y)
{
    click(x, y, UIEvents::MouseButton::Middle);
}

void Internals::click(double x, double y, UIEvents::MouseButton button)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_mousedown(position, position, button, 0, 0);
    page.handle_mouseup(position, position, button, 0, 0);
}

void Internals::mouse_down(double x, double y)
{
    mouse_down(x, y, UIEvents::MouseButton::Primary);
}

void Internals::mouse_down(double x, double y, UIEvents::MouseButton button)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_mousedown(position, position, button, 0, 0);
}

void Internals::move_pointer_to(double x, double y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_mousemove(position, position, 0, 0);
}

void Internals::wheel(double x, double y, double delta_x, double delta_y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_mousewheel(position, position, 0, 0, 0, delta_x, delta_y);
}

void Internals::pinch(double x, double y, double scale_delta)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_pinch_event(position, scale_delta);
}

String Internals::current_cursor()
{
    auto& page = this->page();

    return page.current_cursor().visit(
        [](Gfx::StandardCursor cursor) {
            auto cursor_string = Gfx::standard_cursor_to_string(cursor);
            return String::from_utf8_without_validation(cursor_string.bytes());
        },
        [](Gfx::ImageCursor const&) {
            return "Image"_string;
        });
}

WebIDL::ExceptionOr<bool> Internals::dispatch_user_activated_event(DOM::EventTarget& target, DOM::Event& event)
{
    event.set_is_trusted(true);
    return target.dispatch_event(event);
}

void Internals::spoof_current_url(String const& url_string)
{
    auto url = DOMURL::parse(url_string);

    VERIFY(url.has_value());

    auto origin = url->origin();

    auto& window = this->window();
    window.associated_document().set_url(url.value());
    window.associated_document().set_origin(origin);
    HTML::relevant_settings_object(window.associated_document()).creation_url = url.release_value();
}

GC::Ref<InternalAnimationTimeline> Internals::create_internal_animation_timeline()
{
    auto& realm = this->realm();
    return realm.create<InternalAnimationTimeline>(realm);
}

void Internals::simulate_drag_start(double x, double y, String const& name, String const& contents)
{
    Vector<HTML::SelectedFile> files;
    files.empend(name.to_byte_string(), MUST(ByteBuffer::copy(contents.bytes())));

    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_drag_and_drop_event(DragEvent::Type::DragStart, position, position, UIEvents::MouseButton::Primary, 0, 0, move(files));
}

void Internals::simulate_drag_move(double x, double y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_drag_and_drop_event(DragEvent::Type::DragMove, position, position, UIEvents::MouseButton::Primary, 0, 0, {});
}

void Internals::simulate_drop(double x, double y)
{
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_drag_and_drop_event(DragEvent::Type::Drop, position, position, UIEvents::MouseButton::Primary, 0, 0, {});
}

void Internals::enable_cookies_on_file_domains()
{
    window().associated_document().enable_cookies_on_file_domains({});
}

void Internals::expire_cookies_with_time_offset(WebIDL::LongLong seconds)
{
    page().client().page_did_expire_cookies_with_time_offset(AK::Duration::from_seconds(seconds));
}

bool Internals::set_http_memory_cache_enabled(bool enabled)
{
    auto was_enabled = Web::Fetch::Fetching::http_memory_cache_enabled();
    Web::Fetch::Fetching::set_http_memory_cache_enabled(enabled);
    return was_enabled;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static
String Internals::get_computed_role(DOM::Element& element)
{
    if (auto role = element.role_or_default(); role.has_value())
        return MUST(String::from_utf8(ARIA::role_name(role.value())));
    return String {};
}

String Internals::get_computed_label(DOM::Element& element)
{
    auto& active_document = window().associated_document();
    return MUST(element.accessible_name(active_document));
}

String Internals::get_computed_aria_level(DOM::Element& element)
{
    auto aria_data = MUST(ARIA::AriaData::build_data(element));
    return MUST(ARIA::state_or_property_to_string_value(ARIA::StateAndProperties::AriaLevel, *aria_data));
}

u16 Internals::get_echo_server_port()
{
    return s_echo_server_port;
}

void Internals::set_echo_server_port(u16 const port)
{
    s_echo_server_port = port;
}

void Internals::set_browser_zoom(double factor)
{
    page().client().page_did_set_browser_zoom(factor);
}

bool Internals::headless()
{
    return page().client().is_headless();
}

String Internals::dump_display_list()
{
    return window().associated_document().dump_display_list();
}

String Internals::dump_gc_graph()
{
    return Bindings::main_thread_vm().heap().dump_graph().serialized();
}

GC::Ptr<DOM::ShadowRoot> Internals::get_shadow_root(GC::Ref<DOM::Element> element)
{
    return element->shadow_root();
}

void Internals::handle_sdl_input_events()
{
    page().handle_sdl_input_events();
}

GC::Ref<InternalGamepad> Internals::connect_virtual_gamepad()
{
    auto& realm = this->realm();
    return realm.create<InternalGamepad>(realm);
}

}
