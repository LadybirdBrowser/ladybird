/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Find.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/NumericLimits.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCore/EventLoop.h>
#include <LibCore/TimeZone.h>
#include <LibGC/Heap.h>
#include <LibGfx/Cursor.h>
#include <LibHTTP/HSTS/ParsedHSTSPolicy.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Environment.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>
#include <LibMediaClient/Client.h>
#include <LibURL/Parser.h>
#include <LibWeb/ARIA/AriaData.h>
#include <LibWeb/ARIA/StateAndProperties.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Bindings/Internals.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Node.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/CSSGroupingRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/Compositor/CompositorFrame.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/Compositor/RenderClock.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Editing/SpellingMarkers.h>
#include <LibWeb/Editing/WritingSuggestionController.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>
#include <LibWeb/HTML/AutoplaySettings.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/TaskQueue.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HistoryExecutor.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/HTML/TextDirectiveGenerator.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Internals/InternalGamepad.h>
#include <LibWeb/Internals/Internals.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/HitTestResult.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/Scrolling.h>
#include <LibWeb/PictureInPicture/PictureInPictureController.h>
#include <LibWeb/ResizeObserver/ResizeObserver.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebSockets/WebSocket.h>
#include <LibWeb/XHR/XMLHttpRequest.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>
#include <LibWebCommon/Clipboard/SystemClipboard.h>
#include <LibWebCommon/Page/DragEvent.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/StorageAPI/StorageEndpoint.h>

namespace Web::Internals {

static u16 s_echo_server_port { 0 };
static u16 s_https_echo_server_port { 0 };

static Web::WheelDeltaPrecision wheel_delta_precision_from(bool precise)
{
    return precise ? Web::WheelDeltaPrecision::Precise : Web::WheelDeltaPrecision::Discrete;
}

static Web::ScrollGesturePhase scroll_gesture_phase_from(Bindings::ScrollGesturePhase scroll_gesture_phase)
{
    switch (scroll_gesture_phase) {
    case Bindings::ScrollGesturePhase::None:
        return Web::ScrollGesturePhase::None;
    case Bindings::ScrollGesturePhase::Ongoing:
        return Web::ScrollGesturePhase::Ongoing;
    case Bindings::ScrollGesturePhase::Momentum:
        return Web::ScrollGesturePhase::Momentum;
    case Bindings::ScrollGesturePhase::Ended:
        return Web::ScrollGesturePhase::Ended;
    }
    VERIFY_NOT_REACHED();
}

static double wheel_tick_120ths_from(Optional<double> wheel_ticks, double pixel_delta, Web::WheelDeltaPrecision wheel_delta_precision)
{
    static constexpr double wheel_tick_120ths_per_tick = 120;
    if (wheel_ticks.has_value())
        return *wheel_ticks * wheel_tick_120ths_per_tick;
    return Web::wheel_tick_120ths_from_pixel_delta(pixel_delta, wheel_delta_precision);
}

GC_DEFINE_ALLOCATOR(Internals);

static GC::Ptr<HTML::LocalNavigable> local_root_of(HTML::Window const& window)
{
    if (auto navigable = window.navigable())
        return navigable->local_root();
    return {};
}

Internals::Internals(HTML::Window& window)
    : InternalsBase(window)
{
}

Internals::~Internals() = default;

void Internals::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_gamepads);
}

void Internals::signal_test_is_done(Utf16String const& text)
{
    perform_per_test_cleanup();
    page().client().page_did_finish_test(text);
}

void Internals::set_test_timeout(double milliseconds)
{
    page().client().page_did_set_test_timeout(milliseconds);
}

void Internals::force_incompatible_visual_context_tree_rebuild()
{
    auto& document = window().associated_document();
    if (!document.has_committed_viewport_box())
        return;
    document.schedule_full_accumulated_visual_context_rebuild(Layout::RustFFI::VisualContextUpdateScope::FreshTree);
}

u64 Internals::accumulated_visual_context_incremental_update_count()
{
    auto& document = window().associated_document();
    if (!document.has_committed_viewport_box())
        return 0;
    return document.paint_state().accumulated_visual_context_tree_incremental_update_count();
}

u64 Internals::layout_tree_pre_order_label_violation_count()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto& document = window().associated_document();
    return Layout::RustFFI::render_state_pre_order_label_violation_count(
        document.layout_node_arena().host(), Painting::viewport_row_slot(read, document));
}

u64 Internals::layout_tree_pre_order_relabel_count()
{
    auto& document = window().associated_document();
    return Layout::RustFFI::render_state_layout_counts(document.layout_node_arena().host()).pre_order_relabels;
}

u64 Internals::visual_context_tree_node_count()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto& document = window().associated_document();
    if (!document.has_committed_viewport_box() || !document.paint_state().has_visual_context_tree(read))
        return 0;
    auto visual_context_tree = document.paint_state().visual_context_tree(document);
    return visual_context_tree.live_node_count();
}

u64 Internals::visual_context_tree_dead_node_count()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto& document = window().associated_document();
    if (!document.has_committed_viewport_box() || !document.paint_state().has_visual_context_tree(read))
        return 0;
    auto visual_context_tree = document.paint_state().visual_context_tree(document);
    return visual_context_tree.node_count() - visual_context_tree.live_node_count();
}

u64 Internals::visual_context_tree_structural_epoch()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto& document = window().associated_document();
    if (!document.has_committed_viewport_box() || !document.paint_state().has_visual_context_tree(read))
        return 0;
    return document.paint_state().visual_context_tree(document).structural_epoch();
}

u64 Internals::visual_context_tree_node_capacity()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto& document = window().associated_document();
    if (!document.has_committed_viewport_box() || !document.paint_state().has_visual_context_tree(read))
        return 0;
    auto visual_context_tree = document.paint_state().visual_context_tree(document);
    return visual_context_tree.node_count();
}

GC::Ref<JS::Object> Internals::visual_context_node_indices(DOM::Element& element)
{
    auto& realm = window().principal_realm();
    auto& document = window().associated_document();
    Layout::ForcedReadScope read { document };
    document.update_layout(DOM::UpdateLayoutReason::Debugging);
    document.update_paint_and_hit_testing_properties_if_needed();
    auto const* layout_node = element.layout_node(read);
    auto owned_indices_as_array = [&](Layout::RustFFI::FfiVisualContextBoxNodeList list) -> GC::Ref<JS::Array> {
        Vector<u32> indices;
        if (layout_node)
            indices = Painting::rust_owned_visual_context_node_indices(*layout_node, list);
        return JS::Array::create_from<u32>(realm, indices.span(), [](u32 index) { return JS::Value { index }; });
    };
    auto object = JS::Object::create(realm, nullptr);
    object->define_direct_property("spatial"_utf16_fly_string, owned_indices_as_array(Layout::RustFFI::FfiVisualContextBoxNodeList::SpatialNodes), JS::default_attributes);
    object->define_direct_property("clips"_utf16_fly_string, owned_indices_as_array(Layout::RustFFI::FfiVisualContextBoxNodeList::ClipNodes), JS::default_attributes);
    object->define_direct_property("effects"_utf16_fly_string, owned_indices_as_array(Layout::RustFFI::FfiVisualContextBoxNodeList::EffectNodes), JS::default_attributes);
    object->define_direct_property("needsCompositorEffectsLayer"_utf16_fly_string, JS::Value(layout_node && layout_node->needs_compositor_effects_layer()), JS::default_attributes);
    object->define_direct_property("needsCompositorBackgroundColorFrame"_utf16_fly_string, JS::Value(layout_node && layout_node->needs_compositor_background_color_frame()), JS::default_attributes);
    Vector<Gfx::Color> background_color_animation_keyframe_colors;
    if (layout_node) {
        Layout::BegunRead const& begun_read = read;
        for (auto effect_node_index : Painting::rust_owned_visual_context_node_indices(*layout_node, Layout::RustFFI::FfiVisualContextBoxNodeList::EffectNodes)) {
            Layout::RustFFI::render_state_background_color_animation_keyframe_colors(document.layout_node_arena().host(), &begun_read, effect_node_index, &background_color_animation_keyframe_colors, [](void* colors, Gfx::Color color) {
                static_cast<Vector<Gfx::Color>*>(colors)->append(color);
            });
        }
    }
    object->define_direct_property("backgroundColorAnimationKeyframes"_utf16_fly_string,
        JS::Array::create_from<Gfx::Color>(realm, background_color_animation_keyframe_colors.span(), [&](Gfx::Color const& color) {
            return JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(color.to_string()));
        }),
        JS::default_attributes);
    return object;
}

void Internals::set_force_dark_enabled(bool enabled)
{
    auto& document = window().associated_document();
    if (auto navigable = document.navigable())
        navigable->set_force_dark_enabled(enabled);
}

void Internals::set_show_line_box_borders(bool enabled)
{
    auto& document = window().associated_document();
    if (auto navigable = document.navigable())
        navigable->set_should_show_line_box_borders(enabled);
}

void Internals::set_force_dark_thresholds(i32 foreground, i32 background)
{
    auto& document = window().associated_document();
    if (auto navigable = document.navigable())
        navigable->set_force_dark_thresholds(foreground, background);
}

void Internals::send_mismatched_visual_context_tree_update_to_compositor()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto& document = window().associated_document();
    auto navigable = document.navigable();
    if (!navigable || !navigable->has_compositor_context())
        return;
    if (!document.has_committed_viewport_box() || !document.paint_state().has_visual_context_tree(read))
        return;

    // Force a fresh, incompatible rebuild — so the tree is minted with a new structural epoch that the Compositor's installed
    // display list was never recorded against.
    document.schedule_full_accumulated_visual_context_rebuild(Layout::RustFFI::VisualContextUpdateScope::FreshTree);
    document.update_paint_and_hit_testing_properties_if_needed();

    // Send a visual-context-tree update carrying that new structural epoch *without* re-recording the display list —
    // deliberately reproducing the peer inconsistency behind issue #10368.
    navigable->commit_mismatched_visual_context_tree_for_testing();
}

// https://web-platform-tests.org/writing-tests/reftests.html#components-of-a-reftest
WebIDL::ExceptionOr<void> Internals::load_reference_test_metadata()
{
    auto& vm = window().principal_realm().vm();
    auto& page = this->page();

    auto local_root = local_root_of(window());
    auto document = local_root ? local_root->active_document() : nullptr;
    if (!document)
        return vm.throw_completion<JS::InternalError>("No active document available"_utf16);

    JsonObject metadata;

    // Collect all <link rel="match"> and <link rel="mismatch"> references.
    auto collect_references = [&vm, &document](StringView type) -> WebIDL::ExceptionOr<JsonArray> {
        JsonArray references;
        auto reference_nodes = TRY(document->query_selector_all(Utf16String::formatted("link[rel={}]", type)));
        for (size_t i = 0; i < reference_nodes->length(); ++i) {
            auto const* reference_node = reference_nodes->item(i);
            auto href = as<DOM::Element>(reference_node)->get_attribute_value(HTML::AttributeNames::href);
            auto url = document->encoding_parse_url(href);
            if (!url.has_value())
                return vm.throw_completion<JS::InternalError>(Utf16String::formatted("Failed to construct URL for '{}'", href));
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
        if (auto colon_offset = content.find_code_unit_offset(':'); colon_offset.has_value()) {
            auto reference_url = document->encoding_parse_url(content.substring_view(0, *colon_offset));
            fuzzy_configuration.set("reference"sv, reference_url->to_string());
            content = Utf16String::from_utf16(content.substring_view(*colon_offset + 1));
        }
        fuzzy_configuration.set("content"sv, content.to_utf8());

        fuzzy_configurations.must_append(fuzzy_configuration);
    }
    metadata.set("fuzzy"sv, fuzzy_configurations);

    page.client().page_did_receive_reference_test_metadata(metadata);
    return {};
}

void Internals::gc()
{
    window().principal_realm().vm().heap().collect_garbage();
}

void Internals::gc_async(GC::Ref<WebIDL::Promise> promise)
{
    // Queue a LibCore event to run the collection, so that it runs outside the JS execution context with as small a
    // stack as possible.
    Core::deferred_invoke([promise = GC::make_root(promise)] {
        auto& realm = WebIDL::promise_realm(*promise);
        HTML::TemporaryExecutionContext execution_context { realm };
        realm.vm().heap().collect_garbage();
        WebIDL::resolve_promise(*promise);
    });
}

WebIDL::ExceptionOr<void> Internals::mark_as_garbage(Utf16String const& variable_name)
{
    auto& vm = window().principal_realm().vm();
    auto variable_name_fly = Utf16FlyString::from_utf16(variable_name.utf16_view());

    // This helper is intentionally limited to real environment bindings. It cannot
    // find values that the bytecode generator stored only in optimized locals.
    // In native functions we don't have a lexical environment so get the outer via the execution stack.
    auto outer_environment = vm.last_execution_context_matching([&](auto* execution_context) {
        return execution_context->lexical_environment != nullptr;
    });
    if (!outer_environment.has_value())
        return vm.throw_completion<JS::ReferenceError>(JS::ErrorType::UnknownIdentifier, variable_name_fly);

    auto environment = TRY(vm.resolve_binding(variable_name_fly, JS::Strict::No, outer_environment.value()->lexical_environment));
    if (!environment)
        return vm.throw_completion<JS::ReferenceError>(JS::ErrorType::UnknownIdentifier, variable_name_fly);
    auto value = TRY(environment->get_binding_value(vm, variable_name_fly, false));

    if (!JS::can_be_held_weakly(value))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::CannotBeHeldWeakly, value);

    TRY(environment->set_mutable_binding(vm, variable_name_fly, JS::js_undefined(), false));
    (void)TRY(environment->delete_binding(vm, variable_name_fly));
    vm.heap().uproot_cell(&value.as_cell());
    return {};
}

bool Internals::has_completed_initial_paint() const
{
    return window().associated_document().font_computer().has_completed_initial_paint();
}

bool Internals::initial_paint_had_pending_fonts() const
{
    return window().associated_document().font_computer().initial_paint_had_pending_fonts();
}

void Internals::set_font_display_time(CSS::FontFace& font_face, u32 milliseconds)
{
    font_face.set_font_display_time_for_testing(milliseconds);
}

WebIDL::ExceptionOr<Utf16String> Internals::set_time_zone(Utf16String const& time_zone)
{
    auto current_time_zone = Core::TimeZone::current_time_zone();

    if (auto result = Core::TimeZone::set_current_time_zone(time_zone); result.is_error())
        return window().principal_realm().vm().throw_completion<JS::InternalError>(Utf16String::formatted("Could not set time zone: {}", result.error()));

    JS::clear_system_time_zone_cache();
    return current_time_zone;
}

bool Internals::wrapper_is_preserved(JS::Object& object)
{
    auto* platform_object = Bindings::as_platform_object(object);
    if (!platform_object)
        return false;
    return Bindings::wrapper_is_preserved(*platform_object);
}

bool Internals::has_activity_root(JS::Object& object)
{
    if (auto* xhr = Bindings::impl_from<XHR::XMLHttpRequest>(&object))
        return xhr->has_activity_root();
    if (auto* resize_observer = Bindings::impl_from<ResizeObserver::ResizeObserver>(&object))
        return resize_observer->has_activity_root();
    if (auto* web_socket = Bindings::impl_from<WebSockets::WebSocket>(&object))
        return web_socket->has_activity_root();
    return false;
}

WebIDL::UnsignedLongLong Internals::message_port_pending_outgoing_message_count(HTML::MessagePort& port)
{
    return port.pending_outgoing_message_count();
}

WebIDL::UnsignedLongLong Internals::html_collection_cache_generation(DOM::HTMLCollection& collection)
{
    return collection.cache_generation_for_testing();
}

void Internals::fail_next_message_port_transfer(HTML::MessagePort& port)
{
    port.fail_next_transfer_for_testing();
}

bool Internals::message_ports_are_directly_entangled(HTML::MessagePort& first, HTML::MessagePort& second)
{
    return first.entangled_port().ptr() == &second && second.entangled_port().ptr() == &first;
}

GC::Ref<XHR::XMLHttpRequest> Internals::create_xml_http_request_for_document(DOM::Document& document)
{
    VERIFY(document.window());
    return XHR::XMLHttpRequest::create(*document.window());
}

