/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/SourceLocation.h>
#include <LibGC/Heap.h>
#include <LibGC/HeapVector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Bindings/CSS.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/Clipboard/SystemClipboard.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/HistoryExecutor.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/RemoteNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintFacts.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Selection/Selection.h>

namespace Web {

GC_DEFINE_ALLOCATOR(Page);

GC::Ref<Page> Page::create(GC::Ref<PageClient> page_client)
{
    return GC::Heap::the().allocate<Page>(page_client);
}

Page::Page(GC::Ref<PageClient> client)
    : m_client(client)
    , m_history_executor(GC::Heap::the().allocate<HTML::HistoryExecutor>(*this))
{
}

Page::~Page() = default;

void Page::acquire_screen_wake_lock()
{
    if (m_active_screen_wake_lock_count++ == 0)
        client().page_did_change_screen_wake_lock_state(ScreenWakeLockState::Acquired);
}

void Page::release_screen_wake_lock()
{
    VERIFY(m_active_screen_wake_lock_count > 0);
    if (--m_active_screen_wake_lock_count == 0)
        client().page_did_change_screen_wake_lock_state(ScreenWakeLockState::Released);
}

bool Page::has_compositor_host() const
{
    return m_client->compositor_host();
}

void Page::ensure_compositor_host()
{
    if (!m_client->supports_compositor())
        return;

    m_client->ensure_compositor_host();
}

Compositor::CompositorHost& Page::compositor_host()
{
    auto* compositor_host = m_client->compositor_host();
    VERIFY(compositor_host);
    return *compositor_host;
}

Compositor::CompositorHost const& Page::compositor_host() const
{
    auto const* compositor_host = m_client->compositor_host();
    VERIFY(compositor_host);
    return *compositor_host;
}

void Page::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_context_menu_request.has_value())
        visitor.visit(m_context_menu_request->target);
    visitor.visit(m_top_level_traversable);
    visitor.visit(m_browsing_context_group);
    visitor.visit(m_history_executor);
    visitor.visit(m_client);
    visitor.visit(m_window_rect_observer);
    visitor.visit(m_on_pending_dialog_closed);
    visitor.visit(m_pending_clipboard_requests);
    for (auto const& request : m_pending_geolocation_requests)
        visitor.visit(request.value.callback);
    m_pending_fullscreen_operations.for_each([&](auto const& operation) {
        operation.visit([&](PendingFullscreenEnter const& enter_operation) {
                visitor.visit(enter_operation.element);
                visitor.visit(enter_operation.pending_doc);
                visitor.visit(enter_operation.promise); },
            [&](PendingFullscreenExit const& exit_operation) {
                visitor.visit(exit_operation.doc);
                visitor.visit(exit_operation.promise);
            });
    });
}

HTML::LocalNavigable& Page::focused_navigable()
{
    if (m_focused_navigable)
        return *m_focused_navigable;
    if (has_local_traversable())
        return local_traversable();
    return *local_roots().first();
}

void Page::set_focused_navigable(HTML::LocalNavigable& navigable)
{
    if (m_focused_navigable == &navigable)
        return;
    invalidate_compositor_keyboard_scroll_state();
    m_focused_navigable = navigable;
    if (has_local_traversable())
        local_traversable()->set_needs_repaint();
}

void Page::navigable_document_destroyed(Badge<DOM::Document>, HTML::LocalNavigable& navigable)
{
    if (GC::Ref { navigable } == m_focused_navigable.ptr())
        m_focused_navigable = nullptr;
    if (GC::Ref { navigable } == m_mouse_event_tracking_navigable.ptr())
        m_mouse_event_tracking_navigable = nullptr;
}

void Page::load(URL::URL const& url, Bindings::NavigationHistoryBehavior history_handling, Utf16String navigation_id)
{
    (void)local_traversable()->navigate({ .url = url, .history_handling = history_handling, .user_involvement = HTML::UserNavigationInvolvement::BrowserUI, .navigation_id = move(navigation_id) });
}

void Page::load_html(StringView html, Utf16String navigation_id)
{
    // FIXME: #23909 Figure out why GC threshold does not stay low when repeatedly loading html from the WebView
    heap().collect_garbage();

    (void)local_traversable()->navigate({ .url = URL::about_srcdoc(),
        .document_resource = Utf16String::from_utf8(html),
        .user_involvement = HTML::UserNavigationInvolvement::BrowserUI,
        .navigation_id = move(navigation_id) });
}

void Page::reload()
{
    local_traversable()->reload();
}

void Page::queue_screenshot_task(Optional<UniqueNodeID> node_id)
{
    m_screenshot_tasks.enqueue({ node_id });
    local_traversable()->set_needs_repaint();
    client().request_frame();
}

void Page::process_screenshot_requests()
{
    // Screenshots are of the tab as the view displays it.
    if (m_screenshot_tasks.is_empty())
        return;
    auto& client = this->client();
    auto navigable = local_traversable();
    while (!m_screenshot_tasks.is_empty()) {
        auto task = m_screenshot_tasks.dequeue();
        if (task.node_id.has_value()) {
            auto* dom_node = DOM::Node::from_unique_id(*task.node_id);
            if (dom_node)
                dom_node->document().update_layout(DOM::UpdateLayoutReason::ProcessScreenshot);
            auto const* layout_node = dom_node ? dom_node->layout_node() : nullptr;
            if (!layout_node || !Painting::has_committed_box(*layout_node)) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto rect = enclosing_device_rect(Painting::absolute_border_box_rect(*layout_node));
            auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, rect.size().to_type<int>());
            if (bitmap_or_error.is_error()) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto bitmap = bitmap_or_error.release_value();
            auto painting_surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
            HTML::PaintConfig paint_config { .canvas_fill_rect = rect.to_type<int>() };
            navigable->render_screenshot(painting_surface, paint_config, [bitmap, &client] {
                client.page_did_take_screenshot(bitmap->to_shareable_bitmap());
            });
        } else {
            navigable->active_document()->update_layout(DOM::UpdateLayoutReason::ProcessScreenshot);
            auto const* layout_node = navigable->active_document()->layout_node();
            VERIFY(layout_node && Painting::has_committed_box(*layout_node));
            auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(*layout_node);
            auto rect = enclosing_device_rect(scrollable_overflow_rect.value());
            auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, rect.size().to_type<int>());
            if (bitmap_or_error.is_error()) {
                client.page_did_take_screenshot({});
                continue;
            }
            auto bitmap = bitmap_or_error.release_value();
            auto painting_surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
            HTML::PaintConfig paint_config { .paint_overlay = true, .canvas_fill_rect = rect.to_type<int>() };
            navigable->render_screenshot(painting_surface, paint_config, [bitmap, &client] {
                client.page_did_take_screenshot(bitmap->to_shareable_bitmap());
            });
        }
    }
}

Gfx::Palette Page::palette() const
{
    return m_client->palette();
}

// https://drafts.csswg.org/cssom-view-1/#web-exposed-screen-area
CSSPixelRect Page::web_exposed_screen_area() const
{
    // FIXME: 1. Let target be this’s relevant global object’s browsing context.
    // FIXME: 2. Let emulated screen area be the WebDriver BiDi emulated total screen area of target.
    // FIXME: 3. If emulated screen area is not null, return emulated screen area.

    // 4. Otherwise, return one of the following:
    //    - The area of the output device, in CSS pixels.
    //    - The area of the viewport, in CSS pixels.
    // NB: This is the area of the output device, but in device pixels.
    // See: https://github.com/LadybirdBrowser/ladybird/pull/4084
    auto device_pixel_rect = m_client->screen_rect();
    return {
        device_pixel_rect.x().value(),
        device_pixel_rect.y().value(),
        device_pixel_rect.width().value(),
        device_pixel_rect.height().value()
    };
}

// https://drafts.csswg.org/cssom-view-1/#web-exposed-available-screen-area
CSSPixelRect Page::web_exposed_available_screen_area() const
{
    // FIXME: 1. Let target be this’s relevant global object’s browsing context.
    // FIXME: 2. Let emulated screen area be the WebDriver BiDi emulated total screen area of target.
    // FIXME: 3. If emulated screen area is not null, return emulated screen area.

    // 4. Otherwise, return one of the following:
    //    - The available area of the rendering surface of the output device, in CSS pixels.
    //    - The area of the output device, in CSS pixels.
    //    - The area of the viewport, in CSS pixels.
    // NB: This is the area of the output device, but in device pixels. See note in web_exposed_screen_area()
    auto device_pixel_rect = m_client->screen_rect();
    return {
        device_pixel_rect.x().value(),
        device_pixel_rect.y().value(),
        device_pixel_rect.width().value(),
        device_pixel_rect.height().value()
    };
}

