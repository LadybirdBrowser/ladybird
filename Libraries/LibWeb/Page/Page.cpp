/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/SourceLocation.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Selection/Selection.h>

namespace Web {

GC_DEFINE_ALLOCATOR(Page);

GC::Ref<Page> Page::create(JS::VM& vm, GC::Ref<PageClient> page_client)
{
    return vm.heap().allocate<Page>(page_client);
}

Page::Page(GC::Ref<PageClient> client)
    : m_client(client)
{
}

Page::~Page() = default;

void Page::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_top_level_traversable);
    visitor.visit(m_client);
    visitor.visit(m_window_rect_observer);
    visitor.visit(m_on_pending_dialog_closed);
}

HTML::Navigable& Page::focused_navigable()
{
    if (m_focused_navigable)
        return *m_focused_navigable;
    return top_level_traversable();
}

void Page::set_focused_navigable(Badge<EventHandler>, HTML::Navigable& navigable)
{
    m_focused_navigable = navigable;
}

void Page::navigable_document_destroyed(Badge<DOM::Document>, HTML::Navigable& navigable)
{
    if (&navigable == m_focused_navigable.ptr())
        m_focused_navigable.clear();
}

void Page::load(URL::URL const& url)
{
    (void)top_level_traversable()->navigate({ .url = url, .source_document = *top_level_traversable()->active_document(), .user_involvement = HTML::UserNavigationInvolvement::BrowserUI });
}

void Page::load_html(StringView html)
{
    // FIXME: #23909 Figure out why GC threshold does not stay low when repeatedly loading html from the WebView
    heap().collect_garbage();

    (void)top_level_traversable()->navigate({ .url = URL::about_srcdoc(),
        .source_document = *top_level_traversable()->active_document(),
        .document_resource = String::from_utf8(html).release_value_but_fixme_should_propagate_errors(),
        .user_involvement = HTML::UserNavigationInvolvement::BrowserUI });
}

void Page::reload()
{
    top_level_traversable()->reload();
}

void Page::traverse_the_history_by_delta(int delta)
{
    top_level_traversable()->traverse_the_history_by_delta(delta);
}

Gfx::Palette Page::palette() const
{
    return m_client->palette();
}

