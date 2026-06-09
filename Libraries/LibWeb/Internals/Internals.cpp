/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/NumericLimits.h>
#include <LibCore/EventLoop.h>
#include <LibCore/TimeZone.h>
#include <LibGfx/Cursor.h>
#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Reference.h>
#include <LibJS/Runtime/VM.h>
#include <LibURL/Parser.h>
#include <LibWeb/ARIA/AriaData.h>
#include <LibWeb/ARIA/StateAndProperties.h>
#include <LibWeb/Bindings/Internals.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>
#include <LibWeb/HTML/AutoplaySettings.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/TaskQueue.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/InternalGamepad.h>
#include <LibWeb/Internals/Internals.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

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

void Internals::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_gamepads);
}

void Internals::signal_test_is_done(String const& text)
{
    perform_per_test_cleanup();
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

void Internals::gc()
{
    vm().heap().collect_garbage();
}

GC::Ref<WebIDL::Promise> Internals::gc_async()
{
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);

    // Queue a task so that the collection runs outside the JS execution context.
    HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise] {
        HTML::TemporaryExecutionContext execution_context { realm };
        realm.vm().heap().collect_garbage();
        WebIDL::resolve_promise(realm, promise, JS::js_undefined());
    }));

    return promise;
}

WebIDL::ExceptionOr<void> Internals::mark_as_garbage(StringView variable_name)
{
    auto& vm = this->vm();

    // This helper is intentionally limited to real environment bindings. It cannot
    // find values that the bytecode generator stored only in optimized locals.
    // In native functions we don't have a lexical environment so get the outer via the execution stack.
    auto outer_environment = vm.last_execution_context_matching([&](auto* execution_context) {
        return execution_context->lexical_environment != nullptr;
    });
    if (!outer_environment.has_value())
        return vm.throw_completion<JS::ReferenceError>(JS::ErrorType::UnknownIdentifier, variable_name);

    auto reference = TRY(vm.resolve_binding(Utf16FlyString::from_utf8(variable_name), JS::Strict::No, outer_environment.value()->lexical_environment));
    auto value = TRY(reference.get_value(vm));

    if (!JS::can_be_held_weakly(value))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::CannotBeHeldWeakly, value);

    TRY(reference.put_value(vm, JS::js_undefined()));
    (void)TRY(reference.delete_(vm));
    vm.heap().uproot_cell(&value.as_cell());
    return {};
}

WebIDL::ExceptionOr<String> Internals::set_time_zone(StringView time_zone)
{
    auto current_time_zone = Core::TimeZone::current_time_zone();

    if (auto result = Core::TimeZone::set_current_time_zone(time_zone); result.is_error())
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
    auto result = active_document.hit_test({ x, y }, Painting::HitTestType::Exact);
    if (result.has_value()) {
        auto hit_testing_result = JS::Object::create(realm(), nullptr);
        hit_testing_result->define_direct_property("node"_utf16_fly_string, result->dom_node(), JS::default_attributes);
        hit_testing_result->define_direct_property("indexInNode"_utf16_fly_string, JS::Value(result->index_in_node), JS::default_attributes);
        return hit_testing_result;
    }
    return nullptr;
}

struct WebDriverKeyData {
    UIEvents::KeyCode key_code;
    u32 additional_modifiers;
    u32 code_point_to_send;
};