CSS::PreferredColorScheme Page::preferred_color_scheme() const
{
    if (m_preferred_color_scheme_override_for_testing.has_value())
        return *m_preferred_color_scheme_override_for_testing;

    // Force-dark presents a dark preference, the way Android WebView's force-dark does (Chrome itself leaves the
    // preference alone and darkens per element): a page that can style itself dark does, the color-scheme opt-out keeps
    // the filter away from it, and only pages with no dark support get filtered.
    if (has_local_traversable() && local_traversable()->force_dark_enabled())
        return CSS::PreferredColorScheme::Dark;

    auto preferred_color_scheme = m_client->preferred_color_scheme();

    if (preferred_color_scheme == CSS::PreferredColorScheme::Auto)
        preferred_color_scheme = palette().is_dark() ? CSS::PreferredColorScheme::Dark : CSS::PreferredColorScheme::Light;

    return preferred_color_scheme;
}

CSS::PreferredContrast Page::preferred_contrast() const
{
    return m_client->preferred_contrast();
}

CSS::PreferredMotion Page::preferred_motion() const
{
    return m_client->preferred_motion();
}

CSSPixelPoint Page::device_to_css_point(DevicePixelPoint point) const
{
    return {
        point.x().value() / client().device_pixels_per_css_pixel(),
        point.y().value() / client().device_pixels_per_css_pixel(),
    };
}

DevicePixelPoint Page::css_to_device_point(CSSPixelPoint point) const
{
    return {
        point.x() * client().device_pixels_per_css_pixel(),
        point.y() * client().device_pixels_per_css_pixel(),
    };
}

DevicePixelRect Page::css_to_device_rect(CSSPixelRect rect) const
{
    return {
        rect.location().to_type<double>() * client().device_pixels_per_css_pixel(),
        rect.size().to_type<double>() * client().device_pixels_per_css_pixel(),
    };
}

CSSPixelRect Page::device_to_css_rect(DevicePixelRect rect) const
{
    auto scale = client().device_pixels_per_css_pixel();
    return {
        CSSPixels::nearest_value_for(rect.x().value() / scale),
        CSSPixels::nearest_value_for(rect.y().value() / scale),
        CSSPixels::floored_value_for(rect.width().value() / scale),
        CSSPixels::floored_value_for(rect.height().value() / scale),
    };
}

CSSPixelSize Page::device_to_css_size(DevicePixelSize size) const
{
    auto scale = client().device_pixels_per_css_pixel();
    return {
        CSSPixels::floored_value_for(size.width().value() / scale),
        CSSPixels::floored_value_for(size.height().value() / scale),
    };
}

DevicePixelRect Page::enclosing_device_rect(CSSPixelRect rect) const
{
    auto scale = client().device_pixels_per_css_pixel();
    return DevicePixelRect(
        floor(rect.x().to_double() * scale),
        floor(rect.y().to_double() * scale),
        ceil(rect.width().to_double() * scale),
        ceil(rect.height().to_double() * scale));
}

DevicePixelRect Page::rounded_device_rect(CSSPixelRect rect) const
{
    auto scale = client().device_pixels_per_css_pixel();
    return {
        roundf(rect.x().to_double() * scale),
        roundf(rect.y().to_double() * scale),
        roundf(rect.width().to_double() * scale),
        roundf(rect.height().to_double() * scale)
    };
}

ChromeMetrics Page::chrome_metrics() const
{
    return ChromeMetrics { m_client->zoom_level() };
}

EventResult Page::handle_mouseup(HTML::LocalNavigable& root, DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers)
{
    // INTEROP: Releasing outside an iframe still ends selection and drag tracking in the child document where the
    //          interaction began, while the mouseup event itself remains targeted at the document under the pointer.
    ScopeGuard reset_mouse_input_tracking = [&] {
        if (auto navigable = m_mouse_event_tracking_navigable) {
            m_mouse_event_tracking_navigable = nullptr;
            navigable->event_handler().reset_mouse_input_tracking({});
        }
    };
    return root.event_handler().handle_mouseup(device_to_css_point(position), device_to_css_point(screen_position), button, buttons, modifiers);
}

EventResult Page::handle_mouseup(DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers)
{
    return handle_mouseup(local_traversable(), position, screen_position, button, buttons, modifiers);
}

EventResult Page::handle_mousedown(HTML::LocalNavigable& root, DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int click_count)
{
    if (button == UIEvents::MouseButton::Primary) {
        if (auto navigable = m_mouse_event_tracking_navigable)
            navigable->event_handler().reset_mouse_input_tracking({});
        m_mouse_event_tracking_navigable = nullptr;
    }
    return root.event_handler().handle_mousedown(device_to_css_point(position), device_to_css_point(screen_position), button, buttons, modifiers, click_count);
}

EventResult Page::handle_mousedown(DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int click_count)
{
    return handle_mousedown(local_traversable(), position, screen_position, button, buttons, modifiers, click_count);
}

void Page::set_mouse_event_tracking_navigable(Badge<EventHandler>, HTML::LocalNavigable& navigable)
{
    m_mouse_event_tracking_navigable = navigable;
}

void Page::set_hover_reporting_navigable(Badge<EventHandler>, GC::Ptr<HTML::LocalNavigable> navigable)
{
    m_hover_reporting_navigable = navigable;
}

EventResult Page::handle_mousemove(HTML::LocalNavigable& root, DevicePixelPoint position, DevicePixelPoint screen_position, unsigned buttons, unsigned modifiers)
{
    return root.event_handler().handle_mousemove(device_to_css_point(position), device_to_css_point(screen_position), buttons, modifiers);
}

EventResult Page::handle_mousemove(DevicePixelPoint position, DevicePixelPoint screen_position, unsigned buttons, unsigned modifiers)
{
    return handle_mousemove(local_traversable(), position, screen_position, buttons, modifiers);
}

EventResult Page::handle_mouseleave(HTML::LocalNavigable& root)
{
    return root.event_handler().handle_mouseleave();
}

EventResult Page::handle_mouseleave()
{
    return handle_mouseleave(local_traversable());
}

#if defined(AK_OS_MACOS)
bool Page::select_word_for_dictionary_lookup(DevicePixelPoint position)
{
    return local_traversable()->event_handler().select_word_for_dictionary_lookup(device_to_css_point(position));
}
#endif

UniqueNodeID Page::node_id_at_position(DevicePixelPoint position)
{
    auto node = local_traversable()->event_handler().target_node_for_mouse_position(device_to_css_point(position));
    if (!node)
        return 0;

    return node->unique_id();
}

EventResult Page::handle_mousewheel(HTML::LocalNavigable& root, DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, double wheel_delta_x, double wheel_delta_y, WheelDeltaPrecision wheel_delta_precision, ScrollGesturePhase scroll_gesture_phase, bool async_scroll_performed_default_action, Optional<AsyncScrollOperation>* async_scroll_operation)
{
    return root.event_handler().handle_mousewheel(device_to_css_point(position), device_to_css_point(screen_position), button, buttons, modifiers, wheel_delta_x, wheel_delta_y, wheel_delta_precision, scroll_gesture_phase, async_scroll_performed_default_action, async_scroll_operation);
}

EventResult Page::handle_mousewheel(DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, double wheel_delta_x, double wheel_delta_y, WheelDeltaPrecision wheel_delta_precision, ScrollGesturePhase scroll_gesture_phase, bool async_scroll_performed_default_action, Optional<AsyncScrollOperation>* async_scroll_operation)
{
    return handle_mousewheel(local_traversable(), position, screen_position, button, buttons, modifiers, wheel_delta_x, wheel_delta_y, wheel_delta_precision, scroll_gesture_phase, async_scroll_performed_default_action, async_scroll_operation);
}

EventResult Page::handle_drag_and_drop_event(HTML::LocalNavigable& root, DragEvent::Type type, DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Vector<HTML::SelectedFile> files)
{
    return root.event_handler().handle_drag_and_drop_event(type, device_to_css_point(position), device_to_css_point(screen_position), button, buttons, modifiers, move(files));
}

EventResult Page::handle_drag_and_drop_event(DragEvent::Type type, DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Vector<HTML::SelectedFile> files)
{
    return handle_drag_and_drop_event(local_traversable(), type, position, screen_position, button, buttons, modifiers, move(files));
}