// https://w3c.github.io/csswg-drafts/cssom-view-1/#web-exposed-screen-area
CSSPixelRect Page::web_exposed_screen_area() const
{
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
    return m_client->preferred_color_scheme();
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

EventResult Page::handle_mouseup(DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers)
{
    return top_level_traversable()->event_handler().handle_mouseup(device_to_css_point(position), device_to_css_point(screen_position), button, buttons, modifiers);
}

EventResult Page::handle_mousedown(DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers)
{
    return top_level_traversable()->event_handler().handle_mousedown(device_to_css_point(position), device_to_css_point(screen_position), button, buttons, modifiers);
}

EventResult Page::handle_mousemove(DevicePixelPoint position, DevicePixelPoint screen_position, unsigned buttons, unsigned modifiers)
{
    return top_level_traversable()->event_handler().handle_mousemove(device_to_css_point(position), device_to_css_point(screen_position), buttons, modifiers);
}

EventResult Page::handle_mousewheel(DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, DevicePixels wheel_delta_x, DevicePixels wheel_delta_y)
{
    return top_level_traversable()->event_handler().handle_mousewheel(device_to_css_point(position), device_to_css_point(screen_position), button, buttons, modifiers, wheel_delta_x.value(), wheel_delta_y.value());
}

EventResult Page::handle_doubleclick(DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers)
{
    return top_level_traversable()->event_handler().handle_doubleclick(device_to_css_point(position), device_to_css_point(screen_position), button, buttons, modifiers);
}

EventResult Page::handle_drag_and_drop_event(DragEvent::Type type, DevicePixelPoint position, DevicePixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Vector<HTML::SelectedFile> files)
{
    return top_level_traversable()->event_handler().handle_drag_and_drop_event(type, device_to_css_point(position), device_to_css_point(screen_position), button, buttons, modifiers, move(files));
}

EventResult Page::handle_keydown(UIEvents::KeyCode key, unsigned modifiers, u32 code_point, bool repeat)
{
    return focused_navigable().event_handler().handle_keydown(key, modifiers, code_point, repeat);
}

EventResult Page::handle_keyup(UIEvents::KeyCode key, unsigned modifiers, u32 code_point, bool repeat)
{
    return focused_navigable().event_handler().handle_keyup(key, modifiers, code_point, repeat);
}

void Page::set_top_level_traversable(GC::Ref<HTML::TraversableNavigable> navigable)
{
    VERIFY(!m_top_level_traversable); // Replacement is not allowed!
    VERIFY(&navigable->page() == this);
    m_top_level_traversable = navigable;
}

bool Page::top_level_traversable_is_initialized() const
{
    return m_top_level_traversable;
}

HTML::BrowsingContext& Page::top_level_browsing_context()
{
    return *m_top_level_traversable->active_browsing_context();
}

HTML::BrowsingContext const& Page::top_level_browsing_context() const
{
    return *m_top_level_traversable->active_browsing_context();
}

GC::Ref<HTML::TraversableNavigable> Page::top_level_traversable() const
{
    return *m_top_level_traversable;
}

void Page::did_update_window_rect()
{
    if (m_window_rect_observer)
        m_window_rect_observer->function()({ window_position(), window_size() });
}

template<typename ResponseType>
static ResponseType spin_event_loop_until_dialog_closed(PageClient& client, Optional<ResponseType>& response, SourceLocation location = SourceLocation::current())
{
    auto& event_loop = Web::HTML::current_principal_settings_object().responsible_event_loop();
    auto pause_handle = event_loop.pause();

    Web::Platform::EventLoopPlugin::the().spin_until(GC::create_function(event_loop.heap(), [&]() {
        return response.has_value() || !client.is_connection_open();
    }));

    if (!client.is_connection_open()) {
        dbgln("WebContent client disconnected during {}. Exiting peacefully.", location.function_name());
        exit(0);
    }

    return response.release_value();
}

void Page::did_request_alert(String const& message)
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

bool Page::did_request_confirm(String const& message)
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

Optional<String> Page::did_request_prompt(String const& message, String const& default_)
{
    m_pending_dialog = PendingDialog::Prompt;
    m_client->page_did_request_prompt(message, default_);

    if (!message.is_empty())
        m_pending_dialog_text = message;

    return spin_event_loop_until_dialog_closed(*m_client, m_pending_prompt_response);
}

void Page::prompt_closed(Optional<String> response)
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

void Page::did_request_color_picker(WeakPtr<HTML::HTMLInputElement> target, Color current_color)
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
                m_pending_non_blocking_dialog_target.clear();
        }
    }
}

void Page::did_request_file_picker(WeakPtr<HTML::HTMLInputElement> target, HTML::FileFilter const& accepted_file_types, HTML::AllowMultipleFiles allow_multiple_files)
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

            m_pending_non_blocking_dialog_target.clear();
        }
    }
}

void Page::did_request_select_dropdown(WeakPtr<HTML::HTMLSelectElement> target, Web::CSSPixelPoint content_position, Web::CSSPixels minimum_width, Vector<Web::HTML::SelectItem> items)
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
            m_pending_non_blocking_dialog_target.clear();
        }
    }
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

void Page::did_request_media_context_menu(UniqueNodeID media_id, CSSPixelPoint position, ByteString const& target, unsigned modifiers, MediaContextMenu const& menu)
{
    m_media_context_menu_element_id = media_id;
    client().page_did_request_media_context_menu(position, target, modifiers, menu);
}

WebIDL::ExceptionOr<void> Page::toggle_media_play_state()
{
    auto media_element = media_context_menu_element();
    if (!media_element)
        return {};

    // AD-HOC: An execution context is required for Promise creation hooks.
    HTML::TemporaryExecutionContext execution_context { media_element->realm() };

    if (media_element->potentially_playing())
        TRY(media_element->pause());
    else
        TRY(media_element->play());

    return {};
}

void Page::toggle_media_mute_state()
{
    auto media_element = media_context_menu_element();
    if (!media_element)
        return;

    // AD-HOC: An execution context is required for Promise creation hooks.
    HTML::TemporaryExecutionContext execution_context { media_element->realm() };

    media_element->set_muted(!media_element->muted());
}