// Maps WebDriver-style key codes (0xE000-0xE05D) to KeyCode and modifiers.
// https://w3c.github.io/webdriver/#keyboard-actions
static constexpr Optional<WebDriverKeyData> webdriver_key_to_key_code(u32 code_point)
{
    switch (code_point) {
    case 0xE003: // Backspace
        return WebDriverKeyData { UIEvents::Key_Backspace, 0, '\b' };
    case 0xE004: // Tab
        return WebDriverKeyData { UIEvents::Key_Tab, 0, '\t' };
    case 0xE006: // Return (main keyboard)
        return WebDriverKeyData { UIEvents::Key_Return, 0, '\n' };
    case 0xE007: // Enter (numpad)
        return WebDriverKeyData { UIEvents::Key_Return, UIEvents::Mod_Keypad, '\n' };
    case 0xE008: // Shift
        return WebDriverKeyData { UIEvents::Key_LeftShift, UIEvents::Mod_Shift, 0 };
    case 0xE009: // Control
        return WebDriverKeyData { UIEvents::Key_LeftControl, UIEvents::Mod_Ctrl, 0 };
    case 0xE00A: // Alt
        return WebDriverKeyData { UIEvents::Key_LeftAlt, UIEvents::Mod_Alt, 0 };
    case 0xE00D: // Space
        return WebDriverKeyData { UIEvents::Key_Space, 0, ' ' };
    case 0xE010: // End
        return WebDriverKeyData { UIEvents::Key_End, 0, 0 };
    case 0xE011: // Home
        return WebDriverKeyData { UIEvents::Key_Home, 0, 0 };
    case 0xE012: // Left Arrow
        return WebDriverKeyData { UIEvents::Key_Left, 0, 0 };
    case 0xE013: // Up Arrow
        return WebDriverKeyData { UIEvents::Key_Up, 0, 0 };
    case 0xE014: // Right Arrow
        return WebDriverKeyData { UIEvents::Key_Right, 0, 0 };
    case 0xE015: // Down Arrow
        return WebDriverKeyData { UIEvents::Key_Down, 0, 0 };
    case 0xE017: // Delete
        return WebDriverKeyData { UIEvents::Key_Delete, 0, 0 };
    case 0xE03D: // Meta
        return WebDriverKeyData { UIEvents::Key_LeftSuper, UIEvents::Mod_Super, 0 };
    }
    return {};
}

void Internals::send_text(HTML::HTMLElement& target, String const& text, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    target.focus();

    for (auto code_point : text.code_points()) {
        if (auto data = webdriver_key_to_key_code(code_point); data.has_value())
            page.handle_keydown(data->key_code, modifiers | data->additional_modifiers, data->code_point_to_send, false, data->code_point_to_send != 0);
        else
            page.handle_keydown(UIEvents::code_point_to_key_code(code_point), modifiers, code_point, false, true);
    }
}

void Internals::send_key(HTML::HTMLElement& target, String const& key_name, WebIDL::UnsignedShort modifiers)
{
    auto key_code = UIEvents::key_code_from_string(key_name);
    target.focus();

    page().handle_keydown(key_code, modifiers, 0, false, false);
}

void Internals::paste(HTML::HTMLElement& target, Utf16String const& text)
{
    auto& page = this->page();
    target.focus();

    page.focused_navigable().paste(text);
}

void Internals::commit_text()
{
    page().handle_keydown(UIEvents::Key_Return, 0, 0x0d, false, true);
}

void Internals::clobber_next_navigation_with_a_traversal()
{
    HTML::Navigable::clobber_next_navigation_with_a_traversal_for_testing();
}

UIEvents::MouseButton Internals::button_from_unsigned_short(WebIDL::UnsignedShort button)
{
    switch (button) {
    case BUTTON_MIDDLE:
        return UIEvents::MouseButton::Middle;
    case BUTTON_RIGHT:
        return UIEvents::MouseButton::Secondary;
    default:
        return UIEvents::MouseButton::Primary;
    }
}

void Internals::mouse_down(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_mousedown(position, position, button_from_unsigned_short(button), 0, modifiers, click_count);
}

void Internals::mouse_up(double x, double y, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_mouseup(position, position, button_from_unsigned_short(button), 0, modifiers);
}

void Internals::mouse_move(double x, double y, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_mousemove(position, position, 0, modifiers);
}

void Internals::mouse_leave()
{
    this->page().handle_mouseleave();
}

void Internals::click(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers)
{
    click_and_hold(x, y, click_count, button, modifiers);
    mouse_up(x, y, button, modifiers);
}

void Internals::click_and_hold(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    auto mouse_button = button_from_unsigned_short(button);
    page.handle_mousedown(position, position, mouse_button, 0, modifiers, click_count);
}

GC::Ref<WebIDL::Promise> Internals::wheel(double x, double y, double delta_x, double delta_y)
{
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);
    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    Optional<AsyncScrollOperation> async_scroll_operation;
    page.handle_mousewheel(position, position, 0, 0, 0, delta_x, delta_y, false, &async_scroll_operation);

    if (async_scroll_operation.has_value() && async_scroll_operation->navigable) {
        async_scroll_operation->navigable->wait_for_async_scroll_operation(async_scroll_operation->operation_id, promise);
        return promise;
    }

    WebIDL::resolve_promise(realm, promise);
    return promise;
}