Optional<Painting::HitTestResult> Internals::hit_test(double x, double y)
{
    auto& active_document = window().associated_document();
    // NOTE: Force a layout update just before hit testing. This is because the current layout tree, which is required
    //       for stacking context traversal, might not exist if this call occurs between the tear_down_layout_tree()
    //       and update_layout() calls
    Layout::ForcedReadScope read { active_document };
    active_document.update_layout(DOM::UpdateLayoutReason::InternalsHitTest);
    return active_document.hit_test(read, { x, y });
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
    case 0xE00C: // Escape
        return WebDriverKeyData { UIEvents::Key_Escape, 0, 0 };
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

WebIDL::ExceptionOr<void> Internals::send_text_editing_command(HTML::HTMLElement& target, Utf16String const& command_name, bool extend_selection)
{
    using Type = Web::TextEditingCommand::Type;
    struct NamedCommand {
        Utf16View name;
        Type type;
    };
    static constexpr NamedCommand commands[] = {
        { u"MoveBackward"sv, Type::MoveBackward },
        { u"MoveForward"sv, Type::MoveForward },
        { u"MoveWordBackward"sv, Type::MoveWordBackward },
        { u"MoveWordForward"sv, Type::MoveWordForward },
        { u"MoveUp"sv, Type::MoveUp },
        { u"MoveDown"sv, Type::MoveDown },
        { u"MoveToBeginningOfParagraph"sv, Type::MoveToBeginningOfParagraph },
        { u"MoveToEndOfParagraph"sv, Type::MoveToEndOfParagraph },
        { u"MoveToBeginningOfLine"sv, Type::MoveToBeginningOfLine },
        { u"MoveToEndOfLine"sv, Type::MoveToEndOfLine },
        { u"MoveToLeftEndOfLine"sv, Type::MoveToLeftEndOfLine },
        { u"MoveToRightEndOfLine"sv, Type::MoveToRightEndOfLine },
        { u"PageUp"sv, Type::PageUp },
        { u"PageDown"sv, Type::PageDown },
        { u"DeleteBackward"sv, Type::DeleteBackward },
        { u"DeleteForward"sv, Type::DeleteForward },
        { u"DeleteWordBackward"sv, Type::DeleteWordBackward },
        { u"DeleteWordForward"sv, Type::DeleteWordForward },
        { u"DeleteToEndOfParagraph"sv, Type::DeleteToEndOfParagraph },
    };
    for (auto const& command : commands) {
        if (command_name.utf16_view() != command.name)
            continue;
        target.focus();
        Array<TextEditingCommand, 1> editing_commands { TextEditingCommand { command.type, extend_selection } };
        page().handle_keydown(UIEvents::KeyCode::Key_A, UIEvents::Mod_Ctrl, 'a', false, false, false, editing_commands, true);
        return {};
    }
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Unknown text editing command"_utf16 };
}

void Internals::send_text(HTML::HTMLElement& target, Utf16String const& text, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    target.focus();

    for (auto code_point : text) {
        if (auto data = webdriver_key_to_key_code(code_point); data.has_value()) {
            page.handle_keydown(data->key_code, modifiers | data->additional_modifiers, data->code_point_to_send, false, data->code_point_to_send != 0);
            page.handle_keyup(data->key_code, modifiers | data->additional_modifiers, data->code_point_to_send, false);
        } else {
            page.handle_keydown(UIEvents::code_point_to_key_code(code_point), modifiers, code_point, false, true);
            page.handle_keyup(UIEvents::code_point_to_key_code(code_point), modifiers, code_point, false);
        }
    }
}

// The compositor context that the UI routes this page's input through.
static Compositor::CompositorContextHandle* compositor_context_routing_input_for(HTML::Window& window)
{
    auto local_root = window.navigable()->local_root();
    if (!local_root->has_compositor_context())
        return nullptr;
    return &local_root->compositor_context();
}

// Frames reach the compositor from WebContent, and input from the UI. Input that a test sends through the UI once the
// compositor has every frame rendered so far meets the content the test has rendered, as the user's input would.
static void let_compositor_take_in_every_rendered_frame(HTML::Window& window)
{
    auto& page = window.page();
    for (auto& navigable : HTML::all_local_navigables()) {
        if (&navigable->page() == &page && !navigable->has_been_destroyed())
            (void)navigable->take_recording_in_flight_in(HTML::LocalNavigable::TakeIn::Wait);
    }
    if (auto* compositor_context = compositor_context_routing_input_for(window))
        compositor_context->synchronize_for_testing();
}

void Internals::send_text_through_ui_process(Utf16String const& text)
{
    let_compositor_take_in_every_rendered_frame(window());
    for (auto code_point : text) {
        for (auto type : { Web::KeyEvent::Type::KeyDown, Web::KeyEvent::Type::KeyUp }) {
            Web::KeyEvent event;
            event.type = type;
            event.key = UIEvents::code_point_to_key_code(code_point);
            event.code_point = code_point;
            event.should_insert_text = type == Web::KeyEvent::Type::KeyDown;
            page().client().page_did_request_key_event_for_testing(move(event));
        }
    }
}

void Internals::grant_transient_activation()
{
    window().notify_about_user_activation();
}

namespace {

struct MouseEventsTheUIProcessHasNotHandledYet : public RefCounted<MouseEventsTheUIProcessHasNotHandledYet> {
    size_t count { 0 };
};

}

// A mouse event at a position in this document's viewport, as the UI process sends it to the page hosting the document.
Web::MouseEvent Internals::mouse_event_through_ui_process_at(Web::MouseEvent::Type type, double x, double y)
{
    auto position = page().css_to_device_point(window().navigable()->to_page_position({ x, y }));
    Web::MouseEvent event;
    event.type = type;
    event.position = position;
    event.screen_position = position;
    return event;
}

// Sends mouse events through the UI process as it sends the user's, and resolves the promise once the UI has handled
// every one of them.
void Internals::send_mouse_events_through_ui_process(Vector<Web::MouseEvent> events, GC::Ref<WebIDL::Promise> promise)
{
    let_compositor_take_in_every_rendered_frame(window());

    auto& page = this->page();
    auto local_root_id = window().navigable()->local_root()->id();
    auto& realm = window().principal_realm();
    auto unhandled_events = make_ref_counted<MouseEventsTheUIProcessHasNotHandledYet>();
    unhandled_events->count = events.size();
    for (auto& event : events) {
        page.client().page_did_request_webdriver_mouse_event(local_root_id, move(event), GC::create_function(heap(), [&realm, promise, unhandled_events] {
            if (--unhandled_events->count != 0)
                return;
            HTML::TemporaryExecutionContext execution_context { realm };
            WebIDL::resolve_promise(realm, promise);
        }));
    }
}

void Internals::click_through_ui_process(double x, double y, GC::Ref<WebIDL::Promise> promise)
{
    Vector<Web::MouseEvent> events;
    for (auto type : { Web::MouseEvent::Type::MouseDown, Web::MouseEvent::Type::MouseUp }) {
        auto event = mouse_event_through_ui_process_at(type, x, y);
        event.button = UIEvents::MouseButton::Primary;
        event.buttons = type == Web::MouseEvent::Type::MouseDown ? UIEvents::MouseButton::Primary : UIEvents::MouseButton::None;
        event.click_count = 1;
        events.append(move(event));
    }
    send_mouse_events_through_ui_process(move(events), promise);
}

static Web::MouseEvent::Type mouse_event_type_from(Bindings::MouseEventThroughUIProcessType type)
{
    switch (type) {
    case Bindings::MouseEventThroughUIProcessType::Mousedown:
        return Web::MouseEvent::Type::MouseDown;
    case Bindings::MouseEventThroughUIProcessType::Mouseup:
        return Web::MouseEvent::Type::MouseUp;
    case Bindings::MouseEventThroughUIProcessType::Mousemove:
        return Web::MouseEvent::Type::MouseMove;
    case Bindings::MouseEventThroughUIProcessType::Mouseleave:
        return Web::MouseEvent::Type::MouseLeave;
    }
    VERIFY_NOT_REACHED();
}

void Internals::mouse_event_through_ui_process(Bindings::MouseEventThroughUIProcessType type, double x, double y, Bindings::MouseEventThroughUIProcessOptions const& options, GC::Ref<WebIDL::Promise> promise)
{
    auto event = mouse_event_through_ui_process_at(mouse_event_type_from(type), x, y);
    event.button = button_from_unsigned_short(options.button);
    if (options.buttons.has_value())
        event.buttons = static_cast<UIEvents::MouseButton>(*options.buttons);
    else if (event.type == Web::MouseEvent::Type::MouseDown)
        event.buttons = event.button;
    event.modifiers = static_cast<UIEvents::KeyModifier>(options.modifiers);
    event.click_count = options.click_count;
    if (options.pointer.has_value() && (options.pointer->pointer_type == u"pen"sv || options.pointer->pointer_type == u"touch"sv)) {
        auto const& init = *options.pointer;
        PointerProperties properties;
        properties.pointer_id = init.pointer_id;
        properties.pointer_type = init.pointer_type == u"touch"sv ? PointerType::Touch : PointerType::Pen;
        properties.width = init.width;
        properties.height = init.height;
        properties.pressure = init.pressure;
        properties.tangential_pressure = init.tangential_pressure;
        properties.tilt_x = init.tilt_x.value_or(0);
        properties.tilt_y = init.tilt_y.value_or(0);
        properties.twist = init.twist;
        properties.altitude_angle = init.altitude_angle.value_or(AK::Pi<double> / 2);
        properties.azimuth_angle = init.azimuth_angle.value_or(0);
        event.pointer_data = PointerInputData { properties };
    }
    Vector<Web::MouseEvent> events;
    events.append(move(event));
    send_mouse_events_through_ui_process(move(events), promise);
}

void Internals::wheel_through_ui_process(double x, double y, double delta_x, double delta_y, bool precise, Bindings::ScrollGesturePhase phase, Bindings::WheelTicks const& ticks, GC::Ref<WebIDL::Promise> promise)
{
    auto event = mouse_event_through_ui_process_at(Web::MouseEvent::Type::MouseWheel, x, y);
    event.wheel_delta_x = delta_x;
    event.wheel_delta_y = delta_y;
    event.wheel_delta_precision = wheel_delta_precision_from(precise);
    event.wheel_tick_120ths_x = wheel_tick_120ths_from(ticks.x, delta_x, event.wheel_delta_precision);
    event.wheel_tick_120ths_y = wheel_tick_120ths_from(ticks.y, delta_y, event.wheel_delta_precision);
    event.scroll_gesture_phase = scroll_gesture_phase_from(phase);
    Vector<Web::MouseEvent> events;
    events.append(move(event));
    send_mouse_events_through_ui_process(move(events), promise);
}

void Internals::send_key(HTML::HTMLElement& target, Utf16String const& key_name, WebIDL::UnsignedShort modifiers, WebIDL::UnsignedLong repeat_count)
{
    if (repeat_count == 0)
        return;

    auto key_code = UIEvents::key_code_from_string(key_name.utf16_view());
    target.focus();

    for (u32 press = 0; press < repeat_count; ++press)
        page().handle_keydown(key_code, modifiers, 0, press > 0, false);
    page().handle_keyup(key_code, modifiers, 0, false);
}

void Internals::paste(HTML::HTMLElement& target, Utf16String const& text)
{
    auto& page = this->page();
    target.focus();

    if (auto navigable = page.hosted_focused_navigable())
        navigable->paste(text);
}

void Internals::paste_from_clipboard()
{
    if (auto navigable = page().hosted_focused_navigable())
        navigable->paste_from_clipboard();
}

void Internals::commit_text()
{
    page().handle_keydown(UIEvents::Key_Return, 0, 0x0d, false, true);
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

void Internals::mouse_down(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers, Optional<WebIDL::UnsignedShort> buttons)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    auto pressed_button = button_from_unsigned_short(button);
    page.handle_mousedown(position, position, pressed_button, buttons.value_or(pressed_button), modifiers, click_count);
}

void Internals::mouse_up(double x, double y, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers, WebIDL::UnsignedShort buttons)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_mouseup(position, position, button_from_unsigned_short(button), buttons, modifiers);
}

void Internals::mouse_move(double x, double y, WebIDL::UnsignedShort modifiers, WebIDL::UnsignedShort buttons)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_mousemove(position, position, buttons, modifiers);
}

void Internals::mouse_leave()
{
    this->page().handle_mouseleave();
}

void Internals::mouse_cancel()
{
    this->page().handle_mousecancel();
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
    page.handle_mousedown(position, position, mouse_button, mouse_button, modifiers, click_count);
}

void Internals::wheel(GC::Ref<WebIDL::Promise> promise, double x, double y, double delta_x, double delta_y, bool precise, Bindings::ScrollGesturePhase phase, Bindings::WheelTicks const& ticks)
{
    auto& page = this->page();

    auto wheel_delta_precision = wheel_delta_precision_from(precise);
    auto scroll_gesture_phase = scroll_gesture_phase_from(phase);
    auto position = page.css_to_device_point({ x, y });
    Optional<AsyncScrollOperation> async_scroll_operation;
    auto wheel_tick_120ths_x = wheel_tick_120ths_from(ticks.x, delta_x, wheel_delta_precision);
    auto wheel_tick_120ths_y = wheel_tick_120ths_from(ticks.y, delta_y, wheel_delta_precision);
    page.handle_mousewheel(position, position, 0, 0, 0, delta_x, delta_y, wheel_tick_120ths_x, wheel_tick_120ths_y, wheel_delta_precision, scroll_gesture_phase, false, &async_scroll_operation);

    if (async_scroll_operation.has_value() && async_scroll_operation->navigable) {
        async_scroll_operation->navigable->wait_for_async_scroll_operation(async_scroll_operation->operation_id, promise);
        return;
    }

    GC::RootVector<GC::Ref<WebIDL::Promise>> scroll_promises;
    for (auto& navigable : HTML::all_local_navigables()) {
        if (&navigable->page() == &page && !navigable->has_been_destroyed())
            navigable->wait_for_wheel_scrolls(scroll_promises);
    }
    auto& realm = window().principal_realm();
    auto resolve = [&realm, promise] {
        HTML::TemporaryExecutionContext execution_context { realm };
        WebIDL::resolve_promise(realm, promise);
    };
    WebIDL::wait_for_all(realm, scroll_promises, [resolve](auto const&) { resolve(); }, [resolve](auto) { resolve(); });
}

void Internals::wheel(double x, double y, double delta_x, double delta_y, bool precise, Bindings::ScrollGesturePhase phase, Bindings::WheelTicks const& ticks, GC::Ref<WebIDL::Promise> promise)
{
    wheel(promise, x, y, delta_x, delta_y, precise, phase, ticks);
}

static JS::Value wrapped_node_with_unique_id(HTML::Window& window, Optional<UniqueNodeID> node_id)
{
    auto* node = node_id.has_value() ? DOM::Node::from_unique_id(*node_id) : nullptr;
    if (!node)
        return JS::js_null();
    auto& realm = HTML::relevant_realm(window);
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref<DOM::Node> { *node });
}

// A JSON report of a testing message as a script reads it, made of objects without a prototype like the other objects
// these hooks return.
static JS::Value json_value_to_js_value(HTML::Window& window, JsonValue const& value)
{
    auto& realm = HTML::relevant_realm(window);
    if (value.is_null())
        return JS::js_null();
    if (value.is_bool())
        return JS::Value(value.as_bool());
    if (value.is_number())
        return JS::Value(value.get_double_with_precision_loss().value());
    if (value.is_string())
        return JS::PrimitiveString::create(window.vm(), Utf16String::from_utf8(value.as_string()));
    if (value.is_array()) {
        auto const& values = value.as_array().values();
        auto array = MUST(JS::Array::create(realm, values.size()));
        for (size_t i = 0; i < values.size(); ++i)
            MUST(array->create_data_property_or_throw(i, json_value_to_js_value(window, values[i])));
        return array;
    }
    auto object = JS::Object::create(realm, nullptr);
    value.as_object().for_each_member([&](String const& name, JsonValue const& member) {
        object->define_direct_property(Utf16FlyString::from_utf8(name), json_value_to_js_value(window, member), JS::default_attributes);
    });
    return object;
}

// A scrolling state as a script reads it, with the document it is the state of.
static GC::Ref<JS::Object> scrolling_state_report_to_object(HTML::Window& window, JsonObject const& report)
{
    auto object = GC::Ref { json_value_to_js_value(window, report).as_object() };
    auto document_id = report.get_i64("documentID"sv).map([](i64 id) { return UniqueNodeID { id }; });
    object->define_direct_property("document"_utf16_fly_string, wrapped_node_with_unique_id(window, document_id), JS::default_attributes);
    return object;
}