EventResult Page::handle_pinch_event(HTML::LocalNavigable& root, DevicePixelPoint position, unsigned modifiers, double scale)
{
    return root.event_handler().handle_pinch_event(device_to_css_point(position), modifiers, scale);
}

EventResult Page::handle_pinch_event(DevicePixelPoint position, unsigned modifiers, double scale)
{
    return handle_pinch_event(local_traversable(), position, modifiers, scale);
}

EventResult Page::handle_keydown(UIEvents::KeyCode key, unsigned modifiers, u32 code_point, bool repeat, bool should_insert_text, bool async_scroll_performed_default_action)
{
    // The compositor forwards these updates ahead of keyboard events. Both DOM listeners and a main-thread
    // fallback default action must observe the offsets that have already been presented.
    focused_navigable().local_root()->adopt_pending_async_scroll_offsets(Compositor::AsyncScrollUpdateFreshness::Pushed);
    return focused_navigable().event_handler().handle_keydown(key, modifiers, code_point, repeat, should_insert_text, async_scroll_performed_default_action);
}

EventResult Page::handle_keyup(UIEvents::KeyCode key, unsigned modifiers, u32 code_point, bool repeat)
{
    focused_navigable().local_root()->adopt_pending_async_scroll_offsets(Compositor::AsyncScrollUpdateFreshness::Pushed);
    return focused_navigable().event_handler().handle_keyup(key, modifiers, code_point, repeat);
}

void Page::handle_sdl_input_events()
{
    // The view's input reaches the page displaying the tab.
    if (has_local_traversable())
        local_traversable()->event_handler().handle_sdl_input_events();
}

void Page::invalidate_compositor_keyboard_scroll_state()
{
    if (!m_keyboard_scroll_state_is_current)
        return;
    m_keyboard_scroll_state_is_current = false;
    ++m_keyboard_scroll_state_generation;
    if (m_async_scrolling_enabled && has_local_traversable() && local_traversable()->has_compositor_context()) {
        // No synchronous barrier is needed if the last publication already disabled keyboard scrolling.
        if (m_keyboard_scroll_state_is_scrollable)
            local_traversable()->compositor_context().invalidate_keyboard_scroll_state(m_keyboard_scroll_state_generation);
        local_traversable()->set_needs_repaint();
    }
}

void Page::invalidate_compositor_keyboard_scroll_state_for_document(DOM::Document const& document)
{
    if (has_local_traversable() && local_traversable()->active_document().ptr() == &document)
        invalidate_compositor_keyboard_scroll_state();
}

void Page::keyboard_scroll_event_path_changed(DOM::EventTarget const& target)
{
    if (m_keyboard_scroll_state_is_current && m_keyboard_scroll_event_path.contains([&](auto const& dependency) { return dependency.ptr().ptr() == &target; }))
        invalidate_compositor_keyboard_scroll_state();
}

void Page::keyboard_scroll_dom_tree_changed(DOM::Node const& subtree)
{
    if (!m_keyboard_scroll_state_is_current || !has_local_traversable())
        return;
    auto document = local_traversable()->active_document();
    if (!document || &subtree.document() != document.ptr())
        return;

    keyboard_scroll_event_path_changed(subtree);
    if (auto target = m_keyboard_scroll_dom_target.ptr(); target && subtree.is_shadow_including_inclusive_ancestor_of(*target))
        invalidate_compositor_keyboard_scroll_state();

    // With no focused area, inserting/replacing the body can change the event target even though the new body
    // was not in the previous snapshot's path.
    if (!document->focused_area() && !m_keyboard_scroll_event_path.is_empty()) {
        DOM::Node const* event_target = document->body() ?: &document->root();
        if (m_keyboard_scroll_event_path.first().ptr().ptr() != event_target)
            invalidate_compositor_keyboard_scroll_state();
    }
}

void Page::keyboard_scroll_editability_changed(DOM::Document& document)
{
    if (m_keyboard_scroll_state_is_current && has_local_traversable()
        && local_traversable()->active_document().ptr() == &document
        && m_keyboard_scroll_focus_is_editable != (document.active_input_events_target() != nullptr))
        invalidate_compositor_keyboard_scroll_state();
}

Compositor::KeyboardScrollState Page::take_keyboard_scroll_state_for_compositor(u64 visual_context_tree_structural_epoch)
{
    if (!m_async_scrolling_enabled || !has_local_traversable() || !local_traversable()->has_compositor_context())
        return {};
    auto snapshot = local_traversable()->event_handler().keyboard_scroll_snapshot();
    m_keyboard_scroll_event_path = move(snapshot.event_path);
    m_keyboard_scroll_dom_target = snapshot.scroll_target;
    auto document = local_traversable()->active_document();
    m_keyboard_scroll_focus_is_editable = document && document->active_input_events_target();
    auto state = snapshot.state;
    state.generation = m_keyboard_scroll_state_generation;
    state.visual_context_tree_structural_epoch = visual_context_tree_structural_epoch;
    m_keyboard_scroll_state_is_scrollable = state.target.has_value();
    m_keyboard_scroll_state_is_current = true;
    return state;
}

void Page::invalidate_compositor_wheel_event_listener_state()
{
    ++m_wheel_event_listener_state_generation;

    if (!m_async_scrolling_enabled)
        return;

    for (auto const& root : local_roots()) {
        if (root->has_compositor_context())
            root->compositor_context().invalidate_wheel_event_listener_state(m_wheel_event_listener_state_generation);
    }
}

void Page::update_needs_beforeunload_check()
{
    auto needs_beforeunload_check = [&] {
        if (!has_top_level_traversable())
            return true;

        // Each page reports the listeners of the documents it hosts.
        for (auto const& navigable : hosted_navigables()) {
            auto window = navigable->active_window();
            if (window && window->has_event_listener(HTML::EventNames::beforeunload))
                return true;
        }

        return false;
    }();

    if (m_needs_beforeunload_check == needs_beforeunload_check)
        return;

    m_needs_beforeunload_check = needs_beforeunload_check;
    client().page_did_change_needs_beforeunload_check(m_needs_beforeunload_check);
}

void Page::set_top_level_traversable(GC::Ref<HTML::Navigable> navigable)
{
    VERIFY(!m_top_level_traversable); // Replacement is not allowed!
    VERIFY(&navigable->page() == this);
    m_top_level_traversable = navigable;
    update_needs_beforeunload_check();
}

GC::Ref<HTML::Navigable> Page::top_level_traversable() const
{
    return *m_top_level_traversable;
}

bool Page::has_local_traversable() const
{
    return m_top_level_traversable && is<HTML::LocalNavigable>(*m_top_level_traversable);
}

GC::Ref<HTML::LocalNavigable> Page::local_traversable() const
{
    return as<HTML::LocalNavigable>(*m_top_level_traversable);
}

Vector<GC::Ref<HTML::LocalNavigable>> Page::local_roots() const
{
    Vector<GC::Ref<HTML::LocalNavigable>> roots;
    if (!m_top_level_traversable)
        return roots;
    Function<void(HTML::Navigable&)> collect = [&](HTML::Navigable& navigable) {
        if (navigable.has_been_destroyed())
            return;
        if (auto* local_navigable = as_if<HTML::LocalNavigable>(navigable)) {
            roots.append(*local_navigable);
            return;
        }
        for (auto const& child : as<HTML::RemoteNavigable>(navigable).children())
            collect(*child);
    };
    collect(*m_top_level_traversable);
    return roots;
}

Vector<GC::Root<HTML::LocalNavigable>> Page::hosted_navigables() const
{
    Vector<GC::Root<HTML::LocalNavigable>> navigables;
    for (auto const& root : local_roots())
        navigables.extend(root->hosted_inclusive_descendant_navigables());
    return navigables;
}

GC::Ptr<HTML::Navigable> Page::navigable_with_id(HTML::CrossProcessId id) const
{
    for (auto& navigable : HTML::all_local_navigables()) {
        if (navigable->id() == id && &navigable->page() == this && !navigable->has_been_destroyed())
            return navigable;
    }
    if (auto navigable = HTML::remote_navigable_with_id(*this, id); navigable && !navigable->has_been_destroyed())
        return navigable;
    return nullptr;
}