void Internals::pinch(double x, double y, double scale_delta, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_pinch_event(position, modifiers, scale_delta);
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

String Internals::selected_text_for_clipboard()
{
    return page().focused_navigable().selected_text();
}

void Internals::set_marked_text_from_input_method(Utf16String const& text)
{
    page().focused_navigable().set_marked_text_from_input_method(text);
}

void Internals::commit_text_from_input_method(Utf16String const& text)
{
    page().focused_navigable().commit_text_from_input_method(text);
}

void Internals::unmark_text_from_input_method()
{
    page().focused_navigable().unmark_text_from_input_method();
}

GC::Ptr<Geometry::DOMRect> Internals::current_caret_rect()
{
    auto& active_document = window().associated_document();
    auto rect = active_document.current_caret_rect();
    if (!rect.has_value())
        return nullptr;
    return MUST(Geometry::DOMRect::construct_impl(realm(), static_cast<double>(rect->x()), static_cast<double>(rect->y()), static_cast<double>(rect->width()), static_cast<double>(rect->height())));
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

void Internals::load_url(String const& url_string)
{
    auto url = DOMURL::parse(url_string);

    VERIFY(url.has_value());

    Core::deferred_invoke([page = GC::make_root(page()), url = url.release_value()] {
        page->load(url);
    });
}

GC::Ref<InternalAnimationTimeline> Internals::create_internal_animation_timeline()
{
    auto& realm = this->realm();
    return realm.create<InternalAnimationTimeline>(realm, as<HTML::Window>(realm.global_object()).associated_document());
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

void Internals::expire_cookies_with_time_offset(WebIDL::LongLong seconds)
{
    page().client().page_did_expire_cookies_with_time_offset(AK::Duration::from_seconds(seconds));
}

GC::Ref<WebIDL::Promise> Internals::delete_all_cookies()
{
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);
    auto const& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    page().client().page_did_delete_all_cookies(document.url(), promise);
    return promise;
}

bool Internals::set_http_memory_cache_enabled(bool enabled)
{
    auto was_enabled = Web::Fetch::Fetching::http_memory_cache_enabled();
    Web::Fetch::Fetching::set_http_memory_cache_enabled(enabled);
    return was_enabled;
}

WebIDL::ExceptionOr<void> Internals::set_content_blockers(String const& patterns_source)
{
    Vector<String> patterns;

    for (auto line : patterns_source.bytes_as_string_view().split_view('\n', SplitBehavior::Nothing)) {
        if (line.ends_with('\r'))
            line = line.substring_view(0, line.length() - 1);
        if (line.is_empty())
            continue;

        auto pattern = String::from_utf8(line);
        if (pattern.is_error())
            return vm().throw_completion<JS::InternalError>(MUST(String::formatted("Could not set content blockers: {}", pattern.error())));

        patterns.append(pattern.release_value());
    }

    auto& blocker = ContentBlocker::the();
    auto had_cosmetic_rules = blocker.has_cosmetic_rules();
    auto result = blocker.set_patterns(patterns);
    if (result.is_error())
        return vm().throw_completion<JS::InternalError>(MUST(String::formatted("Could not set content blockers: {}", result.error())));

    if (had_cosmetic_rules || blocker.has_cosmetic_rules())
        page().invalidate_user_style();

    return {};
}

void Internals::set_content_blocking_enabled(bool enabled)
{
    page().set_content_blocking_enabled(enabled);
}

void Internals::set_autoplay_policy(String const& policy)
{
    if (auto parsed = HTML::autoplay_policy_from_string(policy); parsed.has_value())
        HTML::AutoplaySettings::the().set_policy(*parsed, {});
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

void Internals::set_hsts_policy(String const& domain, u64 max_age, bool include_sub_domains)
{
    // NB: Clamp to i64::max so AK::Duration::from_seconds cannot overflow, mirroring the HSTS header parser.
    auto clamped_seconds = AK::min<u64>(max_age, NumericLimits<i64>::max());
    page().client().page_did_store_hsts_policy(domain, HTTP::HSTS::ParsedHSTSPolicy {
                                                           AK::Duration::from_seconds(static_cast<i64>(clamped_seconds)),
                                                           include_sub_domains,
                                                       });
}

void Internals::ingest_hsts_header(String const& url, String const& header_value)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    if (!parsed_url.has_value())
        return;

    ResourceLoader::try_store_hsts_policy_for_url(page(), parsed_url.value(), header_value);
}

bool Internals::is_known_hsts_host(String const& domain)
{
    return ResourceLoader::is_known_hsts_host(page(), domain);
}

void Internals::set_browser_zoom(double factor)
{
    page().client().page_did_set_browser_zoom(factor);
}

void Internals::set_device_pixel_ratio(double ratio)
{
    page().client().page_did_set_device_pixel_ratio_for_testing(ratio);
}

bool Internals::headless()
{
    return page().client().is_headless();
}

String Internals::dump_display_list()
{
    return window().associated_document().dump_display_list();
}

String Internals::dump_accessibility_tree()
{
    return window().associated_document().dump_accessibility_tree_as_json();
}

String Internals::dump_layout_tree(GC::Ref<DOM::Node> node)
{
    node->document().update_layout(DOM::UpdateLayoutReason::Debugging);

    auto* layout_node = node->layout_node();
    if (!layout_node)
        return "(no layout node)"_string;

    StringBuilder builder;
    Web::dump_tree(builder, *layout_node);
    return builder.to_string_without_validation();
}

String Internals::dump_paintable_tree(GC::Ref<DOM::Node> node)
{
    node->document().update_layout(DOM::UpdateLayoutReason::Debugging);

    auto paintable = node->paintable();
    if (!paintable)
        return "(no paintable)"_string;

    StringBuilder builder;
    Web::dump_tree(builder, *paintable);
    return builder.to_string_without_validation();
}

String Internals::dump_stacking_context_tree()
{
    return window().associated_document().dump_stacking_context_tree();
}

String Internals::dump_gc_graph()
{
    return Bindings::main_thread_vm().heap().dump_graph().serialized();
}

String Internals::dump_session_history()
{
    auto& document = window().associated_document();
    auto navigable = document.navigable();
    if (!navigable)
        return "(no navigable)"_string;

    auto traversable = navigable->traversable_navigable();
    if (!traversable)
        return "(no traversable)"_string;

    auto const& entries = navigable->get_session_history_entries();
    auto current_step = traversable->current_session_history_step();

    // Find the minimum step to use as a base offset, so output is stable across test runs.
    Optional<int> min_step;
    for (auto const& entry : entries) {
        auto step = entry->step();
        if (step.has<int>() && (!min_step.has_value() || step.get<int>() < *min_step))
            min_step = step.get<int>();
    }

    StringBuilder builder;
    for (auto const& entry : entries) {
        auto step = entry->step();
        auto const& url = entry->url();
        auto filename = url.basename();
        StringBuilder display_builder;
        display_builder.append(filename);
        if (url.query().has_value())
            display_builder.appendff("?{}", *url.query());
        if (url.fragment().has_value())
            display_builder.appendff("#{}", *url.fragment());
        auto display = display_builder.to_string_without_validation();
        auto is_current = step.has<int>() && step.get<int>() == current_step;
        auto relative_step = step.has<int>() && min_step.has_value() ? String::number(step.get<int>() - *min_step) : "pending"_string;
        builder.appendff("  step {} {}{}\n", relative_step, display, is_current ? " (current)"sv : ""sv);
    }
    return builder.to_string_without_validation();
}

String Internals::dump_ui_process_session_history()
{
    auto& document = window().associated_document();
    if (auto navigable = document.navigable()) {
        if (auto traversable = navigable->traversable_navigable();
            traversable && document.page().client().should_report_session_history_updates()) {
            auto session_history_snapshot = traversable->create_session_history_snapshot();
            return document.page().client().page_did_update_session_history_and_request_ui_process_session_history_for_testing(
                session_history_snapshot.top_level_session_history_entries,
                session_history_snapshot.used_session_history_steps,
                session_history_snapshot.current_used_step_index);
        }
    }

    return document.page().client().page_did_request_ui_process_session_history_for_testing();
}

GC::Ref<WebIDL::Promise> Internals::flush_session_history_traversal_queue()
{
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);
    auto& document = window().associated_document();
    auto navigable = document.navigable();
    if (!navigable) {
        WebIDL::resolve_promise(realm, promise);
        return promise;
    }

    auto traversable = navigable->traversable_navigable();
    if (!traversable) {
        WebIDL::resolve_promise(realm, promise);
        return promise;
    }

    traversable->append_session_history_traversal_steps(GC::create_function(heap(), [&realm, promise](NonnullRefPtr<Core::Promise<Empty>> signal) {
        HTML::TemporaryExecutionContext execution_context { realm };
        WebIDL::resolve_promise(realm, promise);
        signal->resolve({});
    }));
    return promise;
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
    auto gamepad = realm.create<InternalGamepad>(realm, *this);
    m_gamepads.append(gamepad);
    return gamepad;
}

void Internals::disconnect_virtual_gamepad(GC::Ref<InternalGamepad> gamepad)
{
    if (auto index = m_gamepads.find_first_index(gamepad); index.has_value())
        m_gamepads.remove(index.value());
}

void Internals::perform_per_test_cleanup()
{
    // Detach any virtual gamepads
    for (auto gamepad : m_gamepads)
        gamepad->disconnect();
    m_gamepads.clear();

    // Clear any input state
    page().top_level_traversable()->event_handler().clear_per_test_input_state({});
}

void Internals::set_highlighted_node(GC::Ptr<DOM::Node> node)
{
    window().associated_document().set_highlighted_node(node, {});
}

void Internals::clear_element(HTML::HTMLElement& element)
{
    auto& form_associated_element = as<HTML::FormAssociatedElement>(element);
    form_associated_element.clear_algorithm();
}

void Internals::set_environments_top_level_url(StringView url)
{
    auto& realm = *vm().current_realm();
    HTML::principal_realm_settings_object(realm).top_level_creation_url = URL::Parser::basic_parse(url);
}

JS::Object* Internals::get_style_invalidation_counters()
{
    auto const& counters = window().associated_document().style_invalidation_counters();
    auto object = JS::Object::create(realm(), nullptr);
    object->define_direct_property("hasAncestorWalkInvocations"_utf16_fly_string, JS::Value(counters.has_ancestor_walk_invocations), JS::default_attributes);
    object->define_direct_property("hasAncestorWalkVisits"_utf16_fly_string, JS::Value(counters.has_ancestor_walk_visits), JS::default_attributes);
    object->define_direct_property("hasAncestorSiblingElementChecks"_utf16_fly_string, JS::Value(counters.has_ancestor_sibling_element_checks), JS::default_attributes);
    object->define_direct_property("hasInvalidationMetadataCandidates"_utf16_fly_string, JS::Value(counters.has_invalidation_metadata_candidates), JS::default_attributes);
    object->define_direct_property("hasInvalidationRuleCacheBuilds"_utf16_fly_string, JS::Value(counters.has_invalidation_rule_cache_builds), JS::default_attributes);
    object->define_direct_property("hasMatchInvocations"_utf16_fly_string, JS::Value(counters.has_match_invocations), JS::default_attributes);
    object->define_direct_property("hasResultCacheHits"_utf16_fly_string, JS::Value(counters.has_result_cache_hits), JS::default_attributes);
    object->define_direct_property("hasResultCacheMisses"_utf16_fly_string, JS::Value(counters.has_result_cache_misses), JS::default_attributes);
    object->define_direct_property("fullStyleInvalidations"_utf16_fly_string, JS::Value(counters.full_style_invalidations), JS::default_attributes);
    object->define_direct_property("styleInvalidations"_utf16_fly_string, JS::Value(counters.style_invalidations), JS::default_attributes);
    object->define_direct_property("elementStyleRecomputations"_utf16_fly_string, JS::Value(counters.element_style_recomputations), JS::default_attributes);
    object->define_direct_property("elementStyleNoopRecomputations"_utf16_fly_string, JS::Value(counters.element_style_noop_recomputations), JS::default_attributes);
    object->define_direct_property("elementInheritedStyleRecomputations"_utf16_fly_string, JS::Value(counters.element_inherited_style_recomputations), JS::default_attributes);
    object->define_direct_property("elementInheritedStyleNoopRecomputations"_utf16_fly_string, JS::Value(counters.element_inherited_style_noop_recomputations), JS::default_attributes);
    object->define_direct_property("previousSiblingInvalidationWalkVisits"_utf16_fly_string, JS::Value(counters.previous_sibling_invalidation_walk_visits), JS::default_attributes);
    object->define_direct_property("descendantSlotInvalidationSubtreeScans"_utf16_fly_string, JS::Value(counters.descendant_slot_invalidation_subtree_scans), JS::default_attributes);
    object->define_direct_property("mediaRuleEvaluations"_utf16_fly_string, JS::Value(counters.media_rule_evaluations), JS::default_attributes);
    return object;
}

void Internals::reset_style_invalidation_counters()
{
    window().associated_document().reset_style_invalidation_counters();
}

void Internals::update_style()
{
    window().associated_document().update_style();
}

void Internals::set_preferred_color_scheme(StringView color_scheme)
{
    auto preferred_color_scheme = CSS::preferred_color_scheme_from_string(color_scheme);

    Optional<CSS::PreferredColorScheme> preferred_color_scheme_override;
    if (preferred_color_scheme != CSS::PreferredColorScheme::Auto)
        preferred_color_scheme_override = preferred_color_scheme;
    page().set_preferred_color_scheme_override_for_testing(preferred_color_scheme_override);

    auto& document = window().associated_document();
    document.invalidate_style(DOM::StyleInvalidationReason::SettingsChange);
    document.set_needs_media_query_evaluation();
}

String Internals::canvas_color_scheme()
{
    auto& document = window().associated_document();
    document.update_layout(DOM::UpdateLayoutReason::Debugging);
    return MUST(String::from_utf8(CSS::preferred_color_scheme_to_string(document.canvas_color_scheme())));
}

bool Internals::style_sheet_may_have_has_selectors(CSS::CSSStyleSheet& style_sheet)
{
    return style_sheet.selector_insights().has_has_selectors;
}

WebIDL::ExceptionOr<JS::Object*> Internals::image_animation_state_for_url(String const& url)
{
    auto& document = window().associated_document();
    auto parsed_url = document.encoding_parse_url(url);
    if (!parsed_url.has_value())
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = MUST(String::formatted("Invalid URL: '{}'", url)) };

    auto it = document.shared_resource_requests().find(*parsed_url);
    if (it == document.shared_resource_requests().end())
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = MUST(String::formatted("URL doesn't have any associated shared resource requests: '{}'", url)) };

    auto image_data = it->value->image_data();

    if (!image_data)
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = MUST(String::formatted("URL's shared resource request doesn't have any associated image data: '{}'", url)) };

    auto const* animated_bitmap_data = as_if<HTML::AnimatedBitmapDecodedImageData>(*image_data);

    if (!animated_bitmap_data)
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = MUST(String::formatted("URL's associated image is not an animated bitmap: '{}'", url)) };

    auto object = JS::Object::create(realm(), nullptr);

    object->define_direct_property("timerActive"_utf16_fly_string, JS::Value(animated_bitmap_data->m_animation_timer->is_active()), JS::default_attributes);
    object->define_direct_property("sessionID"_utf16_fly_string, JS::Value(static_cast<double>(animated_bitmap_data->m_session_id)), JS::default_attributes);
    object->define_direct_property("frameIndex"_utf16_fly_string, JS::Value(animated_bitmap_data->m_current_frame_index), JS::default_attributes);
    object->define_direct_property("frameCount"_utf16_fly_string, JS::Value(animated_bitmap_data->m_frame_count), JS::default_attributes);
    object->define_direct_property("loopsCompleted"_utf16_fly_string, JS::Value(animated_bitmap_data->m_loops_completed), JS::default_attributes);
    object->define_direct_property("loopCount"_utf16_fly_string, JS::Value(animated_bitmap_data->m_loop_count), JS::default_attributes);
    object->define_direct_property("clientCount"_utf16_fly_string, JS::Value(image_data->m_clients.size()), JS::default_attributes);

    return object.ptr();
}