GC::Ref<JS::Object> Internals::compositor_scroll_state(GC::Ptr<DOM::Element> scroller)
{
    let_compositor_take_in_every_rendered_frame(window());

    auto& document = window().associated_document();
    Web::AsyncScrollNodeStableID stretched_scroller { .node_id = document.unique_id(), .kind = Web::AsyncScrollNodeKind::Viewport };
    if (scroller) {
        Layout::ForcedReadScope read { document };
        document.update_layout(DOM::UpdateLayoutReason::InternalsHitTest);
        if (auto const* scrolling_box = scroller->layout_node(read)) {
            if (auto scroller_stable_node_id = Painting::async_scroll_node_stable_id(*scrolling_box); scroller_stable_node_id.has_value())
                stretched_scroller = *scroller_stable_node_id;
        }
    }

    String scroll_state;
    if (auto navigable = document.navigable(); navigable && navigable->has_compositor_context())
        scroll_state = navigable->compositor_context().scroll_state_for_testing(stretched_scroller);
    auto report = JsonValue::from_string(scroll_state);
    if (report.is_error() || !report.value().is_object())
        return scrolling_state_report_to_object(window(), Compositing::async_scrolling_state_to_json({}));
    return scrolling_state_report_to_object(window(), report.value().as_object());
}

void Internals::pinch(double x, double y, double scale_delta, WebIDL::UnsignedShort modifiers)
{
    auto& page = this->page();
    auto position = page.css_to_device_point({ x, y });
    page.handle_pinch_event(position, modifiers, scale_delta);
}

void Internals::reset_zoom()
{
    if (auto local_root = local_root_of(window()))
        local_root->reset_zoom();
}

Utf16String Internals::current_cursor()
{
    auto& page = this->page();

    return page.current_cursor().visit(
        [](Gfx::StandardCursor cursor) {
            return Utf16String::from_utf8_without_validation(Gfx::standard_cursor_to_string(cursor));
        },
        [](Gfx::ImageCursor const&) {
            return "Image"_utf16;
        });
}

Optional<Utf16String> Internals::current_cursor_pixel(i32 x, i32 y)
{
    auto cursor = page().current_cursor();
    auto const* image_cursor = cursor.get_pointer<Gfx::ImageCursor>();
    if (!image_cursor || !image_cursor->bitmap.is_valid())
        return {};
    return Utf16String::from_utf8(image_cursor->bitmap.bitmap()->get_pixel(x, y).to_string());
}

Utf16String Internals::theme_color()
{
    auto& document = window().associated_document();
    Layout::ForcedReadScope read { document };
    return Utf16String::from_utf8(document.theme_color(read).to_string());
}

Utf16String Internals::selected_text_for_clipboard()
{
    if (auto navigable = page().hosted_focused_navigable())
        return navigable->selected_text();
    return {};
}

void Internals::find_in_page(Utf16String const& query, bool highlight_all)
{
    page().find_in_page({ .string = query, .highlight_all_matches = highlight_all });
}

void Internals::find_in_page_next_match()
{
    page().find_in_page_next_match();
}

void Internals::find_in_page_end()
{
    page().find_in_page_end();
}

WebIDL::ExceptionOr<void> Internals::set_clipboard_file(Utf16String const& name, Utf16String const& mime_type, Utf16String const& data)
{
    auto mime_type_utf8 = mime_type.to_utf8_but_should_be_ported_to_utf16();
    auto data_utf8 = data.to_utf8_but_should_be_ported_to_utf16();

    HTML::SelectedFile file { name, MUST(ByteBuffer::copy(data_utf8.bytes())) };
    Clipboard::SystemClipboardRepresentation item { move(mime_type_utf8), move(file) };

    page().client().page_did_insert_clipboard_item({ { move(item) } }, "unspecified"sv);
    return {};
}

Optional<Utf16String> Internals::generate_text_fragment_url(DOM::Range& range, Utf16String const& current_url)
{
    auto url = DOMURL::parse(current_url.utf16_view());
    if (!url.has_value())
        return {};
    auto generated_url = HTML::generate_text_fragment_url(window().associated_document(), range, *url);
    if (!generated_url.has_value())
        return {};
    return Utf16String::from_utf8(generated_url->serialize());
}

void Internals::set_marked_text_from_input_method(Utf16String const& text)
{
    if (auto navigable = page().hosted_focused_navigable())
        navigable->set_marked_text_from_input_method(text);
}

void Internals::commit_text_from_input_method(Utf16String const& text, WebIDL::Long replacement_start, WebIDL::Long replacement_length)
{
    if (auto navigable = page().hosted_focused_navigable())
        navigable->commit_text_from_input_method(text, replacement_start, replacement_length);
}

void Internals::unmark_text_from_input_method()
{
    if (auto navigable = page().hosted_focused_navigable())
        navigable->unmark_text_from_input_method();
}

GC::Ptr<Geometry::DOMRect> Internals::current_caret_rect()
{
    auto& active_document = window().associated_document();
    auto rect = active_document.current_caret_rect();
    if (!rect.has_value())
        return nullptr;
    return Geometry::DOMRect::create(static_cast<double>(rect->x()), static_cast<double>(rect->y()), static_cast<double>(rect->width()), static_cast<double>(rect->height()));
}

WebIDL::ExceptionOr<bool> Internals::dispatch_user_activated_event(DOM::EventTarget& target, DOM::Event& event)
{
    event.set_is_trusted(true);
    return target.dispatch_event(event);
}

void Internals::set_navigator_compatibility_mode(Bindings::InternalsNavigatorCompatibilityMode mode)
{
    switch (mode) {
    case Bindings::InternalsNavigatorCompatibilityMode::Chrome:
        ResourceLoader::the().set_navigator_compatibility_mode(NavigatorCompatibilityMode::Chrome);
        break;
    case Bindings::InternalsNavigatorCompatibilityMode::Gecko:
        ResourceLoader::the().set_navigator_compatibility_mode(NavigatorCompatibilityMode::Gecko);
        break;
    case Bindings::InternalsNavigatorCompatibilityMode::Webkit:
        ResourceLoader::the().set_navigator_compatibility_mode(NavigatorCompatibilityMode::WebKit);
        break;
    }
}

void Internals::spoof_current_url(Utf16String const& url_string)
{
    auto url = DOMURL::parse(url_string.utf16_view());

    VERIFY(url.has_value());

    auto origin = url->origin();

    auto& window = this->window();
    window.associated_document().set_url(url.value());
    window.associated_document().set_origin(origin);
    HTML::relevant_settings_object(window.associated_document()).creation_url = url.release_value();
    page().client().page_did_spoof_document_origin_for_testing(window.associated_document().relevant_settings_object(), origin);
}

void Internals::spoof_current_origin_in_this_process(Utf16String const& url_string)
{
    auto url = DOMURL::parse(url_string.utf16_view());
    VERIFY(url.has_value());
    window().associated_document().set_origin(url->origin());
}

void Internals::load_url(Utf16String const& url_string)
{
    auto url = DOMURL::parse(url_string.utf16_view());

    VERIFY(url.has_value());

    auto local_root = local_root_of(window());
    if (!local_root)
        return;

    Core::deferred_invoke([local_root = GC::make_root(*local_root), url = url.release_value()] {
        (void)local_root->navigate({ .url = url,
            .history_handling = Web::Bindings::NavigationHistoryBehavior::Auto,
            .user_involvement = HTML::UserNavigationInvolvement::BrowserUI });
    });
}

GC::Ref<InternalAnimationTimeline> Internals::create_internal_animation_timeline()
{
    return InternalAnimationTimeline::create(window().associated_document());
}

WebIDL::ExceptionOr<void> Internals::simulate_drag_start(double x, double y, Utf16String const& name, Utf16String const& contents)
{
    auto contents_utf8 = TRY_OR_THROW_OOM(vm(), contents.utf16_view().to_utf8());

    Vector<HTML::SelectedFile> files;
    files.empend(name, TRY_OR_THROW_OOM(vm(), ByteBuffer::copy(contents_utf8.bytes())));

    auto& page = this->page();

    auto position = page.css_to_device_point({ x, y });
    page.handle_drag_and_drop_event(DragEvent::Type::DragStart, position, position, UIEvents::MouseButton::Primary, 0, 0, move(files));
    return {};
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

void Internals::clear_storage_for_testing(Utf16String const& type)
{
    auto storage_endpoint = type == "session"_utf16 ? StorageAPI::StorageEndpointType::SessionStorage : StorageAPI::StorageEndpointType::LocalStorage;
    page().client().page_did_request_clear_storage_for_testing(storage_endpoint, window().associated_document().relevant_settings_object());
}

void Internals::expire_cookies_with_time_offset(WebIDL::LongLong seconds)
{
    page().client().page_did_expire_cookies_with_time_offset(AK::Duration::from_seconds(seconds));
}

GC::Ref<WebIDL::Promise> Internals::delete_all_cookies()
{
    auto promise = WebIDL::create_promise_for(window());
    auto const& document = window().associated_document();

    page().client().page_did_delete_all_cookies(document.url(), promise);
    return promise;
}

WebIDL::ExceptionOr<bool> Internals::has_cookie_for_url(Utf16String const& url, String const& name, String const& value)
{
    auto parsed_url = URL::Parser::basic_parse(url.utf16_view());
    if (!parsed_url.has_value())
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = Utf16String::formatted("Invalid URL: '{}'", url) };

    // NB: Look at both the first-party cookies of the URL, as its top-level documents see them, and the cookies of the
    //     URL under this document's top-level site, as its cross-site frames see them.
    auto has_cookie = [&](Optional<HTTP::Cookie::PartitionContext> const& partition_context) {
        auto cookies = page().client().page_did_request_all_cookies_webdriver(parsed_url.value(), partition_context);
        return any_of(cookies, [&](auto const& cookie) {
            return cookie.name == name && cookie.value == value;
        });
    };
    if (has_cookie({}))
        return true;

    auto partition_key = Fetch::Infrastructure::determine_the_network_partition_key(window().associated_document().relevant_settings_object());
    return partition_key.has_value() && has_cookie(HTTP::Cookie::PartitionContext { partition_key->top_level_site, partition_key->has_cross_site_ancestor });
}

bool Internals::set_http_memory_cache_enabled(bool enabled)
{
    auto was_enabled = Web::Fetch::Fetching::http_memory_cache_enabled();
    Web::Fetch::Fetching::set_http_memory_cache_enabled(enabled);
    return was_enabled;
}

bool Internals::set_disk_cache_enabled_for_navigations(bool enabled)
{
    auto was_enabled = Web::Fetch::Fetching::disk_cache_enabled_for_navigations_for_testing();
    Web::Fetch::Fetching::set_disk_cache_enabled_for_navigations_for_testing(enabled);
    return was_enabled;
}

void Internals::simulate_request_server_connection_loss()
{
    Vector<Optional<Utf16String>> top_level_sites { OptionalNone {} };
    for (auto& top_level_site : ResourceLoader::the().top_level_sites())
        top_level_sites.append(move(top_level_site));

    for (auto const& top_level_site : top_level_sites) {
        auto disconnected_client = ResourceLoader::the().take_client_for_testing(top_level_site);
        if (!disconnected_client)
            continue;
        auto request_server_died_callback = move(disconnected_client->on_request_server_died);
        disconnected_client = nullptr;

        // Replace the connection synchronously so resource loading can continue.
        page().client().page_did_lose_request_server_connection(top_level_site);

        // Exercise reconnection before the deferred connection-loss callback runs.
        if (request_server_died_callback)
            request_server_died_callback();
    }
}

void Internals::simulate_worker_request_server_connection_loss()
{
    page().client().page_did_simulate_worker_request_server_connection_loss();
}

WebIDL::ExceptionOr<void> Internals::send_bad_ipc_message_for_testing(Utf16String const& kind)
{
    auto kind_utf8 = TRY_OR_THROW_OOM(window().principal_realm().vm(), kind.utf16_view().to_utf8());
    if (kind_utf8 != "cookie-request-unknown-page-id"sv)
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = Utf16String::formatted("Unknown bad IPC message kind: '{}'", kind) };

    page().client().send_bad_ipc_message_for_testing(kind_utf8, window().associated_document().url());
    return {};
}

WebIDL::ExceptionOr<void> Internals::set_content_blockers(Utf16String const& patterns_source)
{
    Utf16StringBuilder patterns_builder;

    patterns_source.for_each_split_view('\n', SplitBehavior::Nothing, [&](Utf16View line) {
        if (line.ends_with('\r'))
            line = line.substring_view(0, line.length_in_code_units() - 1);
        if (line.is_empty())
            return IterationDecision::Continue;

        patterns_builder.append(line);
        patterns_builder.append("\n"_utf16);
        return IterationDecision::Continue;
    });
    auto patterns = patterns_builder.to_string();
    auto patterns_utf8 = TRY_OR_THROW_OOM(window().principal_realm().vm(), patterns.utf16_view().to_utf8());

    auto& blocker = ContentBlocker::the();
    auto had_cosmetic_rules = blocker.has_cosmetic_rules();
    auto result = blocker.set_rules_from_bytes(patterns_utf8.bytes());
    if (result.is_error())
        return window().principal_realm().vm().throw_completion<JS::InternalError>(Utf16String::formatted("Could not set content blockers: {}", result.error()));

    if (had_cosmetic_rules || blocker.has_cosmetic_rules())
        page().invalidate_user_style();

    return {};
}

WebIDL::ExceptionOr<void> Internals::set_site_compatibility_data(Utf16String const& source)
{
    auto source_utf8 = TRY_OR_THROW_OOM(window().principal_realm().vm(), source.utf16_view().to_utf8());
    auto json = JsonValue::from_string(source_utf8);
    if (json.is_error())
        return window().principal_realm().vm().throw_completion<JS::InternalError>("Could not parse site compatibility data"_utf16);

    auto data = SiteCompatibilityData::from_json(json.value());
    if (data.is_error())
        return window().principal_realm().vm().throw_completion<JS::InternalError>(Utf16String::formatted("Could not set site compatibility data: {}", data.error()));

    ResourceLoader::the().set_site_compatibility_data(data.release_value());
    return {};
}

void Internals::set_content_blocking_enabled(bool enabled)
{
    page().set_content_blocking_policy(enabled, {});
}

WebIDL::UnsignedLongLong Internals::partial_layout_count()
{
    return window().associated_document().partial_layout_count();
}

WebIDL::UnsignedLongLong Internals::full_layout_count()
{
    return window().associated_document().full_layout_count();
}

void Internals::begin_layout_trace()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    Layout::RustFFI::render_state_begin_layout_trace(window().associated_document().layout_node_arena().host(),
        [](Layout::RustFFI::DocumentHost const* host, Layout::BegunRead const* read, Compositing::RustFFI::NodeSlotId slot, void* sink, void (*append)(void*, u8 const*, size_t)) {
            auto description = static_cast<Layout::Node const*>(Layout::RustFFI::render_state_node_shell_if_live(host, read, slot))->debug_description();
            append(sink, description.bytes().data(), description.bytes().size());
        });
}

void Internals::update_layout_for_testing()
{
    window().associated_document().update_layout(DOM::UpdateLayoutReason::InternalsLayoutTest);
}

Utf16String Internals::take_layout_trace()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    StringBuilder builder;
    Layout::RustFFI::render_state_take_layout_trace(window().associated_document().layout_node_arena().host(), read, &builder,
        [](void* context, u8 const* bytes, size_t length) {
            static_cast<StringBuilder*>(context)->append(StringView { bytes, length });
        });
    return Utf16String::from_utf8_without_validation(builder.string_view());
}

WebIDL::UnsignedLongLong Internals::accumulated_visual_context_tree_build_count()
{
    auto& document = window().associated_document();
    if (!document.has_committed_viewport_box())
        return 0;
    return document.paint_state().accumulated_visual_context_tree_build_count();
}

void Internals::begin_display_list_trace()
{
    auto& document = window().associated_document();
    (void)document.paint_state().take_recording_traces();
    Layout::RustFFI::render_state_set_recording_trace_enabled(document.layout_node_arena().host(), true);
}

Utf16String Internals::take_display_list_trace()
{
    auto& document = window().associated_document();
    Layout::RustFFI::render_state_set_recording_trace_enabled(document.layout_node_arena().host(), false);
    StringBuilder builder;
    for (auto const& trace : document.paint_state().take_recording_traces())
        builder.append(trace);
    return Utf16String::from_utf8_without_validation(builder.string_view());
}

void Internals::begin_input_trace()
{
    page().input_trace().begin();
    if (auto* compositor_context = compositor_context_routing_input_for(window()))
        compositor_context->begin_input_trace_for_testing();
}