void Page::create_remote_navigable_graph(Vector<HTML::RemoteNavigableDescriptor> descriptors)
{
    VERIFY(!m_top_level_traversable);
    ensure_compositor_host();

    // The graph is every navigable of the tab, parents before children and siblings in creation order, every one of
    // them hosted by another process until this page begins hosting one.
    for (auto& descriptor : descriptors) {
        GC::Ptr<HTML::Navigable> parent;
        if (descriptor.parent_id.has_value()) {
            parent = HTML::remote_navigable_with_id(*this, *descriptor.parent_id);
            VERIFY(parent);
        }
        auto navigable = HTML::RemoteNavigable::create(*this, descriptor.id, parent, move(descriptor.replicated_state));
        if (parent)
            as<HTML::RemoteNavigable>(*parent).append_child(navigable);
        else
            set_top_level_traversable(navigable);
    }
    VERIFY(m_top_level_traversable);
}

// The page destroys a child before the UI process hears of it, and messages about that child, or about its subtree,
// can still be on their way here: a navigable the page no longer holds is left alone.
void Page::insert_remote_navigable(HTML::RemoteNavigableDescriptor descriptor)
{
    VERIFY(descriptor.parent_id.has_value());
    auto parent = HTML::remote_navigable_with_id(*this, *descriptor.parent_id);
    if (!parent)
        return;
    parent->append_child(HTML::RemoteNavigable::create(*this, descriptor.id, parent, move(descriptor.replicated_state)));
}

void Page::remove_remote_navigable(HTML::CrossProcessId id)
{
    auto navigable = HTML::remote_navigable_with_id(*this, id);
    if (!navigable)
        return;
    as<HTML::RemoteNavigable>(*navigable->parent()).remove_child(*navigable);
    navigable->set_has_been_destroyed();
    navigable->remove_from_all_remote_navigables();
}