struct AsyncScrollingStateSnapshot {
    Compositor::AsyncScrollingState state;
    RefPtr<Painting::DisplayList const> display_list;
    Painting::AccumulatedVisualContextTree visual_context_tree;
    RefPtr<Painting::ViewportPaintable> document_paintable;
};

static Optional<AsyncScrollingStateSnapshot> capture_async_scrolling_state(DOM::Document& document)
{
    document.update_layout(DOM::UpdateLayoutReason::InternalsHitTest);
    auto navigable = document.navigable();
    auto document_paintable = document.paintable();
    if (!navigable || !document_paintable)
        return {};
    auto display_list = document.record_display_list(HTML::PaintConfig {}, navigable->display_list_resource_storage());
    if (!display_list)
        return {};
    return AsyncScrollingStateSnapshot {
        .state = Compositor::async_scrolling_state_from_display_list(*display_list),
        .display_list = display_list,
        .visual_context_tree = document_paintable->visual_context_tree(),
        .document_paintable = document_paintable,
    };
}

JS::Object* Internals::async_scrolling_state()
{
    auto object = JS::Object::create(realm(), nullptr);
    auto snapshot = capture_async_scrolling_state(window().associated_document());
    Compositor::AsyncScrollingState empty_state;
    auto const& state = snapshot.has_value() ? snapshot->state : empty_state;

    auto scroll_nodes = MUST(JS::Array::create(realm(), state.scroll_nodes.size()));
    for (size_t i = 0; i < state.scroll_nodes.size(); ++i) {
        auto const& scroll_node = state.scroll_nodes[i];
        auto node = JS::Object::create(realm(), nullptr);
        node->define_direct_property("documentID"_utf16_fly_string, JS::Value(static_cast<double>(scroll_node.node_id.document_id.value())), JS::default_attributes);
        node->define_direct_property("scrollFrameIndex"_utf16_fly_string, JS::Value(scroll_node.node_id.scroll_frame_index.value()), JS::default_attributes);
        node->define_direct_property("parentDocumentID"_utf16_fly_string, JS::Value(scroll_node.parent_node_id.has_value() ? static_cast<double>(scroll_node.parent_node_id->document_id.value()) : 0), JS::default_attributes);
        node->define_direct_property("parentScrollFrameIndex"_utf16_fly_string, JS::Value(scroll_node.parent_node_id.has_value() ? scroll_node.parent_node_id->scroll_frame_index.value() : 0), JS::default_attributes);
        node->define_direct_property("isViewport"_utf16_fly_string, JS::Value(scroll_node.is_viewport), JS::default_attributes);
        MUST(scroll_nodes->create_data_property_or_throw(i, node));
    }

    auto sticky_areas = MUST(JS::Array::create(realm(), state.sticky_areas.size()));
    for (size_t i = 0; i < state.sticky_areas.size(); ++i) {
        auto const& sticky_area = state.sticky_areas[i];
        auto area = JS::Object::create(realm(), nullptr);
        area->define_direct_property("documentID"_utf16_fly_string, JS::Value(static_cast<double>(sticky_area.document_id.value())), JS::default_attributes);
        area->define_direct_property("scrollFrameIndex"_utf16_fly_string, JS::Value(sticky_area.scroll_frame_index.value()), JS::default_attributes);
        area->define_direct_property("parentScrollFrameIndex"_utf16_fly_string, JS::Value(sticky_area.parent_scroll_frame_index.value()), JS::default_attributes);
        area->define_direct_property("nearestScrollingAncestorIndex"_utf16_fly_string, JS::Value(sticky_area.nearest_scrolling_ancestor_index.value()), JS::default_attributes);
        area->define_direct_property("hasTopInset"_utf16_fly_string, JS::Value(sticky_area.inset_top.has_value()), JS::default_attributes);
        area->define_direct_property("hasRightInset"_utf16_fly_string, JS::Value(sticky_area.inset_right.has_value()), JS::default_attributes);
        area->define_direct_property("hasBottomInset"_utf16_fly_string, JS::Value(sticky_area.inset_bottom.has_value()), JS::default_attributes);
        area->define_direct_property("hasLeftInset"_utf16_fly_string, JS::Value(sticky_area.inset_left.has_value()), JS::default_attributes);
        MUST(sticky_areas->create_data_property_or_throw(i, area));
    }

    object->define_direct_property("scrollNodeCount"_utf16_fly_string, JS::Value(state.scroll_nodes.size()), JS::default_attributes);
    object->define_direct_property("scrollNodes"_utf16_fly_string, scroll_nodes, JS::default_attributes);
    object->define_direct_property("stickyAreaCount"_utf16_fly_string, JS::Value(state.sticky_areas.size()), JS::default_attributes);
    object->define_direct_property("stickyAreas"_utf16_fly_string, sticky_areas, JS::default_attributes);
    object->define_direct_property("hasBlockingWheelEventListeners"_utf16_fly_string, JS::Value(state.has_blocking_wheel_event_listeners), JS::default_attributes);
    object->define_direct_property("blockingWheelEventRegionCount"_utf16_fly_string, JS::Value(state.blocking_wheel_event_regions.size()), JS::default_attributes);
    object->define_direct_property("mainThreadWheelEventRegionCount"_utf16_fly_string, JS::Value(state.main_thread_wheel_event_regions.size()), JS::default_attributes);
    object->define_direct_property("wheelEventListenerStateGeneration"_utf16_fly_string, JS::Value(state.wheel_event_listener_state_generation), JS::default_attributes);
    object->define_direct_property("blockingWheelEventRegionsAreCurrent"_utf16_fly_string, JS::Value(state.has_blocking_wheel_event_listeners), JS::default_attributes);
    object->define_direct_property("hasBlockingWheelEventRegionCoveringViewport"_utf16_fly_string, JS::Value(state.has_blocking_wheel_event_region_covering_viewport), JS::default_attributes);
    return object;
}