static Vector<InputTraceRecord> take_input_trace_records(HTML::Window& window)
{
    Vector<InputTraceRecord> records;
    if (auto* compositor_context = compositor_context_routing_input_for(window))
        records.extend(compositor_context->take_input_trace_for_testing());
    records.extend(window.page().input_trace().take());

    // The compositor sees each input event from the UI before this process does. Events in UI order, and within
    // an event the compositor's records first, read as the way the event went. Input that did not come from the UI
    // goes last.
    auto ui_order = [](InputTraceRecord const& record) {
        return record.input_event_id == 0 ? NumericLimits<u64>::max() : record.input_event_id;
    };
    insertion_sort(records, [&](auto const& a, auto const& b) { return ui_order(a) < ui_order(b); });
    return records;
}

static Gfx::FloatPoint device_position_as_float_point(DevicePixelPoint position)
{
    return { static_cast<float>(position.x().value()), static_cast<float>(position.y().value()) };
}

// A scroller as the input hooks describe it: the document for its viewport, or the element whose box scrolls, with the
// pseudo-element that generated the box, if any.
static void define_scroller_properties(HTML::Window& window, JS::Object& object, Optional<AsyncScrollNodeStableID> scroller)
{
    auto& vm = window.vm();
    object.define_direct_property("scroller"_utf16_fly_string, wrapped_node_with_unique_id(window, scroller.map([](auto const& id) { return id.node_id; })), JS::default_attributes);
    if (scroller.has_value() && scroller->kind == AsyncScrollNodeKind::PseudoElement) {
        auto pseudo_element = CSS::pseudo_element_name(static_cast<CSS::PseudoElement>(scroller->pseudo_element_type));
        object.define_direct_property("scrollerPseudoElement"_utf16_fly_string, JS::PrimitiveString::create(vm, pseudo_element), JS::default_attributes);
    }
}

GC::Ref<JS::Object> Internals::take_input_trace()
{
    auto records = take_input_trace_records(window());
    auto& realm = HTML::relevant_realm(window());
    auto array = MUST(JS::Array::create(realm, records.size()));
    for (size_t i = 0; i < records.size(); ++i) {
        auto const& record = records[i];
        auto object = JS::Object::create(realm, nullptr);
        object->define_direct_property("id"_utf16_fly_string, JS::Value(static_cast<double>(record.input_event_id)), JS::default_attributes);
        object->define_direct_property("stage"_utf16_fly_string, JS::PrimitiveString::create(vm(), input_trace_stage_name(record.stage)), JS::default_attributes);
        object->define_direct_property("decision"_utf16_fly_string, JS::PrimitiveString::create(vm(), input_routing_decision_name(record.decision)), JS::default_attributes);
        define_scroller_properties(window(), *object, record.scroller);
        object->define_direct_property("target"_utf16_fly_string, wrapped_node_with_unique_id(window(), record.dom_target), JS::default_attributes);
        MUST(array->create_data_property_or_throw(i, object));
    }
    return array;
}

GC::Ref<JS::Object> Internals::first_wheel_step_route_to_object(Compositing::FirstWheelStepRouteForTesting const& result)
{
    auto& realm = HTML::relevant_realm(window());
    auto object = JS::Object::create(realm, nullptr);
    object->define_direct_property("decision"_utf16_fly_string, JS::PrimitiveString::create(vm(), input_routing_decision_name(result.route.decision)), JS::default_attributes);
    define_scroller_properties(window(), *object, result.route.scroller);
    object->define_direct_property("document"_utf16_fly_string, wrapped_node_with_unique_id(window(), result.document_id), JS::default_attributes);
    return object;
}

GC::Ref<JS::Object> Internals::compositor_wheel_target_at(double x, double y, double delta_x, double delta_y)
{
    let_compositor_take_in_every_rendered_frame(window());
    Optional<Compositing::FirstWheelStepRouteForTesting> result;
    if (auto* compositor_context = compositor_context_routing_input_for(window())) {
        auto position = page().css_to_device_point(window().navigable()->to_page_position({ x, y }));
        result = compositor_context->route_first_wheel_step_for_testing(device_position_as_float_point(position), { static_cast<float>(delta_x), static_cast<float>(delta_y) });
    }
    return first_wheel_step_route_to_object(result.value_or({ .route = { InputRoutingDecision::NoAnswerFromCompositor }, .document_id = {} }));
}

namespace {

struct RecordedScrollTree {
    Compositing::AsyncScrollTree scroll_tree;
    Compositing::AccumulatedVisualContextTree visual_context_tree;
    Compositing::WheelRoutingAdmission admission;
};

}

// The scroll tree of the display list the document last recorded, as the compositor builds it once the list reaches it.
static Optional<RecordedScrollTree> recorded_scroll_tree_of(DOM::Document& document, Layout::BegunRead const& read)
{
    auto* display_list = document.paint_state().display_list_used_as_paint_command_cache_source();
    if (!display_list)
        return {};
    RecordedScrollTree recorded {
        .scroll_tree = {},
        .visual_context_tree = document.paint_state().visual_context_tree_without_update(read, document),
        .admission = {},
    };
    auto async_scrolling_state = Compositing::async_scrolling_state_from_display_list(*display_list);
    recorded.admission = Compositing::wheel_routing_admission_for(async_scrolling_state, document.wheel_event_listener_state_generation());
    recorded.scroll_tree.set_state(move(async_scrolling_state));
    recorded.scroll_tree.rebuild_wheel_hit_test_targets(display_list, &recorded.visual_context_tree, document.scroll_state_snapshot());
    return recorded;
}

GC::Ref<JS::Object> Internals::recorded_display_list_wheel_target_at(double x, double y, double delta_x, double delta_y)
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto recorded = recorded_scroll_tree_of(window().associated_document(), read);
    if (!recorded.has_value())
        return first_wheel_step_route_to_object({ .route = { InputRoutingDecision::NoAsyncScrollingState }, .document_id = {} });
    auto const& scroll_tree = recorded->scroll_tree;

    auto position = device_position_as_float_point(page().css_to_device_point({ x, y }));
    auto delta = Gfx::FloatPoint { static_cast<float>(delta_x), static_cast<float>(delta_y) }.scaled(static_cast<float>(scroll_tree.device_pixels_per_css_pixel()));
    return first_wheel_step_route_to_object({
        .route = scroll_tree.route_first_wheel_step(recorded->admission, recorded->visual_context_tree, position, delta),
        .document_id = scroll_tree.document_id(),
    });
}

bool Internals::recorded_display_list_blocks_wheel_event_at(double x, double y)
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto recorded = recorded_scroll_tree_of(window().associated_document(), read);
    if (!recorded.has_value())
        return false;
    auto position = device_position_as_float_point(page().css_to_device_point({ x, y }));
    return recorded->scroll_tree.blocks_wheel_event_at_position(recorded->visual_context_tree, position);
}

void Internals::record_display_list_for_testing(bool paint_overlay, bool cold)
{
    auto& document = window().associated_document();
    Layout::ForcedReadScope read { document };
    document.update_layout(DOM::UpdateLayoutReason::InternalsHitTest);
    if (!document.navigable() || !document.has_committed_viewport_box())
        return;
    if (cold)
        document.paint_state().invalidate_all_cached_paint(document);
    (void)document.record_display_list(read, HTML::PaintConfig { .paint_overlay = paint_overlay }, document.navigable()->display_list_resource_storage(), Painting::PaintCommandCacheMode::ReadWrite);
}