void Page::update_remote_navigable(HTML::CrossProcessId id, HTML::ReplicatedNavigableState state)
{
    auto navigable = HTML::remote_navigable_with_id(*this, id);
    if (!navigable)
        return;
    navigable->set_replicated_state(move(state));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#completely-finish-loading
void Page::content_navigable_completely_finished_loading(HTML::CrossProcessId id)
{
    // NB: The document completely finished loading in the process hosting it. Its container, whose document this page
    //     hosts, runs steps 4 and 5 here.
    auto navigable = HTML::remote_navigable_with_id(*this, id);
    if (!navigable)
        return;
    if (auto container = navigable->container())
        container->content_navigable_completely_finished_loading();
}

void Page::discard_provisional_navigable_of(HTML::RemoteNavigable& remote_navigable)
{
    auto navigable = remote_navigable.provisional_navigable();
    if (!navigable)
        return;
    remote_navigable.set_provisional_navigable(nullptr);
    navigable->clear_provisional_for();
    navigable->set_container({}, nullptr);
    navigable->set_has_been_destroyed();
    if (auto document = navigable->active_document())
        document->destroy_a_document_and_its_descendants();
    navigable->remove_from_all_local_navigables();
}

GC::Ref<HTML::LocalNavigable> Page::begin_hosting(HTML::CrossProcessId id, HTML::SessionHistoryEntryDescriptor const& current_history_entry, HTML::VisibilityState system_visibility_state)
{
    auto navigable = HTML::remote_navigable_with_id(*this, id);
    VERIFY(navigable && !navigable->has_been_destroyed());
    // A host chosen for the navigable's previous navigation, which never activated a document here.
    discard_provisional_navigable_of(*navigable);
    return HTML::LocalNavigable::create_stand_in({}, *navigable, current_history_entry, system_visibility_state);
}

void Page::adopt_hosted(HTML::LocalNavigable& navigable)
{
    auto remote_navigable = navigable.provisional_for();
    VERIFY(remote_navigable && remote_navigable->provisional_navigable().ptr() == &navigable);

    if (auto container = remote_navigable->container())
        container->swap_content_navigable_to_local({}, navigable);
    else
        as<HTML::RemoteNavigable>(*remote_navigable->parent()).replace_child(*remote_navigable, navigable);

    navigable.clear_provisional_for();
    remote_navigable->set_provisional_navigable(nullptr);
    remote_navigable->set_has_been_destroyed();
    remote_navigable->remove_from_all_remote_navigables();
}

void Page::discard_provisional_navigable(HTML::CrossProcessId id)
{
    if (auto navigable = HTML::remote_navigable_with_id(*this, id))
        discard_provisional_navigable_of(*navigable);
}

void Page::stop_hosting(HTML::CrossProcessId id, HTML::ReplicatedNavigableState state)
{
    auto navigable = navigable_with_id(id);
    if (!navigable)
        return;
    // The page retired the navigable when its document was unloaded. This is the state its next document activated
    // with.
    if (auto* remote_navigable = as_if<HTML::RemoteNavigable>(*navigable)) {
        remote_navigable->set_replicated_state(move(state));
        return;
    }
    stop_hosting(as<HTML::LocalNavigable>(*navigable), move(state));
}

void Page::stop_hosting(HTML::LocalNavigable& local_navigable, HTML::ReplicatedNavigableState state)
{
    if (auto container = local_navigable.container()) {
        container->swap_content_navigable_to_remote({}, move(state));
        return;
    }

    // A local root whose parent's document another process hosts: the RemoteNavigable takes its place among the
    // parent's children.
    auto& parent = as<HTML::RemoteNavigable>(*local_navigable.parent());
    auto remote_navigable = HTML::RemoteNavigable::create(*this, local_navigable.id(), parent, move(state));
    // The container's page can destroy the navigable while its document activates here, after the UI process's walk
    // unloaded the document it displayed before: the document is unloaded now, as that walk would have.
    if (auto document = local_navigable.active_document()) {
        local_navigable.inform_the_navigation_api_about_child_navigable_destruction();
        document->unload();
    }
    parent.replace_child(local_navigable, remote_navigable);
    local_navigable.set_has_been_destroyed();
    local_navigable.remove_from_all_local_navigables();
}

void Page::discard()
{
    // A tab closed while this page still hosted a document of it: the document goes without the unload the UI process
    // runs otherwise, as the documents of a top-level traversable being destroyed do.
    for (auto const& navigable : local_roots()) {
        if (auto document = navigable->active_document())
            document->destroy_a_document_and_its_descendants();
        navigable->set_has_been_destroyed();
        navigable->remove_from_all_local_navigables();
    }
    client().page_did_close();
}

void Page::host_navigable(HTML::CrossProcessId id, HTML::SessionHistoryEntryDescriptor const& current_history_entry, HTML::VisibilityState system_visibility_state)
{
    // The provisional navigable took the node over when its document activated; the hand-over follows it.
    auto navigable = navigable_with_id(id);
    if (!navigable || is<HTML::LocalNavigable>(*navigable))
        return;
    adopt_hosted(begin_hosting(id, current_history_entry, system_visibility_state));
}

HTML::BrowsingContextGroup& Page::browsing_context_group()
{
    // NB: Created with the tab's top-level browsing context when this process holds it, and empty until then in a
    //     process holding only parts of the tab under parents hosted elsewhere.
    if (!m_browsing_context_group)
        m_browsing_context_group = GC::Heap::the().allocate<HTML::BrowsingContextGroup>(*this);
    return *m_browsing_context_group;
}

void Page::set_browsing_context_group(Badge<HTML::BrowsingContextGroup>, GC::Ref<HTML::BrowsingContextGroup> group)
{
    m_browsing_context_group = group;
}

HTML::HistoryExecutor& Page::history_executor()
{
    return *m_history_executor;
}

void Page::did_complete_window_rect_request(u64 completion_id)
{
    if (m_window_rect_observer)
        m_window_rect_observer->function()({ window_position(), window_size() }, completion_id);
}

template<typename ResponseType>
static ResponseType spin_event_loop_until_dialog_closed(PageClient& client, Optional<ResponseType>& response, SourceLocation location = SourceLocation::current())
{
    auto& event_loop = Web::HTML::current_settings_object().responsible_event_loop();
    auto pause_handle = event_loop.pause();

    Web::Platform::EventLoopPlugin::the().spin_until(GC::create_function(GC::Heap::the(), [&]() {
        return response.has_value() || !client.is_connection_open();
    }));

    if (!client.is_connection_open()) {
        dbgln("WebContent client disconnected during {}. Exiting peacefully.", location.function_name());
        exit(0);
    }

    return response.release_value();
}

void Page::did_request_alert(Utf16String const& message)
{
    m_pending_dialog = PendingDialog::Alert;
    m_client->page_did_request_alert(message);

    if (!message.is_empty())
        m_pending_dialog_text = message;

    spin_event_loop_until_dialog_closed(*m_client, m_pending_alert_response);
}

void Page::alert_closed()
{
    if (m_pending_dialog == PendingDialog::Alert) {
        m_pending_alert_response = Empty {};
        on_pending_dialog_closed();
    }
}

bool Page::did_request_confirm(Utf16String const& message)
{
    m_pending_dialog = PendingDialog::Confirm;
    m_client->page_did_request_confirm(message);

    if (!message.is_empty())
        m_pending_dialog_text = message;

    return spin_event_loop_until_dialog_closed(*m_client, m_pending_confirm_response);
}

void Page::confirm_closed(bool accepted)
{
    if (m_pending_dialog == PendingDialog::Confirm) {
        m_pending_confirm_response = accepted;
        on_pending_dialog_closed();
    }
}

Optional<Utf16String> Page::did_request_prompt(Utf16String const& message, Utf16String const& default_)
{
    m_pending_dialog = PendingDialog::Prompt;
    m_client->page_did_request_prompt(message, default_);

    if (!message.is_empty())
        m_pending_dialog_text = message;

    return spin_event_loop_until_dialog_closed(*m_client, m_pending_prompt_response);
}

void Page::prompt_closed(Optional<Utf16String> response)
{
    if (m_pending_dialog == PendingDialog::Prompt) {
        m_pending_prompt_response = move(response);
        on_pending_dialog_closed();
    }
}

void Page::dismiss_dialog(GC::Ref<GC::Function<void()>> on_dialog_closed)
{
    m_on_pending_dialog_closed = on_dialog_closed;

    switch (m_pending_dialog) {
    case PendingDialog::None:
        break;
    case PendingDialog::Alert:
        m_client->page_did_request_accept_dialog();
        break;
    case PendingDialog::Confirm:
    case PendingDialog::Prompt:
        m_client->page_did_request_dismiss_dialog();
        break;
    }
}

void Page::accept_dialog(GC::Ref<GC::Function<void()>> on_dialog_closed)
{
    m_on_pending_dialog_closed = on_dialog_closed;

    switch (m_pending_dialog) {
    case PendingDialog::None:
        break;
    case PendingDialog::Alert:
    case PendingDialog::Confirm:
    case PendingDialog::Prompt:
        m_client->page_did_request_accept_dialog();
        break;
    }
}

void Page::on_pending_dialog_closed()
{
    m_pending_dialog = PendingDialog::None;
    m_pending_dialog_text.clear();

    if (m_on_pending_dialog_closed) {
        m_on_pending_dialog_closed->function()();
        m_on_pending_dialog_closed = nullptr;
    }
}

void Page::did_request_color_picker(GC::Weak<HTML::HTMLInputElement> target, Color current_color)
{
    if (m_pending_non_blocking_dialog == PendingNonBlockingDialog::None) {
        m_pending_non_blocking_dialog = PendingNonBlockingDialog::ColorPicker;
        m_pending_non_blocking_dialog_target = move(target);

        m_client->page_did_request_color_picker(current_color);
    }
}

void Page::color_picker_update(Optional<Color> picked_color, HTML::ColorPickerUpdateState state)
{
    if (m_pending_non_blocking_dialog == PendingNonBlockingDialog::ColorPicker) {
        if (state == HTML::ColorPickerUpdateState::Closed)
            m_pending_non_blocking_dialog = PendingNonBlockingDialog::None;

        if (m_pending_non_blocking_dialog_target) {
            auto& input_element = as<HTML::HTMLInputElement>(*m_pending_non_blocking_dialog_target);
            input_element.did_pick_color(move(picked_color), state);
            if (state == HTML::ColorPickerUpdateState::Closed)
                m_pending_non_blocking_dialog_target = nullptr;
        }
    }
}

void Page::did_request_file_picker(GC::Weak<HTML::HTMLInputElement> target, HTML::FileFilter const& accepted_file_types, HTML::AllowMultipleFiles allow_multiple_files)
{
    if (m_pending_non_blocking_dialog == PendingNonBlockingDialog::None) {
        m_pending_non_blocking_dialog = PendingNonBlockingDialog::FilePicker;
        m_pending_non_blocking_dialog_target = move(target);

        m_client->page_did_request_file_picker(accepted_file_types, allow_multiple_files);
    }
}

void Page::file_picker_closed(Span<HTML::SelectedFile> selected_files)
{
    if (m_pending_non_blocking_dialog == PendingNonBlockingDialog::FilePicker) {
        m_pending_non_blocking_dialog = PendingNonBlockingDialog::None;

        if (m_pending_non_blocking_dialog_target) {
            auto& input_element = as<HTML::HTMLInputElement>(*m_pending_non_blocking_dialog_target);
            input_element.did_select_files(selected_files);

            m_pending_non_blocking_dialog_target = nullptr;
        }
    }
}

void Page::did_request_select_dropdown(GC::Weak<HTML::HTMLSelectElement> target, Web::CSSPixelPoint content_position, Web::CSSPixels minimum_width, Vector<Web::HTML::SelectItem> items)
{
    if (m_pending_non_blocking_dialog == PendingNonBlockingDialog::None) {
        m_pending_non_blocking_dialog = PendingNonBlockingDialog::Select;
        m_pending_non_blocking_dialog_target = move(target);
        m_client->page_did_request_select_dropdown(content_position, minimum_width, move(items));
    }
}

void Page::select_dropdown_closed(Optional<u32> const& selected_item_id)
{
    if (m_pending_non_blocking_dialog == PendingNonBlockingDialog::Select) {
        m_pending_non_blocking_dialog = PendingNonBlockingDialog::None;

        if (m_pending_non_blocking_dialog_target) {
            auto& select_element = as<HTML::HTMLSelectElement>(*m_pending_non_blocking_dialog_target);
            select_element.did_select_item(selected_item_id);
            m_pending_non_blocking_dialog_target = nullptr;
        }
    }
}

void Page::request_clipboard_entries(ClipboardRequest request)
{
    auto request_id = m_next_clipboard_request_id++;
    m_pending_clipboard_requests.set(request_id, request);

    client().page_did_request_clipboard_entries(request_id);
}

void Page::retrieved_clipboard_entries(u64 request_id, Vector<Clipboard::SystemClipboardItem> items)
{
    if (auto request = m_pending_clipboard_requests.take(request_id); request.has_value())
        (*request)->function()(move(items));
}

u64 Page::request_geolocation_position(GeolocationPositionCallback callback, GeolocationRequestType type)
{
    // This is the browser-process bridge for the Geolocation spec's "try to acquire position data from the underlying system" step.
    auto request_id = m_next_geolocation_request_id++;
    m_pending_geolocation_requests.set(request_id, { callback, type });

    if (type == GeolocationRequestType::Watch) {
        client().page_did_start_geolocation_position_watch(request_id);
        return request_id;
    }

    if (!m_active_geolocation_request_id.has_value()) {
        m_active_geolocation_request_id = request_id;
        client().page_did_request_geolocation_position(request_id);
    }

    return request_id;
}

void Page::cancel_geolocation_position_request(u64 request_id)
{
    auto request = m_pending_geolocation_requests.take(request_id);
    if (!request.has_value())
        return;

    if (request->type == GeolocationRequestType::Watch) {
        client().page_did_stop_geolocation_position_watch(request_id);
        return;
    }

    if (m_active_geolocation_request_id != request_id)
        return;

    client().page_did_cancel_geolocation_position_request(request_id);
    m_active_geolocation_request_id = {};
    for (auto const& entry : m_pending_geolocation_requests) {
        if (entry.value.type == GeolocationRequestType::OneShot) {
            m_active_geolocation_request_id = entry.key;
            client().page_did_request_geolocation_position(entry.key);
            return;
        }
    }
}

void Page::receive_geolocation_position(u64 request_id, GeolocationPositionResult result)
{
    auto request = m_pending_geolocation_requests.get(request_id);
    if (!request.has_value())
        return;

    if (request->type == GeolocationRequestType::Watch) {
        auto callback = request->callback;
        if (result.has<Geolocation::GeolocationPositionError::ErrorCode>()
            && result.get<Geolocation::GeolocationPositionError::ErrorCode>() == Geolocation::GeolocationPositionError::ErrorCode::PermissionDenied)
            m_pending_geolocation_requests.remove(request_id);
        callback->function()(result);
        return;
    }

    if (m_active_geolocation_request_id != request_id)
        return;

    m_active_geolocation_request_id = {};

    Vector<GeolocationPositionCallback> callbacks;
    Vector<u64> completed_request_ids;
    for (auto const& entry : m_pending_geolocation_requests) {
        if (entry.value.type != GeolocationRequestType::OneShot)
            continue;
        callbacks.append(entry.value.callback);
        completed_request_ids.append(entry.key);
    }

    for (auto completed_request_id : completed_request_ids)
        m_pending_geolocation_requests.remove(completed_request_id);

    for (auto const& callback : callbacks)
        callback->function()(result);
}

void Page::register_media_element(Badge<HTML::HTMLMediaElement>, UniqueNodeID media_id)
{
    m_media_elements.append(media_id);
}

void Page::unregister_media_element(Badge<HTML::HTMLMediaElement>, UniqueNodeID media_id)
{
    m_media_elements.remove_all_matching([&](auto candidate_id) {
        return candidate_id == media_id;
    });
}

template<typename Callback>
void Page::for_each_media_element(Callback&& callback)
{
    for (auto media_id : m_media_elements) {
        if (auto* node = DOM::Node::from_unique_id(media_id))
            callback(as<HTML::HTMLMediaElement>(*node));
    }
}

bool Page::has_media_element_playing_audio(DOM::Document const& document) const
{
    for (auto media_id : m_media_elements) {
        auto* node = DOM::Node::from_unique_id(media_id);
        if (node && &node->document() == &document && as<HTML::HTMLMediaElement>(*node).is_playing_audio())
            return true;
    }
    return false;
}

void Page::sync_media_element_video_sink_ticking()
{
    for_each_media_element([&](auto& media_element) {
        media_element.sync_video_sink_ticking();
        if (auto* video_element = as_if<HTML::HTMLVideoElement>(&media_element))
            Painting::push_video_paint_facts(*video_element);
    });
}

void Page::restore_all_media_element_video_sinks()
{
    for_each_media_element([&](auto& media_element) {
        media_element.add_current_video_sink();
    });
}

void Page::detach_all_media_element_video_sinks_after_compositor_lost()
{
    for_each_media_element([&](auto& media_element) {
        media_element.detach_video_sink_edge();
    });
}

void Page::register_canvas_element(Badge<HTML::HTMLCanvasElement>, UniqueNodeID canvas_id)
{
    m_canvas_elements.append(canvas_id);
}

void Page::unregister_canvas_element(Badge<HTML::HTMLCanvasElement>, UniqueNodeID canvas_id)
{
    m_canvas_elements.remove_all_matching([&](auto candidate_id) {
        return candidate_id == canvas_id;
    });
}

template<typename Callback>
void Page::for_each_canvas_element(Callback&& callback)
{
    for (auto canvas_id : m_canvas_elements) {
        if (auto* node = DOM::Node::from_unique_id(canvas_id))
            callback(as<HTML::HTMLCanvasElement>(*node));
    }
}

void Page::prepare_canvas_contexts_for_compositing()
{
    for_each_canvas_element([](auto& canvas_element) {
        canvas_element.prepare_for_compositing();
    });

    // Preparing only records commands and present markers into the shared
    // canvas command stream; flush it here so canvases reach the Compositor
    // even when nothing else repaints this rendering update.
    if (has_compositor_host())
        compositor_host().flush_canvas_2d_stream();
}

void Page::notify_all_canvas_elements_of_lost_backing_storage()
{
    for_each_canvas_element([](auto& canvas_element) {
        canvas_element.notify_compositor_backing_storage_lost();
    });
}

void Page::notify_all_webgl_contexts_lost()
{
    for_each_canvas_element([](auto& canvas_element) {
        canvas_element.notify_compositor_connection_lost();
    });
}

void Page::record_context_menu_request(Badge<EventHandler>, ContextMenuRequest request)
{
    m_context_menu_request = request;
}

Optional<Page::ContextMenuRequest> Page::take_context_menu_request()
{
    auto request = m_context_menu_request;
    m_context_menu_request.clear();
    return request;
}

void Page::did_request_media_context_menu(UniqueNodeID media_id, CSSPixelPoint position, ByteString const& target, unsigned modifiers, MediaContextMenu const& menu)
{
    m_media_context_menu_element_id = media_id;
    client().page_did_request_media_context_menu(position, target, modifiers, menu);
}

void Page::toggle_media_play_state()
{
    auto media_element = media_context_menu_element();
    if (!media_element)
        return;

    if (media_element->potentially_playing())
        media_element->pause();
    else
        media_element->play_from_user_interaction();
}

void Page::toggle_media_mute_state()
{
    auto media_element = media_context_menu_element();
    if (!media_element)
        return;

    // AD-HOC: An execution context is required for Promise creation hooks.
    HTML::TemporaryExecutionContext execution_context { media_element->document().relevant_settings_object() };

    media_element->set_muted(!media_element->muted());
}

void Page::toggle_media_loop_state()
{
    auto media_element = media_context_menu_element();
    if (!media_element)
        return;

    // AD-HOC: An execution context is required for Promise creation hooks.
    HTML::TemporaryExecutionContext execution_context { media_element->document().relevant_settings_object() };

    if (media_element->has_attribute(HTML::AttributeNames::loop))
        media_element->remove_attribute(HTML::AttributeNames::loop);
    else
        media_element->set_attribute_value(HTML::AttributeNames::loop, Utf16String {});
}

void Page::toggle_media_fullscreen_state()
{
    auto media_element = media_context_menu_element();
    if (!media_element)
        return;

    HTML::TemporaryExecutionContext execution_context { media_element->document().relevant_settings_object() };
    media_element->toggle_fullscreen();
}

void Page::toggle_media_controls_state()
{
    auto media_element = media_context_menu_element();
    if (!media_element)
        return;

    HTML::TemporaryExecutionContext execution_context { media_element->document().relevant_settings_object() };

    if (media_element->has_attribute(HTML::AttributeNames::controls))
        media_element->remove_attribute(HTML::AttributeNames::controls);
    else
        media_element->set_attribute_value(HTML::AttributeNames::controls, Utf16String {});
}

void Page::set_page_mute_state(HTML::MuteState mute_state)
{
    if (m_mute_state == mute_state)
        return;

    m_mute_state = mute_state;

    for_each_media_element([&](auto& media_element) {
        media_element.page_mute_state_changed({});
    });
}

GC::Ptr<HTML::HTMLMediaElement> Page::media_context_menu_element()
{
    if (!m_media_context_menu_element_id.has_value())
        return nullptr;

    auto* dom_node = DOM::Node::from_unique_id(*m_media_context_menu_element_id);
    if (dom_node == nullptr)
        return nullptr;

    if (!is<HTML::HTMLMediaElement>(dom_node))
        return nullptr;

    return static_cast<HTML::HTMLMediaElement*>(dom_node);
}

void Page::set_user_style(Utf16String source)
{
    // An empty user style is indistinguishable from having none, and shadow scopes only share style
    // caches while no user style is present, so setting an empty source clears it outright. This
    // matters across tests too: page state outlives the documents a test runner loads into it.
    if (source.is_empty())
        m_user_style_sheet_source = {};
    else
        m_user_style_sheet_source = move(source);
    invalidate_user_style();
}

void Page::set_content_blocking_enabled(bool enabled)
{
    auto& blocker = ContentBlocker::the();
    if (blocker.filtering_enabled() == enabled)
        return;

    auto has_cosmetic_rules = blocker.has_cosmetic_rules();
    blocker.set_filtering_enabled(enabled);
    if (has_cosmetic_rules)
        invalidate_user_style();
}

void Page::invalidate_user_style()
{
    if (!has_top_level_traversable())
        return;

    auto invalidate_document = [](DOM::Document& document) {
        document.invalidate_content_blocker_style_sheet();
        document.style_scope().invalidate_user_style_sheet();
        document.for_each_shadow_root([](auto& shadow_root) {
            shadow_root.record_style_environment_change();
        });
        document.record_style_environment_change();
    };

    // Each page invalidates the documents it hosts.
    for (auto const& navigable : hosted_navigables()) {
        if (auto document = navigable->active_document())
            invalidate_document(*document);
    }
}

void Page::invalidate_style_for_preference_change()
{
    if (!has_top_level_traversable())
        return;

    auto invalidate_document = [](DOM::Document& document) {
        document.record_style_environment_change();
        document.set_needs_media_query_evaluation();
    };

    // Each page invalidates the documents it hosts.
    for (auto const& navigable : hosted_navigables()) {
        if (auto document = navigable->active_document())
            invalidate_document(*document);
    }
}

Vector<GC::Root<DOM::Document>> Page::documents_in_active_window() const
{
    if (!has_local_traversable())
        return {};

    auto documents = HTML::main_thread_event_loop().documents_in_this_event_loop_matching([&](auto& document) {
        return document.window() == local_traversable()->active_window();
    });

    return documents;
}

void Page::clear_selection()
{
    for (auto const& document : documents_in_active_window()) {
        auto selection = document->get_selection();
        if (!selection)
            continue;

        selection->remove_all_ranges();
    }
}

Page::FindInPageResult Page::perform_find_in_page_query(FindInPageQuery const& query, Optional<SearchDirection> direction)
{
    VERIFY(has_local_traversable());

    Vector<GC::Root<DOM::Range>> all_matches;

    auto active_range = [](auto& document) -> GC::Ptr<DOM::Range> {
        auto selection = document.get_selection();
        if (!selection || selection->is_collapsed())
            return {};

        return selection->range();
    };

    auto find_current_match_index = [this](DOM::Range& range, auto const& matches) -> Optional<size_t> {
        // Always return the first match if there is no active query.
        if (!m_last_find_in_page_query.has_value())
            return 0;

        for (size_t i = 0; i < matches.size(); ++i) {
            auto boundary_comparison_or_error = matches[i]->compare_boundary_points(DOM::Range::HowToCompareBoundaryPoints::START_TO_START, range);
            if (!boundary_comparison_or_error.is_error() && boundary_comparison_or_error.value() >= 0)
                return i;
        }

        return {};
    };

    auto should_update_match_index = false;
    for (auto const& document : documents_in_active_window()) {
        auto matches = document->find_matching_text(query.string, query.case_sensitivity);
        if (GC::Ptr { document.ptr() } == local_traversable()->active_document()) {
            if (auto range = active_range(*document)) {
                auto new_match_index = find_current_match_index(*range, matches);
                should_update_match_index = true;
                m_find_in_page_match_index = new_match_index.value_or(0) + all_matches.size();
            } else {
                m_find_in_page_match_index = all_matches.size();
            }
        }

        all_matches.extend(move(matches));
    }

    if (auto active_document = local_traversable()->active_document()) {
        if (m_last_find_in_page_url.serialize(URL::ExcludeFragment::Yes) != active_document->url().serialize(URL::ExcludeFragment::Yes)) {
            m_last_find_in_page_url = local_traversable()->active_document()->url();
            m_find_in_page_match_index = 0;
        }
    }

    if (direction.has_value() && should_update_match_index) {
        if (direction == SearchDirection::Forward) {
            if (m_find_in_page_match_index >= all_matches.size() - 1) {
                if (query.wrap_around == WrapAround::No)
                    return {};
                m_find_in_page_match_index = 0;
            } else {
                m_find_in_page_match_index++;
            }
        } else {
            if (m_find_in_page_match_index == 0) {
                if (query.wrap_around == WrapAround::No)
                    return {};
                m_find_in_page_match_index = all_matches.size() - 1;
            } else {
                m_find_in_page_match_index--;
            }
        }
    }

    update_find_in_page_selection(all_matches, query.clear_selection_on_no_match);

    return Page::FindInPageResult {
        .current_match_index = m_find_in_page_match_index,
        .total_match_count = all_matches.size(),
    };
}

Page::FindInPageResult Page::find_in_page(FindInPageQuery const& query)
{
    if (!has_local_traversable())
        return {};

    if (query.string.is_empty()) {
        m_last_find_in_page_query = {};
        clear_selection();
        return {};
    }

    auto result = perform_find_in_page_query(query);

    m_last_find_in_page_query = query;
    m_last_find_in_page_url = local_traversable()->active_document()->url();

    return result;
}

Page::FindInPageResult Page::find_in_page_next_match()
{
    if (!(m_last_find_in_page_query.has_value() && has_local_traversable()))
        return {};

    auto result = perform_find_in_page_query(*m_last_find_in_page_query, SearchDirection::Forward);
    return result;
}

Page::FindInPageResult Page::find_in_page_previous_match()
{
    if (!(m_last_find_in_page_query.has_value() && has_local_traversable()))
        return {};

    auto result = perform_find_in_page_query(*m_last_find_in_page_query, SearchDirection::Backward);
    return result;
}

void Page::update_find_in_page_selection(Vector<GC::Root<DOM::Range>> matches, ClearSelectionOnNoMatch clear_selection_on_no_match)
{
    if (matches.is_empty()) {
        if (clear_selection_on_no_match == ClearSelectionOnNoMatch::Yes)
            clear_selection();
        return;
    }

    clear_selection();

    auto current_range = matches[m_find_in_page_match_index];
    auto common_ancestor_container = current_range->common_ancestor_container();
    auto& document = common_ancestor_container->document();
    if (!document.window())
        return;

    auto selection = document.get_selection();
    if (!selection)
        return;

    selection->add_range(*current_range);

    if (auto element = common_ancestor_container->parent_element()) {
        DOM::Element::ScrollIntoViewOptions scroll_options;
        scroll_options.block = DOM::Element::ScrollLogicalPosition::Nearest;
        scroll_options.inline_ = DOM::Element::ScrollLogicalPosition::Nearest;
        scroll_options.behavior = DOM::Element::ScrollBehavior::Instant;
        element->scroll_into_view(scroll_options, nullptr);
    }
}

void Page::enqueue_fullscreen_enter(GC::Ref<DOM::Element> element, GC::Ref<DOM::Document> pending_doc, DOM::RequestFullscreenError error, GC::Ptr<WebIDL::Promise> promise, Fullscreen::RequestType request_type)
{
    m_pending_fullscreen_operations.enqueue(PendingFullscreenEnter { element, pending_doc, error, promise, request_type });
    // NOTE: Processing is deferred because the spec says "run the remaining steps in parallel",
    //       meaning the caller's synchronous JS should complete before we process the operation.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [this]() {
        process_pending_fullscreen_operations();
    }));
}