bool Internals::async_scrolling_state_blocks_wheel_event_at(double x, double y)
{
    auto snapshot = capture_async_scrolling_state(window().associated_document());
    if (!snapshot.has_value())
        return false;
    return Compositor::blocks_wheel_event_at_position(snapshot->state, snapshot->display_list, &snapshot->visual_context_tree, snapshot->document_paintable->scroll_state_snapshot(), { static_cast<float>(x), static_cast<float>(y) });
}

bool Internals::async_scrolling_state_can_wheel_scroll_at(double x, double y, double delta_x, double delta_y, bool force_stale_wheel_event_regions)
{
    return async_scrolling_state_wheel_scroll_admission_at(x, y, delta_x, delta_y, force_stale_wheel_event_regions) == "accepted"sv;
}

String Internals::async_scrolling_state_wheel_routing_admission()
{
    auto snapshot = capture_async_scrolling_state(window().associated_document());
    auto admission = snapshot.has_value() ? Compositor::wheel_routing_admission_for(snapshot->state) : Compositor::WheelRoutingAdmission::NoAsyncScrollingState;
    return String::from_utf8_without_validation(Compositor::wheel_routing_admission_to_string(admission).bytes());
}

static String wheel_scroll_admission_to_string(Compositor::WheelScrollAdmission admission)
{
    switch (admission) {
    case Compositor::WheelScrollAdmission::Accepted:
        return "accepted"_string;
    case Compositor::WheelScrollAdmission::NoScrollableTarget:
        return "no-scrollable-target"_string;
    case Compositor::WheelScrollAdmission::BlockedByMainThreadRegion:
        return "blocked-by-main-thread-region"_string;
    case Compositor::WheelScrollAdmission::StaleBlockingWheelEventRegions:
        return "stale-blocking-wheel-event-regions"_string;
    case Compositor::WheelScrollAdmission::BlockedByWheelEventRegion:
        return "blocked-by-wheel-event-region"_string;
    }
    VERIFY_NOT_REACHED();
}