void Internals::set_autoplay_policy(Utf16String const& policy)
{
    if (auto parsed = HTML::autoplay_policy_from_string(policy.utf16_view()); parsed.has_value())
        HTML::AutoplaySettings::the().set_policy(*parsed, {});
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static
Utf16String Internals::get_computed_role(DOM::Element& element)
{
    if (auto role = element.role_or_default(); role.has_value())
        return ARIA::role_name(role.value());
    return Utf16String {};
}

Utf16String Internals::get_computed_label(DOM::Element& element)
{
    auto& active_document = window().associated_document();
    return MUST(element.accessible_name(active_document));
}

Utf16String Internals::get_computed_aria_level(DOM::Element& element)
{
    auto aria_data = MUST(ARIA::AriaData::build_data(element));
    return MUST(ARIA::state_or_property_to_string_value(ARIA::StateAndProperties::AriaLevel, *aria_data));
}

void Internals::set_accessibility_focus_target(DOM::Element& element)
{
    element.document().set_accessibility_focus_target(&element);
}

u16 Internals::get_echo_server_port()
{
    return s_echo_server_port;
}

void Internals::set_echo_server_port(u16 const port)
{
    s_echo_server_port = port;
}

u16 Internals::get_https_echo_server_port()
{
    return s_https_echo_server_port;
}

void Internals::set_https_echo_server_port(u16 const port)
{
    s_https_echo_server_port = port;
}

WebIDL::ExceptionOr<void> Internals::set_hsts_policy(Utf16String const& domain, u64 max_age, bool include_sub_domains)
{
    auto domain_utf8 = TRY_OR_THROW_OOM(vm(), domain.utf16_view().to_utf8());

    // NB: Clamp to i64::max so AK::Duration::from_seconds cannot overflow, mirroring the HSTS header parser.
    auto clamped_seconds = AK::min<u64>(max_age, NumericLimits<i64>::max());
    page().client().page_did_store_hsts_policy_for_testing(domain_utf8, HTTP::HSTS::ParsedHSTSPolicy {
                                                                            AK::Duration::from_seconds(static_cast<i64>(clamped_seconds)),
                                                                            include_sub_domains,
                                                                        });
    return {};
}

WebIDL::ExceptionOr<void> Internals::ingest_hsts_header(Utf16String const& url, Utf16String const& header_value)
{
    auto url_utf8 = TRY_OR_THROW_OOM(vm(), url.utf16_view().to_utf8());
    auto parsed_url = URL::Parser::basic_parse(url_utf8);
    if (!parsed_url.has_value())
        return {};

    // https://www.rfc-editor.org/rfc/rfc6797#section-8.1
    // If the substring matching the host production from the Request-URI (of the message to which the host responded)
    // syntactically matches the IP-literal or IPv4address productions from Section 3.2.2 of [RFC3986], then the UA
    // MUST NOT note this host as a Known HSTS Host.
    if (!parsed_url->host().has_value() || !parsed_url->host()->is_domain())
        return {};

    auto header_value_utf8 = TRY_OR_THROW_OOM(vm(), header_value.utf16_view().to_utf8());
    auto parsed_policy = HTTP::HSTS::parse_header(header_value_utf8);
    if (!parsed_policy.has_value())
        return {};

    page().client().page_did_store_hsts_policy_for_testing(parsed_url->host()->get<String>(), parsed_policy.value());
    return {};
}

WebIDL::ExceptionOr<bool> Internals::is_known_hsts_host(Utf16String const& domain)
{
    auto domain_utf8 = TRY_OR_THROW_OOM(vm(), domain.utf16_view().to_utf8());
    return ResourceLoader::is_known_hsts_host(page(), domain_utf8);
}

void Internals::set_browser_zoom(double factor)
{
    page().client().page_did_set_browser_zoom(factor);
}

void Internals::set_device_pixel_ratio(double ratio)
{
    page().client().page_did_set_device_pixel_ratio_for_testing(ratio);
}

void Internals::resize_window(i32 width, i32 height)
{
    page().client().page_did_request_resize_window({ width, height }, 0);
}

bool Internals::headless()
{
    return page().client().is_headless();
}

bool Internals::needs_repaint()
{
    Layout::ForcedReadScope read { window().associated_document() };
    window().associated_document().update_highlight_states_if_needed(read);
    auto local_root = local_root_of(window());
    return local_root && local_root->needs_repaint();
}

bool Internals::has_animation_frame_callbacks()
{
    return window().has_animation_frame_callbacks();
}

bool Internals::needs_display_list_record()
{
    Layout::ForcedReadScope read { window().associated_document() };
    window().associated_document().update_highlight_states_if_needed(read);
    auto local_root = local_root_of(window());
    return local_root && local_root->needs_to_record_display_list();
}

bool Internals::screen_wake_lock_active()
{
    return page().is_screen_wake_lock_active();
}

Utf16String Internals::picture_in_picture_window_state()
{
    auto const& controller = page().picture_in_picture_controller();
    if (controller.is_waiting_for_window_to_open())
        return "opening"_utf16;
    if (controller.has_open_window())
        return "open"_utf16;
    return "none"_utf16;
}

static Utf16String dump_string_to_utf16(String const& string)
{
    return Utf16String::from_utf8_without_validation(string.bytes_as_string_view());
}

Utf16String Internals::dump_display_list()
{
    return window().associated_document().dump_display_list();
}

Utf16String Internals::dump_accessibility_tree()
{
    return window().associated_document().dump_accessibility_tree_as_json();
}

Utf16String Internals::dump_layout_tree(GC::Ref<DOM::Node> node)
{
    // The test API's read of the render state of the node's document.
    Layout::ForcedReadScope read { node->document() };
    if (auto navigable = node->document().navigable())
        navigable->update_layout_of_hosted_inclusive_descendant_documents(DOM::UpdateLayoutReason::Debugging);

    auto* layout_node = node->layout_node(read);
    if (!layout_node)
        return "(no layout node)"_utf16;

    StringBuilder builder;
    Web::dump_tree(builder, *layout_node);
    return dump_string_to_utf16(builder.to_string_without_validation());
}

Utf16String Internals::dump_stacking_context_tree()
{
    return window().associated_document().dump_stacking_context_tree();
}

Utf16String Internals::stacking_context_structure_verification_report()
{
    auto& document = window().associated_document();
    Layout::ForcedReadScope read { document };
    document.update_layout(DOM::UpdateLayoutReason::DumpDisplayList);
    if (!document.has_committed_viewport_box())
        return {};
    document.update_paint_and_hit_testing_properties_if_needed();
    StringBuilder builder;
    Layout::RustFFI::render_state_stacking_context_structure_verification_report(
        document.layout_node_arena().host(), Painting::viewport_row_slot(read, document), &builder,
        [](void* context, u8 const* bytes, size_t byte_count) {
            static_cast<StringBuilder*>(context)->append(StringView { bytes, byte_count });
        });
    return Utf16String::from_utf8_without_validation(builder.string_view());
}

Utf16String Internals::dump_gc_graph()
{
    return dump_string_to_utf16(GC::Heap::the().dump_graph().serialized());
}

Utf16String Internals::dump_session_history()
{
    auto& document = window().associated_document();
    auto navigable = document.navigable();
    if (!navigable)
        return "(no navigable)"_utf16;

    auto serialized_history = document.page().client().page_did_request_ui_process_session_history_for_testing();
    auto parsed_history = JsonValue::from_string(serialized_history);
    if (parsed_history.is_error() || !parsed_history.value().is_object())
        return "(invalid UI process session history)"_utf16;

    auto const& history = parsed_history.value().as_object();
    auto top_level_entries = history.get_array("entries"sv);
    auto used_steps = history.get_array("usedSteps"sv);
    if (!top_level_entries.has_value() || !used_steps.has_value())
        return "(invalid UI process session history)"_utf16;

    Optional<i32> current_step;
    for (auto const& step : used_steps->values()) {
        if (!step.is_object())
            continue;
        auto const& step_object = step.as_object();
        if (step_object.get_bool("current"sv).value_or(false)) {
            current_step = step_object.get_i32("step"sv);
            break;
        }
    }

    auto const* entries = &*top_level_entries;
    if (!navigable->is_traversable()) {
        auto navigable_id = MUST(String::formatted("{}", navigable->id()));
        Function<JsonArray const*(JsonArray const&)> find_entries =
            [&](JsonArray const& candidate_entries) -> JsonArray const* {
            for (auto const& candidate_entry : candidate_entries.values()) {
                if (!candidate_entry.is_object())
                    continue;
                auto nested_histories = candidate_entry.as_object().get_array("nestedHistories"sv);
                if (!nested_histories.has_value())
                    continue;
                for (auto const& nested_history : nested_histories->values()) {
                    if (!nested_history.is_object())
                        continue;
                    auto const& nested_history_object = nested_history.as_object();
                    auto nested_entries = nested_history_object.get_array("entries"sv);
                    if (!nested_entries.has_value())
                        continue;
                    if (auto nested_history_id = nested_history_object.get_string("id"sv);
                        nested_history_id.has_value()
                        && *nested_history_id == navigable_id) {
                        return &*nested_entries;
                    }
                    if (auto const* result = find_entries(*nested_entries))
                        return result;
                }
            }
            return nullptr;
        };
        entries = find_entries(*top_level_entries);
        if (!entries)
            return "(no canonical session history entries)"_utf16;
    }

    // Find the minimum step to use as a base offset, so output is stable across test runs.
    Optional<int> min_step;
    for (auto const& entry : entries->values()) {
        if (!entry.is_object())
            continue;
        auto step = entry.as_object().get_i32("step"sv);
        if (step.has_value()
            && (!min_step.has_value() || *step < *min_step)) {
            min_step = *step;
        }
    }

    StringBuilder builder;
    for (auto const& entry : entries->values()) {
        if (!entry.is_object())
            continue;
        auto const& entry_object = entry.as_object();
        auto step = entry_object.get_i32("step"sv);
        auto serialized_url = entry_object.get_string("url"sv);
        if (!step.has_value() || !serialized_url.has_value())
            continue;
        auto url = URL::Parser::basic_parse(*serialized_url);
        if (!url.has_value())
            continue;
        auto filename = url->basename();
        StringBuilder display_builder;
        display_builder.append(filename);
        if (url->query().has_value())
            display_builder.appendff("?{}", *url->query());
        if (url->fragment().has_value())
            display_builder.appendff("#{}", *url->fragment());
        auto display = display_builder.to_string_without_validation();
        auto is_current = current_step == step;
        auto relative_step = min_step.has_value()
            ? String::number(*step - *min_step)
            : "pending"_string;
        builder.appendff("  step {} {}{}\n", relative_step, display, is_current ? " (current)"sv : ""sv);
    }
    return dump_string_to_utf16(builder.to_string_without_validation());
}

Utf16String Internals::dump_ui_process_session_history()
{
    auto& document = window().associated_document();
    return dump_string_to_utf16(document.page().client().page_did_request_ui_process_session_history_for_testing());
}

Utf16String Internals::dump_ui_process_session_history_without_update()
{
    return dump_string_to_utf16(window().associated_document().page().client().page_did_request_ui_process_session_history_for_testing());
}

bool Internals::capture_session_history_snapshot()
{
    return window().associated_document().page().client().page_did_request_capture_session_history_snapshot_for_testing();
}

bool Internals::restore_captured_session_history_snapshot()
{
    return window().associated_document().page().client().page_did_request_restore_session_history_snapshot_for_testing();
}

bool Internals::register_session_store_tab()
{
    return window().associated_document().page().client().page_did_request_register_session_store_tab_for_testing();
}

Utf16String Internals::dump_session_store_tab_state()
{
    return dump_string_to_utf16(window().associated_document().page().client().page_did_request_session_store_tab_state_for_testing());
}

bool Internals::has_populated_document(HTML::HTMLIFrameElement& iframe)
{
    auto navigable = iframe.content_navigable();
    if (!navigable)
        return false;
    return window().associated_document().page().client().page_did_request_has_populated_document_for_testing(navigable->id());
}

bool Internals::canvas_has_unpresented_content(HTML::HTMLCanvasElement& canvas)
{
    return canvas.has_unpresented_content();
}

Utf16String Internals::dump_site_isolation_process_tree()
{
    return dump_string_to_utf16(window().associated_document().page().client().dump_site_isolation_process_tree_for_testing());
}

void Internals::crash_remote_frame_processes()
{
    window().associated_document().page().client().crash_remote_frame_processes_for_testing();
}

void Internals::stop_loading_through_ui_process()
{
    window().associated_document().page().client().stop_loading_through_ui_process_for_testing();
}

void Internals::reload_through_ui_process()
{
    window().associated_document().page().client().reload_through_ui_process_for_testing(false);
}

void Internals::reload_bypassing_cache_through_ui_process()
{
    window().associated_document().page().client().reload_through_ui_process_for_testing(true);
}

void Internals::traverse_history_through_ui_process(i32 delta)
{
    window().associated_document().page().client().traverse_history_by_delta_through_ui_process_for_testing(delta);
}

GC::Ref<WebIDL::Promise> Internals::flush_session_history_traversal_queue()
{
    auto& realm = window().principal_realm();
    auto promise = WebIDL::create_promise_for(window());
    auto& document = window().associated_document();
    auto navigable = document.navigable();
    if (!navigable) {
        WebIDL::resolve_promise(promise);
        return promise;
    }

    navigable->page().history_executor().request_history_operation(
        FlushSessionHistoryTraversalQueueOperationParameters {},
        {
            .on_complete = GC::create_function(heap(), [&realm, promise] {
                HTML::TemporaryExecutionContext execution_context { realm };
                WebIDL::resolve_promise(promise);
            }),
        });
    return promise;
}

bool Internals::has_shadow_root(GC::Ref<DOM::Element> element)
{
    return element->shadow_root() != nullptr;
}

GC::Ptr<DOM::ShadowRoot> Internals::get_shadow_root(GC::Ref<DOM::Element> element)
{
    if (auto* input = as_if<HTML::HTMLInputElement>(*element))
        input->ensure_user_agent_shadow_tree({});
    return element->shadow_root();
}

void Internals::pump_gamepad_events()
{
    page().client().pump_gamepad_events();
}

GC::Ref<InternalGamepad> Internals::connect_virtual_gamepad()
{
    auto virtual_gamepad = page().client().create_virtual_gamepad();
    VERIFY(virtual_gamepad.has_value());
    auto gamepad = InternalGamepad::create(window(), *this, virtual_gamepad.release_value());
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
    if (auto local_root = local_root_of(window()))
        local_root->event_handler().clear_per_test_input_state({});
    if (page().input_trace().is_recording())
        (void)take_input_trace_records(window());

    // Restore the page to the visible state.
    page().client().page_did_request_set_system_visibility_state(HTML::VisibilityState::Visible);
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

void Internals::set_environments_top_level_url(Utf16String const& url)
{
    window().associated_document().relevant_settings_object().top_level_creation_url = URL::Parser::basic_parse(url.utf16_view());
}

void Internals::set_geolocation_emulated_position(double latitude, double longitude, double accuracy)
{
    page().set_emulated_position_data(Geolocation::CoordinatesData {
        .accuracy = accuracy,
        .latitude = latitude,
        .longitude = longitude,
    });
}

bool Internals::html_parser_body_is_exhausted(DOM::Document& document)
{
    auto parser = document.parser();
    return parser && parser->streaming_body_is_exhausted();
}

u64 Internals::parser_non_append_insertions()
{
    return HTML::parser_non_append_insertions();
}

bool Internals::media_element_is_fetching(HTML::HTMLMediaElement& element)
{
    return element.is_fetching();
}

bool Internals::media_element_is_playing_audio(HTML::HTMLMediaElement& element)
{
    return element.is_playing_audio();
}

bool Internals::media_element_video_sink_is_ticking(HTML::HTMLMediaElement& element)
{
    return element.m_video_sink_is_ticking;
}

void Internals::set_media_element_ready_state(HTML::HTMLMediaElement& element, u16 ready_state)
{
    if (ready_state > to_underlying(HTML::HTMLMediaElement::ReadyState::HaveEnoughData))
        return;
    element.set_ready_state(static_cast<HTML::HTMLMediaElement::ReadyState>(ready_state));
}

void Internals::set_media_element_paused(HTML::HTMLMediaElement& element, bool paused)
{
    element.set_paused(paused);
}

void Internals::set_media_element_seeking(HTML::HTMLMediaElement& element, bool seeking)
{
    element.set_seeking(seeking);
}

void Internals::disconnect_media_server()
{
    if (auto client = MediaClient::Client::acquire(); !client.is_error())
        client.value()->shutdown();
}

void Internals::set_page_muted(bool muted)
{
    window().associated_document().page().set_page_mute_state(muted ? HTML::MuteState::Muted : HTML::MuteState::Unmuted);
}

DOM::Document::StyleInvalidationCounters const& Internals::style_invalidation_counters() const
{
    return window().associated_document().style_invalidation_counters();
}

void Internals::reset_style_invalidation_counters()
{
    Layout::ForcedReadScope read { window().associated_document() };
    window().associated_document().reset_style_invalidation_counters(read);
}

GC::Ref<JS::Object> Internals::get_rendering_scheduler_counters() const
{
    auto& realm = HTML::relevant_realm(window());
    auto const& counters = HTML::main_thread_event_loop().rendering_scheduler_counters();
    auto object = JS::Object::create(realm, nullptr);
    object->define_direct_property("updateRequests"_utf16_fly_string, JS::Value(counters.update_requests), JS::default_attributes);
    object->define_direct_property("coalescedUpdateRequests"_utf16_fly_string, JS::Value(counters.coalesced_update_requests), JS::default_attributes);
    object->define_direct_property("updateRequestsWhileRendering"_utf16_fly_string, JS::Value(counters.update_requests_while_rendering), JS::default_attributes);
    object->define_direct_property("opportunitiesReceived"_utf16_fly_string, JS::Value(counters.opportunities_received), JS::default_attributes);
    object->define_direct_property("opportunitiesThatQueuedATask"_utf16_fly_string, JS::Value(counters.opportunities_that_queued_a_task), JS::default_attributes);
    object->define_direct_property("watchdogOpportunities"_utf16_fly_string, JS::Value(counters.watchdog_opportunities), JS::default_attributes);
    object->define_direct_property("updatesRun"_utf16_fly_string, JS::Value(counters.updates_run), JS::default_attributes);
    object->define_direct_property("updatesSkippedAsUnnecessary"_utf16_fly_string, JS::Value(counters.updates_skipped_as_unnecessary), JS::default_attributes);
    object->define_direct_property("updateMicroseconds"_utf16_fly_string, JS::Value(counters.update_microseconds), JS::default_attributes);
    object->define_direct_property("tasksBetweenUpdates"_utf16_fly_string, JS::Value(counters.tasks_between_updates), JS::default_attributes);
    object->define_direct_property("taskMicrosecondsBetweenUpdates"_utf16_fly_string, JS::Value(counters.task_microseconds_between_updates), JS::default_attributes);
    object->define_direct_property("postedMessageTasksBetweenUpdates"_utf16_fly_string, JS::Value(counters.posted_message_tasks_between_updates), JS::default_attributes);
    object->define_direct_property("postedMessageTaskMicrosecondsBetweenUpdates"_utf16_fly_string, JS::Value(counters.posted_message_task_microseconds_between_updates), JS::default_attributes);
    object->define_direct_property("timerTasksBetweenUpdates"_utf16_fly_string, JS::Value(counters.timer_tasks_between_updates), JS::default_attributes);
    object->define_direct_property("timerTaskMicrosecondsBetweenUpdates"_utf16_fly_string, JS::Value(counters.timer_task_microseconds_between_updates), JS::default_attributes);
    object->define_direct_property("networkingTasksBetweenUpdates"_utf16_fly_string, JS::Value(counters.networking_tasks_between_updates), JS::default_attributes);
    object->define_direct_property("networkingTaskMicrosecondsBetweenUpdates"_utf16_fly_string, JS::Value(counters.networking_task_microseconds_between_updates), JS::default_attributes);
    object->define_direct_property("domManipulationTasksBetweenUpdates"_utf16_fly_string, JS::Value(counters.dom_manipulation_tasks_between_updates), JS::default_attributes);
    object->define_direct_property("domManipulationTaskMicrosecondsBetweenUpdates"_utf16_fly_string, JS::Value(counters.dom_manipulation_task_microseconds_between_updates), JS::default_attributes);
    object->define_direct_property("paints"_utf16_fly_string, JS::Value(counters.paints), JS::default_attributes);
    return object;
}

void Internals::reset_rendering_scheduler_counters()
{
    HTML::main_thread_event_loop().reset_rendering_scheduler_counters();
}

void Internals::inject_clock_tick(double frame_time_ms, Optional<double> viewport_scroll_y)
{
    auto& document = window().associated_document();
    auto frame_time = document.relevant_settings_object().time_origin() + frame_time_ms;
    auto scroll_offset = viewport_scroll_y.map([&](double y) { return Layout::RustFFI::FfiScrollOffset { document.unique_id().value(), 0, y }; });
    Layout::RustFFI::document_host_inject_clock_tick(document.layout_node_arena().host(), static_cast<i64>(frame_time * 1'000'000.0), scroll_offset.has_value() ? &*scroll_offset : nullptr);
}

void Internals::inject_hover_pointer(double x, double y, Optional<double> frame_time_ms, Optional<u64> input_event_id)
{
    auto& document = window().associated_document();
    auto device_pixels_per_css_pixel = page().client().device_pixels_per_css_pixel();
    auto frame_time = frame_time_ms.has_value() ? document.relevant_settings_object().time_origin() + *frame_time_ms : HighResolutionTime::unsafe_shared_current_time();
    Layout::RustFFI::document_host_inject_pointer(document.layout_node_arena().host(), static_cast<float>(x * device_pixels_per_css_pixel), static_cast<float>(y * device_pixels_per_css_pixel), static_cast<i64>(frame_time * 1'000'000.0), input_event_id.value_or(0));
}

u64 Internals::queue_mouse_move(double x, double y, u32 buttons)
{
    // NB: Above every id a UI process hands out in a test.
    static u64 next_input_event_id = 1ull << 40;
    auto& page = this->page();
    auto position = page.css_to_device_point(window().navigable()->to_page_position({ x, y }));
    Web::MouseEvent event;
    event.type = Web::MouseEvent::Type::MouseMove;
    event.position = position;
    event.screen_position = position;
    event.buttons = static_cast<UIEvents::MouseButton>(buttons);
    event.id = next_input_event_id++;
    auto id = event.id;
    page.client().input_event_queue().enqueue({ page.client().id(), move(event), {}, {} });
    return id;
}

void Internals::move_hover_pointer(double x, double y, Optional<u64> input_event_id)
{
    auto& document = window().associated_document();
    auto device_pixels_per_css_pixel = page().client().device_pixels_per_css_pixel();
    Layout::RustFFI::document_host_move_pointer(document.layout_node_arena().host(), static_cast<float>(x * device_pixels_per_css_pixel), static_cast<float>(y * device_pixels_per_css_pixel), input_event_id.value_or(0));
}

Utf16String Internals::clock_lane_state(DOM::Document& document)
{
    switch (Layout::RustFFI::document_host_clock_lane_state(document.layout_node_arena().host())) {
    case Layout::RustFFI::FfiClockLaneState::None:
        return "none"_utf16;
    case Layout::RustFFI::FfiClockLaneState::Ticking:
        return "ticking"_utf16;
    case Layout::RustFFI::FfiClockLaneState::Parked:
        return "parked"_utf16;
    case Layout::RustFFI::FfiClockLaneState::Hovering:
        return "hovering"_utf16;
    }
    VERIFY_NOT_REACHED();
}

bool Internals::clock_lane_is_coming(DOM::Document& document)
{
    return Layout::RustFFI::document_host_clock_lane_is_coming(document.layout_node_arena().host());
}

GC::Ptr<Geometry::DOMRect> Internals::presented_border_box(DOM::Element& element)
{
    CSSPixelRect rect;
    if (!element.style_node_id() || !Layout::RustFFI::document_host_presented_border_box(element.document().layout_node_arena().host(), element.style_node_id().value(), &rect))
        return nullptr;
    return Geometry::DOMRect::create(rect.x().to_double(), rect.y().to_double(), rect.width().to_double(), rect.height().to_double());
}

Optional<u32> Internals::presented_compositor_animation_count(DOM::Element& element)
{
    u32 count = 0;
    if (!element.style_node_id() || !Layout::RustFFI::document_host_presented_compositor_animation_count(element.document().layout_node_arena().host(), element.style_node_id().value(), &count))
        return {};
    return count;
}

Optional<double> Internals::presented_opacity(DOM::Element& element)
{
    float opacity = 0;
    if (!element.style_node_id() || !Layout::RustFFI::document_host_presented_opacity(element.document().layout_node_arena().host(), element.style_node_id().value(), &opacity))
        return {};
    return opacity;
}

Optional<String> Internals::presented_color(DOM::Element& element, Optional<Utf16String> const& pseudo_element)
{
    u8 generated_for = 0;
    if (pseudo_element.has_value()) {
        if (*pseudo_element == "::before"sv)
            generated_for = Layout::RustFFI::GENERATED_FOR_BEFORE;
        else if (*pseudo_element == "::after"sv)
            generated_for = Layout::RustFFI::GENERATED_FOR_AFTER;
        else if (*pseudo_element == "::marker"sv)
            generated_for = Layout::RustFFI::GENERATED_FOR_MARKER;
        else
            return {};
    }
    u32 argb = 0;
    if (!element.style_node_id() || !Layout::RustFFI::document_host_presented_color(element.document().layout_node_arena().host(), element.style_node_id().value(), generated_for, &argb))
        return {};
    return Color::from_bgra(argb).serialize_a_srgb_value();
}

void Internals::set_manual_rendering_opportunities(bool enabled)
{
    // A test that injects its rendering opportunities injects its clock ticks too: the clock lanes tick with the display
    // no more, and the frame in flight reaches the compositor first.
    Compositor::RenderClock::the().set_manual_for_testing(enabled);
    if (enabled) {
        // A lane armed before ticks with the display until it declines a tick, at the compositor's own scroll offsets.
        Compositor::RenderClock::the().disarm_all_for_testing();
        if (auto* navigable = as_if<HTML::LocalNavigable>(window().associated_document().navigable().ptr()))
            (void)navigable->presenter();
    }
    page().client().set_manual_rendering_opportunities(enabled);
}

void Internals::inject_rendering_opportunity(double frame_time_ms)
{
    auto frame_time = window().associated_document().relevant_settings_object().time_origin() + frame_time_ms;
    page().client().inject_rendering_opportunity(frame_time);
}

Utf16String Internals::frame_scheduler_state() const
{
    auto const& event_loop = HTML::main_thread_event_loop();
    if (event_loop.lays_out_rendering_update_in_flight())
        return "laying-out"_utf16;
    if (event_loop.has_frame_in_flight())
        return "in-flight"_utf16;
    if (event_loop.has_rendering_update_in_flight())
        return "landed"_utf16;
    return "idle"_utf16;
}

void Internals::hold_next_frame(Utf16String const& hold)
{
    Layout::RustFFI::render_state_hold_next_recording_for_testing();
    if (hold == "layout"sv) {
        HTML::main_thread_event_loop().hold_next_layout_for_testing();
        return;
    }
    HTML::main_thread_event_loop().hold_next_frame_for_testing();
}

bool Internals::last_frame_keyboard_scroll_state_is_current()
{
    auto& page = this->page();
    if (!page.has_local_traversable())
        return false;
    return page.local_traversable()->presenter().last_keyboard_scroll_state_generation() == page.keyboard_scroll_state_generation();
}

Utf16String Internals::last_frame_presented_by(DOM::Document& document)
{
    auto* navigable = as_if<HTML::LocalNavigable>(document.navigable().ptr());
    if (!navigable)
        return {};
    auto presented_by = navigable->presenter().last_frame_presented_by();
    if (!presented_by.has_value())
        return "none"_utf16;
    switch (*presented_by) {
    case Compositor::PresentedBy::Commit:
        return "commit"_utf16;
    case Compositor::PresentedBy::Clock:
        return "clock"_utf16;
    }
    VERIFY_NOT_REACHED();
}

void Internals::release_held_frame()
{
    Layout::RustFFI::render_state_release_held_recording_for_testing();
    HTML::main_thread_event_loop().release_held_frames_for_testing();
}

bool Internals::rendering_update_holds_tasks_of(DOM::Document& document)
{
    return HTML::main_thread_event_loop().rendering_update_in_flight_holds_tasks_of_for_testing(document);
}

void Internals::update_compositor_animations()
{
    Layout::ForcedReadScope read { window().associated_document() };
    window().associated_document().update_compositor_animations(read);
}

bool Internals::run_empty_animation_style_update_for_testing()
{
    return window().associated_document().run_empty_animation_style_update_for_testing({});
}

void Internals::arm_compositor_animation_timers_for_testing()
{
    window().associated_document().arm_compositor_animation_timers_for_testing({});
}

void Internals::fire_compositor_animation_wakeup_for_testing(double frame_time_ms)
{
    window().associated_document().fire_compositor_animation_wakeup_for_testing({}, frame_time_ms);
}

void Internals::force_visual_context_tree_rebuild_on_next_compositor_animation_update_for_testing()
{
    window().associated_document().force_visual_context_tree_rebuild_on_next_compositor_animation_update_for_testing({});
}

void Internals::request_reentrant_animation_style_flush_for_testing(GC::Ref<DOM::Node> node)
{
    window().associated_document().request_reentrant_animation_style_flush_for_testing({}, node);
}

GC::Ref<JS::Object> Internals::layout_tree_build_stats()
{
    auto object = JS::Object::create(window().principal_realm(), nullptr);
    auto counts = window().associated_document().layout_counts();
    object->define_direct_property("builds"_utf16_fly_string, JS::Value(counts.tree_builds), JS::default_attributes);
    object->define_direct_property("lastBuildRebuiltSubtreeRoots"_utf16_fly_string, JS::Value(counts.last_tree_build_rebuilt_subtree_roots), JS::default_attributes);
    object->define_direct_property("lastBuildEscapedRebuildRoots"_utf16_fly_string, JS::Value(counts.last_tree_build_escaped_rebuild_roots), JS::default_attributes);
    return object;
}

GC::Ref<JS::Object> Internals::compare_layout_tree_with_full_rebuild()
{
    auto& document = window().associated_document();

    auto snapshot_layout_tree = [&]() {
        Layout::ForcedReadScope read { document };
        document.update_layout(DOM::UpdateLayoutReason::Debugging);
        VERIFY(document.layout_node(read));

        StringBuilder builder;
        Web::dump_tree(builder, *document.layout_node(read));
        return builder.to_string_without_validation();
    };

    auto incremental_tree = snapshot_layout_tree();

    // Deliberately tear down every layout node. The next snapshot is therefore produced by the
    // builder's full-tree path and acts as an oracle for the incremental result above.
    document.invalidate_layout_tree(DOM::InvalidateLayoutTreeReason::InternalsFullRebuildComparison);
    auto full_tree = snapshot_layout_tree();

    auto object = JS::Object::create(window().principal_realm(), nullptr);
    auto matches = incremental_tree == full_tree;
    object->define_direct_property("matches"_utf16_fly_string, JS::Value(matches), JS::default_attributes);
    if (!matches) {
        object->define_direct_property("incremental"_utf16_fly_string, JS::PrimitiveString::create(vm(), dump_string_to_utf16(incremental_tree)), JS::default_attributes);
        object->define_direct_property("full"_utf16_fly_string, JS::PrimitiveString::create(vm(), dump_string_to_utf16(full_tree)), JS::default_attributes);
    }
    return object;
}

GC::Ref<JS::Object> Internals::style_ffi_counters()
{
    auto object = JS::Object::create(window().principal_realm(), nullptr);
    auto const counter_count = CSS::StyleValueFFI::rust_style_ffi_counter_count();
    for (size_t index = 0; index < counter_count; ++index) {
        auto const* name = reinterpret_cast<char const*>(CSS::StyleValueFFI::rust_style_ffi_counter_name(index));
        auto const value = CSS::StyleValueFFI::rust_style_ffi_counter_value(index);
        object->define_direct_property(
            Utf16FlyString::from_utf8(StringView { name, strlen(name) }),
            JS::Value(static_cast<double>(value)),
            JS::default_attributes);
    }
    return object;
}

GC::Ref<JS::Object> Internals::style_engine_counters()
{
    Layout::ForcedReadScope read { window().associated_document() };
    auto& realm = HTML::relevant_realm(window());
    auto object = JS::Object::create(realm, nullptr);
    auto& style_engine = window().associated_document().style_computer().style_engine();
    StringView name;
    u64 value = 0;
    for (size_t index = 0; style_engine.counter(read, index, name, value); ++index) {
        object->define_direct_property(
            Utf16FlyString::from_utf8(name),
            JS::Value(static_cast<double>(value)),
            JS::default_attributes);
    }
    object->define_direct_property(
        "computedStyleRecordViewPins"_utf16_fly_string,
        JS::Value(static_cast<double>(window().associated_document().style_computer().computed_style_record_view_pin_count())),
        JS::default_attributes);
    return object;
}

u64 Internals::style_record_identity(DOM::Element& element)
{
    return element.style_record_identity().value();
}

u64 Internals::layout_style_record_identity(DOM::Element& element)
{
    Layout::ForcedReadScope read { element.document() };
    element.document().update_layout(DOM::UpdateLayoutReason::Debugging);
    auto const* layout_node = element.layout_node(read);
    return layout_node ? layout_node->style_record_identity().value() : 0;
}

u64 Internals::before_style_record_identity(DOM::Element& element)
{
    return element.style_record_identity(CSS::PseudoElement::Before).value();
}

u64 Internals::before_layout_style_record_identity(DOM::Element& element)
{
    Layout::ForcedReadScope read { element.document() };
    element.document().update_layout(DOM::UpdateLayoutReason::Debugging);
    auto const* layout_node = element.pseudo_element_layout_node(read, CSS::PseudoElement::Before);
    return layout_node ? layout_node->style_record_identity().value() : 0;
}

u64 Internals::paint_style_record_identity(DOM::Element& element)
{
    Layout::ForcedReadScope read { element.document() };
    element.document().update_layout(DOM::UpdateLayoutReason::Debugging);
    auto const* layout_node = element.layout_node(read);
    if (!layout_node || !Painting::has_committed_box(*layout_node))
        return 0;
    return Painting::style_record_identity(*layout_node).value();
}

u64 Internals::layout_node_identity(DOM::Node& node)
{
    Layout::ForcedReadScope read { node.document() };
    node.document().update_layout(DOM::UpdateLayoutReason::Debugging);
    auto const* layout_node = node.layout_node(read);
    return layout_node ? static_cast<u64>(layout_node->arena_slot_index()) + 1 : 0;
}

u64 Internals::layout_arena_live_slot_count()
{
    auto& document = window().associated_document();
    document.update_layout(DOM::UpdateLayoutReason::Debugging);
    return Layout::RustFFI::render_state_layout_counts(document.layout_node_arena().host()).live_slots;
}

void Internals::panic_render_state_for_testing()
{
    Layout::RustFFI::render_state_panic_for_testing(window().associated_document().layout_node_arena().host());
}

u64 Internals::layout_arena_shell_count()
{
    auto& document = window().associated_document();
    document.update_layout(DOM::UpdateLayoutReason::Debugging);
    return Layout::RustFFI::render_state_shell_count(document.layout_node_arena().host());
}

GC::Ref<JS::Object> Internals::style_engine_transaction_reactions()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto& realm = HTML::relevant_realm(window());
    auto& document = window().associated_document();
    auto& style_computer = document.style_computer();
    auto object = JS::Object::create(realm, nullptr);

    auto* root = document.document_element();
    if (!root || root->style_node_id() == 0)
        return object;

    auto tags = MUST(JS::Array::create(realm, 0));
    u32 index = 0;
    Function<void(ReadonlySpan<CSS::StyleNodeID>)> consume = [&](ReadonlySpan<CSS::StyleNodeID> style_node_ids) {
        for (auto style_node_id : style_node_ids) {
            auto element = style_computer.element_for_style_node(style_node_id);
            if (!element)
                continue;
            auto identity = element->id().has_value() ? *element->id() : element->local_name();
            MUST(tags->create_data_property_or_throw(index++, JS::PrimitiveString::create(vm(), identity)));
        }
    };
    auto transaction_is_scoped = style_computer.style_engine().take_diagnostic_style_transaction(read, root->style_node_id(), move(consume));
    object->define_direct_property("wholeDocument"_utf16_fly_string, JS::Value(!transaction_is_scoped), JS::default_attributes);
    object->define_direct_property("elements"_utf16_fly_string, tags, JS::default_attributes);
    return object;
}

double Internals::style_engine_match_document()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto& document = window().associated_document();
    auto* root = document.document_element();
    if (!root || root->style_node_id() == 0)
        return -1;
    auto matches = CSS::StyleEngineFFI::style_engine_match_document(document.style_computer().style_engine().host(), read, root->style_node_id());
    if (matches == NumericLimits<size_t>::max())
        return -1;
    return static_cast<double>(matches);
}