void Page::enqueue_fullscreen_exit(GC::Ref<DOM::Document> doc, bool resize, GC::Ptr<WebIDL::Promise> promise)
{
    m_pending_fullscreen_operations.enqueue(PendingFullscreenExit { doc, resize, promise });
    // NOTE: Processing is deferred because the spec says "run the remaining steps in parallel",
    //       meaning the caller's synchronous JS should complete before we process the operation.
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [this]() {
        process_pending_fullscreen_operations();
    }));
}

void Page::process_pending_fullscreen_operations()
{
    // FIXME: The Fullscreen API interacts with the top-level traversable's viewport. With site-isolation,
    //        an iframe's content process won't have direct access to this Page, so fullscreen operations
    //        will need to be routed through IPC to the top-level process.

    // NOTE: Resolving/rejecting promises during processing may trigger JS microtasks that re-enter
    //       this function (e.g., JS calls exitFullscreen() after a requestFullscreen() promise resolves).
    //       The outer call's while loop will pick up newly enqueued items.
    if (m_processing_fullscreen_operations)
        return;
    m_processing_fullscreen_operations = true;
    ScopeGuard guard = [this] { m_processing_fullscreen_operations = false; };

    while (!m_pending_fullscreen_operations.is_empty()) {
        auto& front = m_pending_fullscreen_operations.head();

        auto processed = front.visit(
            [&](PendingFullscreenEnter& enter) -> bool {
                // https://fullscreen.spec.whatwg.org/#dom-element-requestfullscreen

                // 8. If error is false, then resize pendingDoc's node navigable's top-level traversable's
                //    active document's viewport's dimensions, optionally taking into account
                //    options["navigationUI"]:
                if (enter.error == DOM::RequestFullscreenError::False) {
                    if (m_viewport_is_fullscreen == ViewportIsFullscreen::No) {
                        if (!m_fullscreen_ipc_sent_to_ui) {
                            m_client->page_did_request_fullscreen_window();
                            m_fullscreen_ipc_sent_to_ui = true;
                        }
                        // NB: Stop processing here and wait for a change in the fullscreen state if we aren't
                        //     in the desired state yet.
                        return false;
                    }

                    // 9. If any of the following conditions are false, then set error to true:
                    //    * This's node document is pendingDoc.
                    //    * The fullscreen element ready check for this returns true.
                    if (enter.element->owner_document() != GC::Ptr { enter.pending_doc.ptr() })
                        enter.error = DOM::RequestFullscreenError::ElementNodeDocIsNotPendingDoc;
                    else if (!enter.element->is_element_ready_for_fullscreen())
                        enter.error = DOM::RequestFullscreenError::ElementReadyCheckFailed;
                }

                auto& realm = HTML::relevant_realm(*enter.pending_doc);
                HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

                // 10. If error is true:
                if (enter.error != DOM::RequestFullscreenError::False) {
                    // 1. Append (fullscreenerror, this) to pendingDoc's list of pending fullscreen events.
                    enter.pending_doc->append_pending_fullscreen_change(DOM::PendingFullscreenEvent::Type::Error, enter.element, enter.request_type);

                    // 2. Reject promise with a TypeError exception and terminate these steps.
                    if (enter.promise)
                        WebIDL::reject_promise(*enter.promise, JS::TypeError::create(realm, DOM::request_fullscreen_error_to_string(enter.error)));
                    return true;
                }

                // 11. Let fullscreenElements be an ordered set initially consisting of this.
                auto fullscreen_elements = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<DOM::Element>>>();
                fullscreen_elements->elements().append(enter.element);

                // 12. While true:
                while (true) {
                    // 1. Let last be the last item of fullscreenElements.
                    auto last = fullscreen_elements->elements().last();

                    // 2. Let container be last's node navigable's container.
                    auto container = last->navigable()->container();

                    // 3. If container is null, then break.
                    if (!container)
                        break;

                    // 4. Append container to fullscreenElements.
                    fullscreen_elements->elements().append(*container);
                }

                // 13. For each element in fullscreenElements:
                for (auto& element : fullscreen_elements->elements()) {
                    // 1. Let doc be element's node document.
                    auto& doc = element->document();

                    // 2. If element is doc's fullscreen element, continue.
                    if (doc.fullscreen_element() == element)
                        continue;

                    // 3. If element is this and this is an iframe element, then set element's iframe fullscreen flag.
                    if (element == enter.element && is<HTML::HTMLIFrameElement>(*enter.element))
                        as<HTML::HTMLIFrameElement>(*element).set_iframe_fullscreen_flag(true);

                    // 4. Fullscreen element within doc.
                    doc.fullscreen_element_within_doc(element, enter.request_type);

                    // 5. Append (fullscreenchange, element) to doc's list of pending fullscreen events.
                    doc.append_pending_fullscreen_change(DOM::PendingFullscreenEvent::Type::Change, element, enter.request_type);
                }

                // 14. Resolve promise with undefined
                if (enter.promise)
                    WebIDL::resolve_promise(*enter.promise);
                return true;
            },
            [&](PendingFullscreenExit& exit) -> bool {
                auto& realm = HTML::relevant_realm(*exit.doc);
                HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

                // https://fullscreen.spec.whatwg.org/#exit-fullscreen

                // FIXME: 9. Run the fully unlock the screen orientation steps with doc.

                // 10. If resize is true, resize doc's viewport to its "normal" dimensions.
                if (exit.resize && m_viewport_is_fullscreen == ViewportIsFullscreen::Yes) {
                    if (!m_fullscreen_ipc_sent_to_ui) {
                        m_client->page_did_request_exit_fullscreen();
                        m_fullscreen_ipc_sent_to_ui = true;
                    }
                    // NB: Stop processing here and wait for a change in the fullscreen state if we aren't
                    //     in the desired state yet.
                    return false;
                }

                // 11. If doc's fullscreen element is null, then resolve promise with undefined and terminate these
                //     steps.
                if (!exit.doc->fullscreen_element()) {
                    if (exit.promise)
                        WebIDL::resolve_promise(*exit.promise);
                    return true;
                }

                // 12. Let exitDocs be the result of collecting documents to unfullscreen given doc.
                auto exit_docs = exit.doc->collect_documents_to_unfullscreen();

                // 13. Let descendantDocs be an ordered set consisting of doc's descendant navigables' active documents
                //     whose fullscreen element is non-null, if any, in tree order.
                auto descendant_docs = GC::Heap::the().allocate<GC::HeapVector<GC::Ref<DOM::Document>>>();
                // FIXME: Unfullscreen a descendant hosted by another process there, through the UI process. The cast
                //        asks for its document here.
                for (auto& descendant : exit.doc->descendant_navigables()) {
                    auto& local_descendant = as<HTML::LocalNavigable>(*descendant);
                    if (local_descendant.active_document()->fullscreen_element())
                        descendant_docs->elements().append(*local_descendant.active_document());
                }

                // 14. For each exitDoc in exitDocs:
                for (auto& exit_doc : exit_docs->elements()) {
                    auto fullscreen_element = exit_doc->fullscreen_element();

                    // 1. Append (fullscreenchange, exitDoc's fullscreen element) to exitDoc's list of pending
                    //    fullscreen events.
                    exit_doc->append_pending_fullscreen_change(DOM::PendingFullscreenEvent::Type::Change, *fullscreen_element, fullscreen_element->fullscreen_request_type());

                    // 2. If resize is true, unfullscreen exitDoc.
                    if (exit.resize)
                        exit_doc->unfullscreen();
                    // 3. Otherwise, unfullscreen exitDoc's fullscreen element.
                    else
                        exit_doc->unfullscreen_element(*exit_doc->fullscreen_element());
                }

                // 15. For each descendantDoc in descendantDocs:
                for (auto& descendant_doc : descendant_docs->elements()) {
                    auto fullscreen_element = descendant_doc->fullscreen_element();

                    // 1. Append (fullscreenchange, descendantDoc's fullscreen element) to descendantDoc's list of
                    //    pending fullscreen events.
                    descendant_doc->append_pending_fullscreen_change(DOM::PendingFullscreenEvent::Type::Change, *fullscreen_element, fullscreen_element->fullscreen_request_type());

                    // 2. Unfullscreen descendantDoc.
                    descendant_doc->unfullscreen();
                }

                // 16. Resolve promise with undefined.
                if (exit.promise)
                    WebIDL::resolve_promise(*exit.promise);
                return true;
            });

        if (!processed)
            break;

        m_pending_fullscreen_operations.dequeue();
    }
}