WebIDL::ExceptionOr<void> Page::toggle_media_loop_state()
{
    auto media_element = media_context_menu_element();
    if (!media_element)
        return {};

    // AD-HOC: An execution context is required for Promise creation hooks.
    HTML::TemporaryExecutionContext execution_context { media_element->realm() };

    if (media_element->has_attribute(HTML::AttributeNames::loop))
        media_element->remove_attribute(HTML::AttributeNames::loop);
    else
        TRY(media_element->set_attribute(HTML::AttributeNames::loop, {}));

    return {};
}

WebIDL::ExceptionOr<void> Page::toggle_media_controls_state()
{
    auto media_element = media_context_menu_element();
    if (!media_element)
        return {};

    HTML::TemporaryExecutionContext execution_context { media_element->realm() };

    if (media_element->has_attribute(HTML::AttributeNames::controls))
        media_element->remove_attribute(HTML::AttributeNames::controls);
    else
        TRY(media_element->set_attribute(HTML::AttributeNames::controls, {}));

    return {};
}

void Page::toggle_page_mute_state()
{
    m_mute_state = HTML::invert_mute_state(m_mute_state);

    for (auto media_id : m_media_elements) {
        if (auto* node = DOM::Node::from_unique_id(media_id)) {
            auto& media_element = as<HTML::HTMLMediaElement>(*node);
            media_element.page_mute_state_changed({});
        }
    }
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

void Page::set_user_style(String source)
{
    m_user_style_sheet_source = source;
    if (top_level_traversable_is_initialized() && top_level_traversable()->active_document()) {
        top_level_traversable()->active_document()->style_computer().invalidate_rule_cache();
    }
}

Vector<GC::Root<DOM::Document>> Page::documents_in_active_window() const
{
    if (!top_level_traversable_is_initialized())
        return {};

    auto documents = HTML::main_thread_event_loop().documents_in_this_event_loop_matching([&](auto& document) {
        return document.window() == top_level_traversable()->active_window();
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
    VERIFY(top_level_traversable_is_initialized());

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
        if (document == top_level_traversable()->active_document()) {
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

    if (auto active_document = top_level_traversable()->active_document()) {
        if (m_last_find_in_page_url.serialize(URL::ExcludeFragment::Yes) != active_document->url().serialize(URL::ExcludeFragment::Yes)) {
            m_last_find_in_page_url = top_level_traversable()->active_document()->url();
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

    update_find_in_page_selection(all_matches);

    return Page::FindInPageResult {
        .current_match_index = m_find_in_page_match_index,
        .total_match_count = all_matches.size(),
    };
}

Page::FindInPageResult Page::find_in_page(FindInPageQuery const& query)
{
    if (!top_level_traversable_is_initialized())
        return {};

    if (query.string.is_empty()) {
        m_last_find_in_page_query = {};
        clear_selection();
        return {};
    }

    auto result = perform_find_in_page_query(query);

    m_last_find_in_page_query = query;
    m_last_find_in_page_url = top_level_traversable()->active_document()->url();

    return result;
}

Page::FindInPageResult Page::find_in_page_next_match()
{
    if (!(m_last_find_in_page_query.has_value() && top_level_traversable_is_initialized()))
        return {};

    auto result = perform_find_in_page_query(*m_last_find_in_page_query, SearchDirection::Forward);
    return result;
}

Page::FindInPageResult Page::find_in_page_previous_match()
{
    if (!(m_last_find_in_page_query.has_value() && top_level_traversable_is_initialized()))
        return {};

    auto result = perform_find_in_page_query(*m_last_find_in_page_query, SearchDirection::Backward);
    return result;
}

void Page::update_find_in_page_selection(Vector<GC::Root<DOM::Range>> matches)
{
    clear_selection();

    if (matches.is_empty())
        return;

    auto current_range = matches[m_find_in_page_match_index];
    auto common_ancestor_container = current_range->common_ancestor_container();
    auto& document = common_ancestor_container->document();
    if (!document.window())
        return;

    auto selection = document.get_selection();
    if (!selection)
        return;

    selection->add_range(*current_range);

    if (auto* element = common_ancestor_container->parent_element()) {
        DOM::ScrollIntoViewOptions scroll_options;
        scroll_options.block = Bindings::ScrollLogicalPosition::Nearest;
        scroll_options.inline_ = Bindings::ScrollLogicalPosition::Nearest;
        scroll_options.behavior = Bindings::ScrollBehavior::Instant;
        (void)element->scroll_into_view(scroll_options);
    }
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
    };
}