// Names every rule identity StyleEngine was given, so a disagreement can be reported as the selector
// that caused it rather than as a number.
static void collect_style_engine_rule_names(Layout::BegunRead const& read, CSS::StyleComputer const& style_computer, CSS::CSSRule const& rule, HashMap<CSS::StyleEngineRuleID, Utf16String>& names, HashMap<CSS::StyleEngineRuleID, Utf16String>& places, Utf16String const& place)
{
    if (auto rule_id = style_computer.style_engine_rule_id_for(read, rule.native_rule()); rule_id != 0) {
        places.set(rule_id, place);
        if (auto const* style_rule = as_if<CSS::CSSStyleRule>(rule)) {
            names.set(rule_id, style_rule->selector_text());
        } else if (auto const* nested = as_if<CSS::CSSNestedDeclarations>(rule)) {
            names.set(rule_id, Utf16String::formatted("{} (nested declarations)", nested->parent_style_rule().selector_text()));
        } else {
            names.set(rule_id, Utf16String::formatted("<rule {}>", rule_id.value()));
        }
    }
    if (auto const* group = as_if<CSS::CSSGroupingRule>(rule)) {
        for (size_t index = 0; index < group->css_rules().length(); ++index)
            collect_style_engine_rule_names(read, style_computer, *group->css_rules().item(index), names, places, place);
    }
    // An imported sheet's rules are the importing sheet's rules as far as the cascade is concerned,
    // and they are not in any style sheet list of their own, so nothing else would reach them.
    if (auto const* import_rule = as_if<CSS::CSSImportRule>(rule)) {
        if (auto const* imported = import_rule->loaded_style_sheet()) {
            for (size_t index = 0; index < imported->css_rules()->length(); ++index)
                collect_style_engine_rule_names(read, style_computer, *imported->css_rules()->item(index), names, places, place);
        }
    }
}

// What StyleEngine matches, element by element, in the order the cascade applies it.
//
// This is a snapshot of the engine's own answer rather than a comparison against a second matcher:
// a shape whose matching changes shows up as a diff, and the expectation is readable enough to say
// whether the change was meant. It is the regression guard the agreement report used to be, without
// a second engine to be right or wrong about.
Utf16String Internals::style_engine_matched_rules()
{
    // A script reads the render state, which waits for a frame in flight.
    Layout::ForcedReadScope read { window().associated_document() };
    auto& document = window().associated_document();
    document.update_style();

    auto& style_computer = document.style_computer();
    // The page's own rules are what a test is about. A user-agent rule reaching an element is not
    // this page's business and would bury the answer, so it is named and then left out.
    HashMap<CSS::StyleEngineRuleID, Utf16String> rule_names;
    HashMap<CSS::StyleEngineRuleID, Utf16String> rule_places;
    HashTable<CSS::StyleEngineRuleID> user_agent_rules;
    auto name_sheet = [&](CSS::StyleSheetState& source_sheet, StringView place) {
        auto& sheet = source_sheet.shared_compiled_style_sheet() ? source_sheet.shared_compiled_style_sheet()->contents() : source_sheet;
        HashMap<CSS::StyleEngineRuleID, Utf16String> names;
        for (size_t index = 0; index < sheet.css_rules()->length(); ++index)
            collect_style_engine_rule_names(read, style_computer, *sheet.css_rules()->item(index), names, rule_places, Utf16String::from_utf8(place));
        for (auto const& [rule_id, name] : names) {
            rule_names.set(rule_id, name);
            if (place == "user-agent"sv)
                user_agent_rules.set(rule_id);
        }
    };
    document.style_scope().for_each_stylesheet(CSS::CascadeOrigin::UserAgent, [&](CSS::StyleSheetState& sheet) { name_sheet(sheet, "user-agent"sv); });
    document.style_scope().for_each_stylesheet(CSS::CascadeOrigin::User, [&](CSS::StyleSheetState& sheet) { name_sheet(sheet, "user"sv); });
    document.style_scope().for_each_stylesheet(CSS::CascadeOrigin::Author, [&](CSS::StyleSheetState& sheet) { name_sheet(sheet, "author"sv); });
    for (auto& sheet : document.style_scope().style_sheets())
        name_sheet(*sheet, "author"sv);
    document.for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) {
        shadow_root.style_scope().for_each_stylesheet(CSS::CascadeOrigin::Author, [&](CSS::StyleSheetState& sheet) { name_sheet(sheet, "shadow"sv); });
        for (auto& sheet : shadow_root.style_scope().style_sheets())
            name_sheet(*sheet, "shadow"sv);
    });

    Utf16StringBuilder builder;
    auto describe = [&](DOM::Element const& element) {
        Utf16StringBuilder description;
        description.append(element.local_name());
        if (auto id = element.id(); id.has_value())
            description.appendff("#{}", *id);
        for (auto const& class_name : element.class_names())
            description.appendff(".{}", class_name);
        return description.to_string();
    };

    document.for_each_shadow_including_descendant([&](DOM::Node& node) {
        auto* element = as_if<DOM::Element>(node);
        if (!element || element->style_node_id() == 0)
            return TraversalDecision::Continue;
        Vector<CSS::StyleEngine::RuleMatch> matches;
        if (!style_computer.style_engine().match_element(read, element->style_node_id(), matches, CSS::StyleEngine::MatchPurpose::Exact)) {
            builder.appendff("{}: the engine could not answer\n", describe(*element));
            return TraversalDecision::Continue;
        }
        Utf16StringBuilder line;
        for (auto const& match : matches) {
            auto rule_id = CSS::StyleEngineRuleID { match.rule };
            if (user_agent_rules.contains(rule_id))
                continue;
            auto name = rule_names.get(rule_id).value_or(Utf16String::formatted("<rule {}>", match.rule));
            line.appendff(" {}", name);
            if (match.pseudo_element != NumericLimits<u32>::max())
                line.appendff("[pseudo {}]", match.pseudo_element);
        }
        if (line.is_empty())
            return TraversalDecision::Continue;
        builder.append(describe(*element));
        builder.append(":"sv);
        builder.append(line.to_string());
        builder.append("\n"sv);
        return TraversalDecision::Continue;
    });
    return builder.to_string();
}