String Internals::async_scrolling_state_wheel_scroll_admission_at(double x, double y, double delta_x, double delta_y, bool force_stale_wheel_event_regions)
{
    auto snapshot = capture_async_scrolling_state(window().associated_document());
    if (!snapshot.has_value())
        return "no-scrollable-target"_string;
    auto admission = Compositor::admit_wheel_scroll(
        snapshot->state,
        snapshot->display_list,
        &snapshot->visual_context_tree,
        snapshot->document_paintable->scroll_state_snapshot(),
        { static_cast<float>(x), static_cast<float>(y) },
        { static_cast<float>(delta_x), static_cast<float>(delta_y) },
        snapshot->state.has_blocking_wheel_event_listeners && !force_stale_wheel_event_regions);
    return wheel_scroll_admission_to_string(admission);
}

String Internals::async_scrolling_state_wheel_target_at(double x, double y, double delta_x, double delta_y)
{
    auto snapshot = capture_async_scrolling_state(window().associated_document());
    if (!snapshot.has_value())
        return "none"_string;

    Compositor::AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(snapshot->state));
    scroll_tree.rebuild_wheel_hit_test_targets(snapshot->display_list, &snapshot->visual_context_tree, snapshot->document_paintable->scroll_state_snapshot());

    auto target = scroll_tree.hit_test_scroll_node_for_wheel(
        { static_cast<float>(x), static_cast<float>(y) },
        { static_cast<float>(delta_x), static_cast<float>(delta_y) });
    if (target.blocked_by_main_thread_region || target.blocked_by_wheel_event_region || !target.node_id.has_value())
        return "none"_string;
    if (scroll_tree.scroll_node_is_viewport(*target.node_id))
        return "viewport"_string;
    return "non-viewport"_string;
}

}