void Page::set_viewport_is_fullscreen(ViewportIsFullscreen is_fullscreen)
{
    if (m_viewport_is_fullscreen == is_fullscreen)
        return;
    m_viewport_is_fullscreen = is_fullscreen;
    m_fullscreen_ipc_sent_to_ui = false;
    process_pending_fullscreen_operations();
}

void PageClient::history_navigation_params_creation_finished(HTML::CrossProcessId operation_id, HTML::HistoryNavigationPopulation population)
{
    page().history_executor().resume_history_navigation_population(operation_id, move(population));
}

void PageClient::request_navigation_start(HTML::LocalNavigable& navigable, NavigationTarget target, URL::URL const&, Utf16String navigation_id, Optional<HTML::NavigationStartRequest> start_request)
{
    // A javascript: navigation runs synchronously in this process and never populates an entry; there is nothing
    // for the embedder to retain.
    if (!start_request.has_value())
        return;

    navigable.run_navigation_unload_check(navigation_id, GC::create_function(navigable.heap(), [client = GC::Ref { *this }, navigable = GC::Ref { navigable }, target, navigation_id, start_request = start_request.release_value()](bool should_continue) mutable {
        if (!should_continue) {
            navigable->resume_navigation_params_creation(navigation_id, {});
            return;
        }
        auto population_request = HTML::create_navigation_population_request(move(start_request), client->allocate_cross_process_id());
        client->request_navigation_population(navigable, target, move(population_request));
    }));
}