void Internals::reset_style_ffi_counters()
{
    CSS::g_style_ffi_counters_enabled = true;
    CSS::StyleValueFFI::rust_style_ffi_counters_reset();
}

GC::Ref<JS::Object> Internals::style_group_sharing_info(DOM::Element& element)
{
    auto& realm = HTML::relevant_realm(window());
    auto object = JS::Object::create(realm, nullptr);
    auto computed_values = element.computed_style();
    if (!computed_values)
        return object;
    auto parent = element.parent_element();
    auto parent_values = parent ? parent->computed_style() : CSS::ComputedStyleRecordView {};
    size_t group_index = 0;
    computed_values->for_each_style_group_sharing_state(parent_values ? &*parent_values : nullptr, [&](StringView name, bool shared_with_parent, bool is_default) {
        auto group = JS::Object::create(realm, nullptr);
        group->define_direct_property("sharedWithParent"_utf16_fly_string, JS::Value(shared_with_parent), JS::default_attributes);
        group->define_direct_property("isDefault"_utf16_fly_string, JS::Value(is_default), JS::default_attributes);
        auto* payload = computed_values->style_group_payload(static_cast<CSS::StyleGroupIndex>(group_index++));
        group->define_direct_property("payloadIdentity"_utf16_fly_string, JS::PrimitiveString::create(vm(), Utf16String::from_utf8(MUST(String::formatted("{:p}", payload)))), JS::default_attributes);
        object->define_direct_property(Utf16FlyString::from_utf8(name), group, JS::default_attributes);
    });
    return object;
}

GC::Ref<JS::Object> Internals::computed_values_stats()
{
    auto& realm = HTML::relevant_realm(window());
    auto const& statistics = CSS::ComputedValues::statistics();
    auto object = JS::Object::create(realm, nullptr);
    object->define_direct_property("liveComputedValues"_utf16_fly_string, JS::Value(statistics.live_instance_count), JS::default_attributes);
    object->define_direct_property("totalComputedValuesCreated"_utf16_fly_string, JS::Value(statistics.total_instances_created), JS::default_attributes);
    return object;
}

void Internals::update_style()
{
    window().associated_document().update_style();
}

void Internals::set_user_style(Utf16String const& source)
{
    page().set_user_style(source);
}

void Internals::set_preferred_color_scheme(Utf16String const& color_scheme)
{
    auto preferred_color_scheme = CSS::preferred_color_scheme_from_string(color_scheme.utf16_view());

    Optional<CSS::PreferredColorScheme> preferred_color_scheme_override;
    if (preferred_color_scheme != CSS::PreferredColorScheme::Auto)
        preferred_color_scheme_override = preferred_color_scheme;
    page().set_preferred_color_scheme_override_for_testing(preferred_color_scheme_override);
    page().invalidate_style_for_preference_change();
}

void Internals::set_page_focus(bool has_focus)
{
    page().client().set_has_focus(has_focus);
    page().client().page_did_request_set_system_focus(has_focus);
}

void Internals::set_system_visibility_state(Utf16String const& state)
{
    auto visibility_state = state == "hidden"sv ? HTML::VisibilityState::Hidden : HTML::VisibilityState::Visible;
    page().client().page_did_request_set_system_visibility_state(visibility_state);
}

void Internals::set_hidden_document_timer_wake_up_interval(double milliseconds)
{
    window().set_hidden_document_timer_wake_up_interval({}, milliseconds);
}

void Internals::set_hidden_document_intensive_timer_throttling(double wake_up_interval, double grace_period_once_loaded, double grace_period_while_loading)
{
    window().set_hidden_document_intensive_timer_throttling({}, wake_up_interval, grace_period_once_loaded, grace_period_while_loading);
}

WebIDL::UnsignedLongLong Internals::active_timer_count(JS::Object& object)
{
    if (auto* window_proxy = HTML::WindowProxy::from_object(object)) {
        if (auto window = window_proxy->window())
            return window->active_timer_count({});
        return 0;
    }
    if (auto* window = HTML::window_from_global_object(object))
        return window->active_timer_count({});
    return 0;
}

WebIDL::UnsignedLongLong Internals::task_queue_length()
{
    return HTML::main_thread_event_loop().task_queue().size_slow();
}

Utf16String Internals::canvas_color_scheme()
{
    auto& document = window().associated_document();
    Layout::ForcedReadScope read { document };
    document.update_layout(DOM::UpdateLayoutReason::Debugging);
    return CSS::preferred_color_scheme_to_utf16_fly_string(document.canvas_color_scheme(read)).to_utf16_string();
}

u64 Internals::image_request_count(DOM::Document const& document) const
{
    return document.shared_resource_requests().size();
}

WebIDL::ExceptionOr<GC::Ref<JS::Object>> Internals::image_animation_state_for_url(Utf16String const& url)
{
    auto& document = window().associated_document();
    auto parsed_url = document.encoding_parse_url(url);
    auto url_utf8 = TRY_OR_THROW_OOM(vm(), url.utf16_view().to_utf8());
    if (!parsed_url.has_value())
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = Utf16String::formatted("Invalid URL: '{}'", url_utf8) };

    auto& shared_resource_requests = document.shared_resource_requests();
    auto it = find_if(shared_resource_requests.begin(), shared_resource_requests.end(), [&](auto const& entry) { return entry.key.url == *parsed_url; });
    if (it == shared_resource_requests.end())
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = Utf16String::formatted("URL doesn't have any associated shared resource requests: '{}'", url_utf8) };

    auto image_data = it->value->image_data();

    if (!image_data)
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = Utf16String::formatted("URL's shared resource request doesn't have any associated image data: '{}'", url_utf8) };

    auto const* animated_bitmap_data = as_if<HTML::AnimatedBitmapDecodedImageData>(*image_data);

    if (!animated_bitmap_data)
        return WebIDL::SimpleException { .type = WebIDL::SimpleExceptionType::TypeError, .message = Utf16String::formatted("URL's associated image is not an animated bitmap: '{}'", url_utf8) };

    auto object = JS::Object::create(window().principal_realm(), nullptr);

    object->define_direct_property("timerActive"_utf16_fly_string, JS::Value(animated_bitmap_data->m_animation_timer->is_active()), JS::default_attributes);
    object->define_direct_property("sessionID"_utf16_fly_string, JS::Value(static_cast<double>(animated_bitmap_data->m_session_id)), JS::default_attributes);
    object->define_direct_property("frameIndex"_utf16_fly_string, JS::Value(animated_bitmap_data->m_current_frame_index), JS::default_attributes);
    object->define_direct_property("frameCount"_utf16_fly_string, JS::Value(animated_bitmap_data->m_frame_count), JS::default_attributes);
    object->define_direct_property("loopsCompleted"_utf16_fly_string, JS::Value(animated_bitmap_data->m_loops_completed), JS::default_attributes);
    object->define_direct_property("loopCount"_utf16_fly_string, JS::Value(animated_bitmap_data->m_loop_count), JS::default_attributes);
    object->define_direct_property("clientCount"_utf16_fly_string, JS::Value(image_data->m_clients.size()), JS::default_attributes);

    return object;
}

String Internals::viewport_overflow_x()
{
    auto& document = window().associated_document();
    Layout::ForcedReadScope read { document };
    document.update_layout(DOM::UpdateLayoutReason::Debugging);
    auto overflow = document.layout_node(read)->overflow_x();
    switch (overflow) {
    case CSS::Overflow::Auto:
        return "auto"_string;
    case CSS::Overflow::Clip:
        return "clip"_string;
    case CSS::Overflow::Hidden:
        return "hidden"_string;
    case CSS::Overflow::Scroll:
        return "scroll"_string;
    case CSS::Overflow::Visible:
        return "visible"_string;
    }
    VERIFY_NOT_REACHED();
}

namespace {

// The bytes of a stream from create_stream_pulling_from_bytes() that it has not pulled yet.
struct UnpulledBytes final : public RefCounted<UnpulledBytes> {
    explicit UnpulledBytes(ByteBuffer bytes)
        : bytes(move(bytes))
    {
    }

    ByteBuffer bytes;
};

}

GC::Ref<Streams::ReadableStream> Internals::create_stream_pulling_from_bytes(String const& bytes, WebIDL::UnsignedLong pull_size)
{
    auto& realm = window().principal_realm();
    auto stream = Streams::ReadableStream::create(realm);
    auto unpulled = make_ref_counted<UnpulledBytes>(MUST(ByteBuffer::copy(bytes.bytes())));

    auto pull_algorithm = GC::create_function(realm.heap(), [realm = GC::Ref { realm }, stream, unpulled, pull_size] {
        auto& remaining = unpulled->bytes;
        auto size = min(remaining.size(), static_cast<size_t>(pull_size));
        if (auto view_byte_length = stream->current_byob_request_view_byte_length(); view_byte_length.has_value())
            size = min(size, static_cast<size_t>(*view_byte_length));

        if (size > 0) {
            MUST(stream->pull_from_bytes(remaining.bytes().trim(size)));
            remaining = MUST(remaining.slice(size, remaining.size() - size));
        }
        if (remaining.is_empty())
            stream->close();
        return WebIDL::create_resolved_promise(*realm, JS::js_undefined());
    });

    stream->set_up_with_byte_reading_support(realm, pull_algorithm);
    return stream;
}

static GC::Ref<JS::Object> create_hit_testing_result(JS::Realm& realm, Painting::HitTestResult& result)
{
    auto hit_testing_result = JS::Object::create(realm, nullptr);
    hit_testing_result->define_direct_property("node"_utf16_fly_string, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { *result.dom_node() }), JS::default_attributes);
    hit_testing_result->define_direct_property("indexInNode"_utf16_fly_string, JS::Value(result.index_in_node), JS::default_attributes);
    return hit_testing_result;
}

static Utf16String context_menu_kind_to_string(Page::ContextMenuRequest::Kind kind)
{
    switch (kind) {
    case Page::ContextMenuRequest::Kind::Page:
        return "page"_utf16;
    case Page::ContextMenuRequest::Kind::Image:
        return "image"_utf16;
    case Page::ContextMenuRequest::Kind::Link:
        return "link"_utf16;
    case Page::ContextMenuRequest::Kind::Media:
        return "media"_utf16;
    }
    VERIFY_NOT_REACHED();
}

GC::Ptr<JS::Object> Internals::take_context_menu_request()
{
    auto request = page().take_context_menu_request();
    if (!request.has_value())
        return nullptr;

    auto& realm = HTML::relevant_realm(window());
    auto object = JS::Object::create(realm, nullptr);
    object->define_direct_property("kind"_utf16_fly_string, JS::PrimitiveString::create(vm(), context_menu_kind_to_string(request->kind)), JS::default_attributes);
    auto target = Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, request->target);
    object->define_direct_property("target"_utf16_fly_string, target, JS::default_attributes);
    object->define_direct_property("x"_utf16_fly_string, JS::Value(request->position.x().to_double()), JS::default_attributes);
    object->define_direct_property("y"_utf16_fly_string, JS::Value(request->position.y().to_double()), JS::default_attributes);
    if (request->media.has_value()) {
        object->define_direct_property("isPictureInPicture"_utf16_fly_string, JS::Value(request->media->is_picture_in_picture), JS::default_attributes);
        object->define_direct_property("canEnterPictureInPicture"_utf16_fly_string, JS::Value(request->media->can_enter_picture_in_picture), JS::default_attributes);
    }
    return object;
}

// The text node a spellchecking test names: the node itself, or the inner text node of a text control.
static GC::Ptr<DOM::Text> spelling_test_text_node(DOM::Node& node)
{
    if (auto* text = as_if<DOM::Text>(node))
        return text;
    if (auto* text_control = as_if<HTML::FormAssociatedTextControlElement>(node))
        return text_control->form_associated_element_to_text_node();
    return nullptr;
}

WebIDL::ExceptionOr<void> Internals::add_spelling_error_for_testing(GC::Ref<DOM::Node> node, WebIDL::UnsignedLong start, WebIDL::UnsignedLong end)
{
    auto text = spelling_test_text_node(node);
    if (!text || start >= end || end > text->length_in_utf16_code_units())
        return WebIDL::IndexSizeError::create("No text to flag in that range"_utf16);
    text->document().spelling_markers().add(*text, start, end);
    return {};
}

Utf16String Internals::spelling_error_ranges(GC::Ref<DOM::Node> node)
{
    auto text = spelling_test_text_node(node);
    if (!text)
        return {};
    StringBuilder builder;
    for (auto const& marker : text->document().spelling_markers().markers_for(*text)) {
        if (!builder.is_empty())
            builder.append(' ');
        builder.appendff("{}-{}", marker.start, marker.end);
    }
    return Utf16String::from_utf8(builder.string_view());
}

GC::Ref<WebIDL::Promise> Internals::check_spelling_for_testing(Utf16String const& text)
{
    auto& realm = window().principal_realm();
    auto promise = WebIDL::create_promise_for(window());
    auto& page = this->page();
    if (!page.spell_checking_available()) {
        WebIDL::reject_promise(realm, promise, JS::PrimitiveString::create(vm(), "Spellchecking is unavailable"_utf16));
        return promise;
    }
    page.request_spell_check(text, GC::create_function(heap(), [&realm, promise](Vector<SpellingError> errors) {
        HTML::TemporaryExecutionContext execution_context { realm };
        StringBuilder builder;
        for (auto const& error : errors) {
            if (!builder.is_empty())
                builder.append(' ');
            builder.appendff("{}-{}", error.offset, error.offset + error.length);
        }
        WebIDL::resolve_promise(realm, promise, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(builder.string_view())));
    }));
    return promise;
}

GC::Ref<WebIDL::Promise> Internals::spell_check_settled()
{
    auto& realm = window().principal_realm();
    auto promise = WebIDL::create_promise_for(window());
    page().when_spell_checking_settled(GC::create_function(heap(), [&realm, promise] {
        HTML::TemporaryExecutionContext execution_context { realm };
        WebIDL::resolve_promise(promise);
    }));
    return promise;
}

GC::Ref<WebIDL::Promise> Internals::request_writing_suggestion_for_testing(Utf16String const& text, WebIDL::UnsignedLong caret_offset)
{
    auto& realm = window().principal_realm();
    auto promise = WebIDL::create_promise_for(window());
    auto& page = this->page();
    if (!page.writing_suggestions_available()) {
        WebIDL::reject_promise(realm, promise, JS::PrimitiveString::create(vm(), "Writing suggestions are unavailable"_utf16));
        return promise;
    }
    page.request_writing_suggestion(text, caret_offset, GC::create_function(heap(), [&realm, promise](Optional<WritingSuggestion> suggestion) {
        HTML::TemporaryExecutionContext execution_context { realm };
        auto result = suggestion.has_value()
            ? Utf16String::formatted("{}-{}:{}", suggestion->replacement_start, suggestion->replacement_length, suggestion->text)
            : "(none)"_utf16;
        WebIDL::resolve_promise(realm, promise, JS::PrimitiveString::create(realm.vm(), move(result)));
    }));
    return promise;
}