void PageClient::request_navigation_population(HTML::LocalNavigable& navigable, NavigationTarget, HTML::NavigationPopulationRequest request)
{
    navigable.resume_navigation_params_creation(request.navigation_id, move(request));
}

void PageClient::navigation_params_creation_finished(HTML::LocalNavigable& navigable, HTML::NavigationPopulationRequest request, HTML::NavigationPopulationResult result)
{
    HTML::apply_navigation_population_result(request, result);
    navigable.continue_navigation_at_population(move(request), move(result));
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::Page::MediaContextMenu const& menu)
{
    TRY(encoder.encode(menu.media_url));
    TRY(encoder.encode(menu.is_video));
    TRY(encoder.encode(menu.is_playing));
    TRY(encoder.encode(menu.is_muted));
    TRY(encoder.encode(menu.has_user_agent_controls));
    TRY(encoder.encode(menu.is_looping));
    TRY(encoder.encode(menu.is_fullscreen));
    return {};
}

template<>
ErrorOr<Web::Page::MediaContextMenu> IPC::decode(Decoder& decoder)
{
    return Web::Page::MediaContextMenu {
        .media_url = TRY(decoder.decode<URL::URL>()),
        .is_video = TRY(decoder.decode<bool>()),
        .is_playing = TRY(decoder.decode<bool>()),
        .is_muted = TRY(decoder.decode<bool>()),
        .has_user_agent_controls = TRY(decoder.decode<bool>()),
        .is_looping = TRY(decoder.decode<bool>()),
        .is_fullscreen = TRY(decoder.decode<bool>()),
    };
}