// The writing suggestion showing in any document of the page.
Utf16String Internals::writing_suggestion()
{
    for (auto const& navigable : page().hosted_navigables()) {
        auto document = navigable->active_document();
        if (!document)
            continue;
        if (auto* controller = document->writing_suggestion_controller_if_exists(); controller && controller->showing().has_value())
            return controller->showing()->remainder;
    }
    return "(none)"_utf16;
}

GC::Ref<WebIDL::Promise> Internals::writing_suggestion_settled()
{
    auto& realm = window().principal_realm();
    auto promise = WebIDL::create_promise_for(window());
    page().when_writing_suggestions_settled(GC::create_function(heap(), [&realm, promise] {
        HTML::TemporaryExecutionContext execution_context { realm };
        WebIDL::resolve_promise(promise);
    }));
    return promise;
}

GC::Ref<WebIDL::Promise> Internals::writing_suggestion_request_sent()
{
    auto& realm = window().principal_realm();
    auto promise = WebIDL::create_promise_for(window());
    page().when_writing_suggestion_requested(GC::create_function(heap(), [&realm, promise] {
        HTML::TemporaryExecutionContext execution_context { realm };
        WebIDL::resolve_promise(promise);
    }));
    return promise;
}

GC::Ref<WebIDL::Promise> Internals::activate_spelling_menu_item(Bindings::SpellingMenuItem item, WebIDL::UnsignedLong guess_index)
{
    auto& realm = window().principal_realm();
    auto promise = WebIDL::create_promise_for(window());
    auto action = [&] {
        switch (item) {
        case Bindings::SpellingMenuItem::None:
            return SpellingMenuActionForTesting::None;
        case Bindings::SpellingMenuItem::Guess:
            return SpellingMenuActionForTesting::Guess;
        case Bindings::SpellingMenuItem::Ignore:
            return SpellingMenuActionForTesting::IgnoreSpelling;
        case Bindings::SpellingMenuItem::Learn:
            return SpellingMenuActionForTesting::LearnSpelling;
        }
        VERIFY_NOT_REACHED();
    }();
    page().request_spelling_menu_action_for_testing(action, guess_index, GC::create_function(heap(), [&realm, promise](String menu) {
        HTML::TemporaryExecutionContext execution_context { realm };
        WebIDL::resolve_promise(realm, promise, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(menu)));
    }));
    return promise;
}

void Internals::toggle_media_context_menu_picture_in_picture()
{
    page().toggle_media_picture_in_picture_state();
}

GC::Ptr<JS::Object> Internals::hit_test_result(double x, double y)
{
    auto& realm = HTML::relevant_realm(window());
    auto result = hit_test(x, y);
    if (result.has_value())
        return create_hit_testing_result(realm, *result);
    return nullptr;
}

GC::Ref<JS::Object> Internals::style_invalidation_counters_object() const
{
    Layout::ForcedReadScope read { window().associated_document() };
    auto& realm = HTML::relevant_realm(window());
    auto const& counters = style_invalidation_counters();
    auto& document = window().associated_document();
    auto object = JS::Object::create(realm, nullptr);
    object->define_direct_property("styleEnvironmentVersion"_utf16_fly_string, JS::Value(window().associated_document().style_environment_version()), JS::default_attributes);
    object->define_direct_property("styleEngineReactionBatchRuns"_utf16_fly_string, JS::Value(counters.style_engine_reaction_batch_runs), JS::default_attributes);
    object->define_direct_property("styleEngineReactionElements"_utf16_fly_string, JS::Value(counters.style_engine_reaction_elements), JS::default_attributes);
    object->define_direct_property("styleEnginePublishedReactions"_utf16_fly_string, JS::Value(counters.style_engine_published_reactions), JS::default_attributes);
    object->define_direct_property("styleEngineRecordDeltasApplied"_utf16_fly_string, JS::Value(counters.style_engine_record_deltas_applied), JS::default_attributes);
    object->define_direct_property("layoutStyleRecordPublications"_utf16_fly_string, JS::Value(counters.layout_style_record_publications), JS::default_attributes);
    object->define_direct_property("styleEngineMaterializedGaps"_utf16_fly_string, JS::Value(counters.style_engine_materialized_gaps), JS::default_attributes);
    object->define_direct_property("elementStyleRecomputations"_utf16_fly_string, JS::Value(counters.element_style_recomputations), JS::default_attributes);
    object->define_direct_property("elementStyleNoopRecomputations"_utf16_fly_string, JS::Value(counters.element_style_noop_recomputations), JS::default_attributes);
    object->define_direct_property("styleRecordPropertyDamageCacheHits"_utf16_fly_string, JS::Value(counters.style_record_property_damage_cache_hits), JS::default_attributes);
    object->define_direct_property("elementComputedStyleChanges"_utf16_fly_string, JS::Value(counters.element_computed_style_changes), JS::default_attributes);
    object->define_direct_property("elementStyleSharedComputations"_utf16_fly_string, JS::Value(counters.element_style_shared_computations), JS::default_attributes);
    object->define_direct_property("elementStyleInputChangedByParentStyle"_utf16_fly_string, JS::Value(counters.element_style_input_changed_by_parent_style), JS::default_attributes);
    object->define_direct_property("elementStyleInputChangedByParentCustomProperties"_utf16_fly_string, JS::Value(counters.element_style_input_changed_by_parent_custom_properties), JS::default_attributes);
    object->define_direct_property("elementStyleInputReused"_utf16_fly_string, JS::Value(counters.element_style_input_reused), JS::default_attributes);
    object->define_direct_property("engineComputedStyleRecords"_utf16_fly_string, JS::Value(counters.engine_computed_style_records), JS::default_attributes);
    object->define_direct_property("animatedStyleReconstructionFallbacks"_utf16_fly_string, JS::Value(counters.animated_style_reconstruction_fallbacks), JS::default_attributes);
    object->define_direct_property("animatedStyleOverlayBuilds"_utf16_fly_string, JS::Value(counters.animated_style_overlay_builds), JS::default_attributes);
    object->define_direct_property("animatedStyleFullBuilds"_utf16_fly_string, JS::Value(counters.animated_style_full_builds), JS::default_attributes);
    object->define_direct_property("animationFramePumpRequests"_utf16_fly_string, JS::Value(counters.animation_frame_pump_requests), JS::default_attributes);
    object->define_direct_property("animationStyleSkipCacheHits"_utf16_fly_string, JS::Value(counters.animation_style_skip_cache_hits), JS::default_attributes);
    object->define_direct_property("animationStyleSkipCacheMisses"_utf16_fly_string, JS::Value(counters.animation_style_skip_cache_misses), JS::default_attributes);
    object->define_direct_property("animationTimelineSynchronizations"_utf16_fly_string, JS::Value(counters.animation_timeline_synchronizations), JS::default_attributes);
    object->define_direct_property("animationTimelineAssociatedAnimationUpdates"_utf16_fly_string, JS::Value(counters.animation_timeline_associated_animation_updates), JS::default_attributes);
    object->define_direct_property("associatedAnimations"_utf16_fly_string, JS::Value(document.associated_animation_count()), JS::default_attributes);
    object->define_direct_property("compositorVisualAnimationUpdates"_utf16_fly_string, JS::Value(counters.compositor_visual_animation_updates), JS::default_attributes);
    object->define_direct_property("compositorVisualAnimationTimingAnchorUpdates"_utf16_fly_string, JS::Value(counters.compositor_visual_animation_timing_anchor_updates), JS::default_attributes);
    object->define_direct_property("compositorKeyframeValueResolutions"_utf16_fly_string, JS::Value(counters.compositor_keyframe_value_resolutions), JS::default_attributes);
    size_t compositor_visual_animation_count = 0;
    Optional<double> compositor_visual_animation_local_time_at_anchor;
    bool compositor_visual_animations_share_timing_anchor = true;
    bool compositor_visual_animation_targets_are_valid = true;
    if (document.has_committed_viewport_box() && document.paint_state().has_visual_context_tree(read)) {
        auto visual_context_tree = document.paint_state().visual_context_tree(document);
        auto summary = visual_context_tree.visual_animation_summary();
        compositor_visual_animation_count = summary.count;
        if (summary.count > 0)
            compositor_visual_animation_local_time_at_anchor = summary.local_time_at_anchor_ms_of_first;
        compositor_visual_animations_share_timing_anchor = summary.share_timing_anchor;
        compositor_visual_animation_targets_are_valid = summary.targets_are_valid;
    }
    object->define_direct_property("compositorVisualAnimations"_utf16_fly_string, JS::Value(compositor_visual_animation_count), JS::default_attributes);
    object->define_direct_property("compositorVisualAnimationLocalTimeAtAnchor"_utf16_fly_string, compositor_visual_animation_local_time_at_anchor.has_value() ? JS::Value(*compositor_visual_animation_local_time_at_anchor) : JS::js_null(), JS::default_attributes);
    object->define_direct_property("compositorVisualAnimationsShareTimingAnchor"_utf16_fly_string, JS::Value(compositor_visual_animations_share_timing_anchor), JS::default_attributes);
    object->define_direct_property("compositorVisualAnimationTargetsAreValid"_utf16_fly_string, JS::Value(compositor_visual_animation_targets_are_valid), JS::default_attributes);
    auto document_timeline_current_time = document.timeline()->current_time();
    object->define_direct_property("documentTimelineCurrentTime"_utf16_fly_string, document_timeline_current_time.has_value() ? JS::Value(document_timeline_current_time->value) : JS::js_null(), JS::default_attributes);
    object->define_direct_property("compositorAnimationWakeupTimerActive"_utf16_fly_string, JS::Value(document.compositor_animation_wakeup_timer_is_active()), JS::default_attributes);
    object->define_direct_property("compositorAnimationObservationTimerActive"_utf16_fly_string, JS::Value(document.compositor_animation_observation_timer_is_active()), JS::default_attributes);
    object->define_direct_property("baseStylePartialBuilds"_utf16_fly_string, JS::Value(counters.base_style_partial_builds), JS::default_attributes);
    object->define_direct_property("baseStyleFullBuilds"_utf16_fly_string, JS::Value(counters.base_style_full_builds), JS::default_attributes);
    object->define_direct_property("computedLonghandEvaluations"_utf16_fly_string, JS::Value(counters.computed_longhand_evaluations), JS::default_attributes);
    object->define_direct_property("computedLonghandDrivesStarted"_utf16_fly_string, JS::Value(counters.computed_longhand_drives_started), JS::default_attributes);
    object->define_direct_property("longhandWrappersMinted"_utf16_fly_string, JS::Value(CSS::longhand_wrappers_minted()), JS::default_attributes);
    object->define_direct_property("styleStabilizationEpochs"_utf16_fly_string, JS::Value(counters.style_stabilization_epochs), JS::default_attributes);
    object->define_direct_property("styleStabilizationFeedbackEpochs"_utf16_fly_string, JS::Value(counters.style_stabilization_feedback_epochs), JS::default_attributes);
    object->define_direct_property("provisionalStylePasses"_utf16_fly_string, JS::Value(counters.provisional_style_passes), JS::default_attributes);
    object->define_direct_property("styleStabilizationRoundGuardHits"_utf16_fly_string, JS::Value(counters.style_stabilization_round_guard_hits), JS::default_attributes);
    object->define_direct_property("styleUpdatePassGuardHits"_utf16_fly_string, JS::Value(counters.style_update_pass_guard_hits), JS::default_attributes);
    object->define_direct_property("exactStabilizationPasses"_utf16_fly_string, JS::Value(counters.exact_stabilization_passes), JS::default_attributes);
    object->define_direct_property("styleStabilizationBoundFailures"_utf16_fly_string, JS::Value(counters.style_stabilization_bound_failures), JS::default_attributes);
    object->define_direct_property("provisionalAnimationEvents"_utf16_fly_string, JS::Value(counters.provisional_animation_events), JS::default_attributes);
    object->define_direct_property("committedAnimationEvents"_utf16_fly_string, JS::Value(counters.committed_animation_events), JS::default_attributes);
    object->define_direct_property("provisionalTransitionDecisions"_utf16_fly_string, JS::Value(counters.provisional_transition_decisions), JS::default_attributes);
    object->define_direct_property("supersededProvisionalTransitionDecisions"_utf16_fly_string, JS::Value(counters.superseded_provisional_transition_decisions), JS::default_attributes);
    object->define_direct_property("committedTransitionActions"_utf16_fly_string, JS::Value(counters.committed_transition_actions), JS::default_attributes);
    object->define_direct_property("committedTransitionsStarted"_utf16_fly_string, JS::Value(counters.committed_transitions_started), JS::default_attributes);
    object->define_direct_property("mediaRuleEvaluations"_utf16_fly_string, JS::Value(counters.media_rule_evaluations), JS::default_attributes);
    object->define_direct_property("registeredPropertiesCacheRebuilds"_utf16_fly_string, JS::Value(counters.registered_properties_cache_rebuilds), JS::default_attributes);
    object->define_direct_property("styleQueryContainerScans"_utf16_fly_string, JS::Value(counters.style_query_container_scans), JS::default_attributes);
    object->define_direct_property("sizeQueryContainerScanVisits"_utf16_fly_string, JS::Value(CSS::StyleEngineFFI::style_engine_size_query_container_scan_visits(document.style_computer().style_engine().host(), read, false)), JS::default_attributes);
    object->define_direct_property("styleEngineTransactionSetups"_utf16_fly_string, JS::Value(counters.style_engine_transaction_setups), JS::default_attributes);
    object->define_direct_property("styleEngineTransactionSetupMicroseconds"_utf16_fly_string, JS::Value(counters.style_engine_transaction_setup_microseconds), JS::default_attributes);
    object->define_direct_property("styleEnginePlanningMicroseconds"_utf16_fly_string, JS::Value(counters.style_update_bridge_microseconds), JS::default_attributes);
    object->define_direct_property("relayoutsPerformed"_utf16_fly_string, JS::Value(counters.relayouts_performed), JS::default_attributes);
    object->define_direct_property("styleUpdateMicroseconds"_utf16_fly_string, JS::Value(counters.style_update_microseconds), JS::default_attributes);
    object->define_direct_property("styleUpdateSubmissionMicroseconds"_utf16_fly_string, JS::Value(counters.style_update_submission_microseconds), JS::default_attributes);
    object->define_direct_property("styleUpdateBridgeMicroseconds"_utf16_fly_string, JS::Value(counters.style_update_bridge_microseconds), JS::default_attributes);
    object->define_direct_property("styleUpdateApplyMicroseconds"_utf16_fly_string, JS::Value(counters.style_update_apply_microseconds), JS::default_attributes);
    object->define_direct_property("styleUpdateRemainderMicroseconds"_utf16_fly_string, JS::Value(counters.style_update_remainder_microseconds), JS::default_attributes);
    object->define_direct_property("customPropertyResolutions"_utf16_fly_string, JS::Value(counters.custom_property_resolutions), JS::default_attributes);
    object->define_direct_property("customPropertyElements"_utf16_fly_string, JS::Value(counters.custom_property_elements), JS::default_attributes);
    object->define_direct_property("customPropertyValueComputations"_utf16_fly_string, JS::Value(counters.custom_property_value_computations), JS::default_attributes);
    object->define_direct_property("customPropertyOverlayHits"_utf16_fly_string, JS::Value(counters.custom_property_overlay_hits), JS::default_attributes);
    object->define_direct_property("customPropertyCycleParticipants"_utf16_fly_string, JS::Value(counters.custom_property_cycle_participants), JS::default_attributes);
    object->define_direct_property("styleCascadeMicroseconds"_utf16_fly_string, JS::Value(counters.style_cascade_microseconds), JS::default_attributes);
    object->define_direct_property("styleValuesMicroseconds"_utf16_fly_string, JS::Value(counters.style_values_microseconds), JS::default_attributes);
    object->define_direct_property("scrollableOverflowRecalculations"_utf16_fly_string, JS::Value(Layout::RustFFI::render_state_scrollable_overflow_recalculation_count(document.layout_node_arena().host(), false)), JS::default_attributes);
    object->define_direct_property("overflowContainedBoxIndexRefills"_utf16_fly_string, JS::Value(Layout::RustFFI::render_state_overflow_contained_box_index_refill_count(document.layout_node_arena().host(), false)), JS::default_attributes);
    return object;
}

GC::Ref<JS::Object> Internals::recorded_async_scrolling_state_object()
{
    auto* display_list = window().associated_document().paint_state().display_list_used_as_paint_command_cache_source();
    auto state = display_list ? Compositing::async_scrolling_state_from_display_list(*display_list) : Compositing::AsyncScrollingState {};
    return scrolling_state_report_to_object(window(), Compositing::async_scrolling_state_to_json(state));
}

}
