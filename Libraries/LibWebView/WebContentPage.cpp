/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibCore/EventLoop.h>
#include <LibDevTools/StorageHelpers.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/SiteIsolation.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/SourceHighlighter.h>
#include <LibWebView/StorageJar.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI.h>
#include <LibWebView/WorkerProcessManager.h>

namespace WebView {

static JsonObject parse_json(StringView json, StringView name)
{
    auto parsed_tree = JsonValue::from_string(json);
    if (parsed_tree.is_error()) {
        dbgln("Unable to parse {}: {}", name, parsed_tree.error());
        return {};
    }

    if (!parsed_tree.value().is_object()) {
        dbgln("Expected {} to be an object: {}", name, parsed_tree.value());
        return {};
    }

    return move(parsed_tree.release_value().as_object());
}

static JsonArray parse_json_array(StringView json, StringView name)
{
    auto parsed_tree = JsonValue::from_string(json);
    if (parsed_tree.is_error()) {
        dbgln("Unable to parse {}: {}", name, parsed_tree.error());
        return {};
    }

    if (!parsed_tree.value().is_array()) {
        dbgln("Expected {} to be an array: {}", name, parsed_tree.value());
        return {};
    }

    return move(parsed_tree.release_value().as_array());
}

static Optional<JsonObject> parse_optional_json_object(StringView json, StringView name)
{
    auto parsed_tree = JsonValue::from_string(json);
    if (parsed_tree.is_error()) {
        dbgln("Unable to parse {}: {}", name, parsed_tree.error());
        return {};
    }

    if (parsed_tree.value().is_null())
        return {};

    if (!parsed_tree.value().is_object()) {
        dbgln("Expected {} to be an object or null: {}", name, parsed_tree.value());
        return {};
    }

    return move(parsed_tree.release_value().as_object());
}

static ErrorOr<Vector<DevTools::DevToolsDelegate::StorageItem>> parse_storage_items(String const& storage_items)
{
    auto parsed_items = JsonValue::from_string(storage_items);
    if (parsed_items.is_error())
        return Error::from_string_literal("Unable to parse storage items");

    if (!parsed_items.value().is_array())
        return Error::from_string_literal("Expected storage items to be an array");

    Vector<DevTools::DevToolsDelegate::StorageItem> items;
    parsed_items.value().as_array().for_each([&](auto const& item) {
        if (!item.is_object())
            return;

        auto name = item.as_object().get_string("name"sv);
        auto value = item.as_object().get_string("value"sv);
        if (!name.has_value() || !value.has_value())
            return;

        items.append({ name.release_value(), value.release_value() });
    });
    return items;
}

static Optional<LexicalPath> choose_download_destination_or_report_error(URL::URL const& url, ByteString const& suggested_filename)
{
    auto destination = Application::the().default_path_for_downloaded_file(suggested_filename);
    if (destination.is_error()) {
        if (!destination.error().is_errno() || destination.error().code() != ECANCELED)
            Application::the().display_error_dialog(ByteString::formatted("Unable to download {}: {}", url, destination.error()));
        return {};
    }

    return destination.release_value();
}

static bool is_download_in_progress(FileDownloader const& file_downloader, u64 download_id)
{
    auto download = file_downloader.download(download_id);
    return download.has_value() && download->status == FileDownloader::DownloadStatus::InProgress;
}

static Optional<String> history_title(Utf16String const& title, URL::URL const& url)
{
    if (title.is_empty())
        return {};

    auto title_utf8 = title.to_utf8();
    if (title_utf8 == url.serialize() || title_utf8 == url.serialize(URL::ExcludeFragment::Yes))
        return {};

    return title_utf8;
}

WebContentPage::WebContentPage(WebContentClient& client, Web::PageId id, CanonicalTraversable& traversable)
    : m_client(client)
    , m_id(id)
    , m_traversable(traversable.make_weak_ptr<CanonicalTraversable>())
{
}

WebContentPage::~WebContentPage() = default;

WebContentClient& WebContentPage::client() const
{
    VERIFY(m_client);
    return *m_client;
}

bool WebContentPage::is_live() const
{
    return m_client && m_client->is_page_open(m_id);
}

CanonicalTraversable& WebContentPage::traversable() const
{
    VERIFY(is_open());
    return *m_traversable;
}

ViewImplementation& WebContentPage::view() const
{
    auto view = traversable().view();
    VERIFY(view.has_value());
    return *view;
}

bool WebContentPage::displays_tab() const
{
    return traversable().display_page() == this;
}

Optional<CanonicalNavigable&> WebContentPage::hosted_navigable(Web::HTML::CrossProcessId navigable_id) const
{
    auto navigable = traversable().find(navigable_id);
    if (!navigable.has_value() || !traversable().hosts(*navigable, *this))
        return {};
    return *navigable;
}

RefPtr<WebContentPage> WebContentPage::endpoint_hosting_navigable_represented_by(Web::HTML::CrossProcessId navigable_id) const
{
    auto target = traversable().find(navigable_id);
    if (!target.has_value() || traversable().hosts(*target, *this))
        return {};
    auto endpoint = traversable().page_hosting(*target);
    if (!endpoint || !endpoint->is_open())
        return {};
    return endpoint;
}

bool WebContentPage::continue_navigation_population_in_selected_process(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id)
{
    // The population steps up to the response ran in the population worker, which delivered the result here.
    auto navigable = population_worker_navigable(navigable_id);
    if (!navigable.has_value())
        return false;

    auto& ongoing_navigation = navigable->ongoing_navigation();
    if (!ongoing_navigation.has_value()
        || ongoing_navigation->navigation_id != navigation_id
        || ongoing_navigation->phase != CanonicalNavigable::OngoingNavigation::Phase::AwaitingResponseBody
        || !ongoing_navigation->loader) {
        return false;
    }

    auto& loader = *ongoing_navigation->loader;
    ongoing_navigation->phase = CanonicalNavigable::OngoingNavigation::Phase::Populating;

    auto populate_in = [&](WebContentPage& host) {
        navigable->set_navigation_host(host);
        host.async_populate_navigation(loader.request(), loader.take_result());
        return true;
    };

    // The task queued by step 5 of attempting to populate the history entry's document runs in the process hosting
    // the browsing context that the document it creates belongs to. A response that creates no document is finished
    // by the process that fetched it.
    auto document = loader.response_document();
    if (!document.has_value())
        return populate_in(*this);

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#initialise-the-document-object
    // 1. Let browsingContext be the result of obtaining a browsing context to use for a navigation response given navigationParams.
    auto browsing_context = navigable->obtain_a_browsing_context_to_use_for_a_navigation_response(document->coop_enforcement_result);
    auto browsing_context_group_switch = browsing_context.ptr() != &navigable->active_browsing_context();

    if (navigable->is_top_level_traversable()) {
        auto& traversable = navigable->top_level_traversable();
        auto site_isolation_process_swap = SiteIsolationManager::the().top_level_navigation_requires_process_swap(
            traversable.active_browsing_context(),
            traversable.replicated_state()->active_document_url,
            document->url);
        if (!browsing_context_group_switch && !site_isolation_process_swap)
            return populate_in(*this);

        if (!displays_tab()) {
            navigable->clear_ongoing_navigation();
            return false;
        }
        if (browsing_context_group_switch)
            ongoing_navigation->destination_browsing_context = move(browsing_context);
        return view().create_new_process_for_cross_site_navigation(navigation_id);
    }

    // A child navigable's document is created in the process hosting the agent cluster of the document's origin
    // within the browsing context group. Without iframe isolation, every agent cluster of a child's document is
    // hosted by its container document's process.
    auto browsing_context_group = navigable->top_level_traversable().active_browsing_context().group();
    VERIFY(browsing_context_group);
    if (site_isolation_mode() != SiteIsolationMode::IFrame)
        return populate_in(*this);

    // A document created for inline content stands in for the resource the process that fetched could not load, in
    // an agent cluster of its own; that process hosts it.
    if (document->is_inline_content)
        return populate_in(*this);

    // FIXME: Pass the document's requestsOAC value once Origin-Agent-Cluster is implemented.
    auto agent = browsing_context_group->obtain_similar_origin_window_agent(document->origin, false);
    SiteIsolationManager::the().host_opaque_origin_agent_with_initiator(*browsing_context_group, *agent, document->origin, loader.request().history_entry.document_state.initiator_origin);

    auto host_or_error = SiteIsolationManager::the().obtain_child_document_host(*navigable, *agent);
    if (host_or_error.is_error()) {
        warnln("Unable to create WebContent page for child frame navigation: {}", host_or_error.error());
        navigable->clear_ongoing_navigation();
        return false;
    }
    // The host takes the container over when the document is activated, after the displayed document is unloaded.
    auto host = host_or_error.release_value();
    return populate_in(host);
}

// A navigation's population steps run in the process recorded as its population worker at admission, which is
// the process with the live source document. That is not necessarily the process hosting the target's document.
Optional<CanonicalNavigable&> WebContentPage::population_worker_navigable(Web::HTML::CrossProcessId navigable_id) const
{
    if (auto navigable = hosted_navigable(navigable_id); navigable.has_value())
        return navigable;

    auto navigable = traversable().top_level_traversable().find(navigable_id);
    if (!navigable.has_value() || !navigable->navigation_population_worker_matches(*this))
        return {};
    return *navigable;
}

StorageJar* WebContentPage::storage_jar(Web::StorageAPI::StorageEndpointType storage_endpoint) const
{
    if (storage_endpoint == Web::StorageAPI::StorageEndpointType::SessionStorage)
        return &traversable().top_level_traversable().session_storage();
    return client().session().storage_jar.ptr();
}

// A position a page sends is in the viewport of a local root it hosts, which the tab's view places in its own.
Optional<WebContentPage::ViewPosition> WebContentPage::view_position(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint position) const
{
    auto local_root = hosted_navigable(local_root_id);
    if (!local_root.has_value())
        return {};
    position.translate_by(view().traversable().local_root_offset(*local_root).to_type<int>());
    return ViewPosition { view(), position };
}

// A dialog blocks the whole tab, so every other page of the tab is told of the one a document of this page opened.
void WebContentPage::did_open_dialog(Web::Page::PendingDialog dialog, Utf16String const& message)
{
    traversable().for_each_hosting_page([&](WebContentPage& page) {
        if (&page != this)
            page.async_did_open_dialog_in_another_process(dialog, message);
    });
}

void WebContentPage::maybe_record_history_visit_for_current_load(URL::URL const& url, Optional<String> title, StringView reason)
{
    auto normalized_url = HistoryStore::normalize_url(url);
    if (!normalized_url.has_value())
        return;

    if (m_history_recorded_url_for_current_load == *normalized_url) {
        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Visit for page {} at '{}' was already recorded during this load before {}", m_id, *normalized_url, reason);
        return;
    }

    dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Recording history visit for page {} at '{}' after {}", m_id, *normalized_url, reason);

    // Title and favicon updates already give us a useful history entry, so
    // do not wait for did_finish_loading() on pages that never reach it.
    auto transition = HistoryVisitTransition::Link;
    if (displays_tab())
        transition = view().m_history_visit_transition_for_current_load;
    client().session().history_store->record_visit(url, move(title), UnixDateTime::now(), transition);
    m_history_recorded_url_for_current_load = normalized_url.release_value();
}

void WebContentPage::begin_top_level_load(Optional<Utf16String> navigation_id, URL::URL const& url)
{
    if (auto process = WebView::Application::the().find_process(client().pid()); process.has_value())
        process->set_title(OptionalNone {});

    m_history_recorded_url_for_current_load.clear();

    auto& view = this->view();
    view.m_history_visit_transition_for_current_load = view.m_history_visit_transition_for_next_load;
    view.m_history_visit_transition_for_next_load = HistoryVisitTransition::Link;
    view.did_start_navigation(move(navigation_id), url);

    view.set_url({}, url);
    view.set_title({}, Utf16String::from_utf8(url.serialize()));
    view.set_favicon({}, {});
    view.set_editing_history_state(false, false);

    if (view.on_load_start)
        view.on_load_start();

    for (auto const& [id, listener] : view.m_navigation_listeners) {
        if (listener.on_load_start)
            listener.on_load_start(url);
    }
}

void WebContentPage::close()
{
    m_is_open = false;
    m_traversable = nullptr;
    m_needs_beforeunload_check = true;
    m_history_recorded_url_for_current_load.clear();
}

void WebContentPage::did_request_navigation_of_navigable(Web::HTML::CrossProcessId navigable_id, Web::HTML::PreparedNavigationDescriptor navigation)
{
    // The request continues navigate at step 8 in the process hosting the target's document.
    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_navigate_navigable(navigable_id, move(navigation));
}

void WebContentPage::did_post_message_to_navigable(Web::HTML::CrossProcessId navigable_id, Web::HTML::PostedMessageDescriptor message)
{
    // The window post message steps queue their task on the target window in the process hosting its document.
    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_deliver_posted_message(navigable_id, move(message));
}

void WebContentPage::did_request_focusing_steps_for_navigable(Web::HTML::CrossProcessId navigable_id, Web::HTML::FocusTrigger focus_trigger)
{
    // The focusing steps for a navigable container go on in the process hosting its content navigable's document.
    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_run_focusing_steps_for_navigable(navigable_id, focus_trigger);
}

void WebContentPage::did_request_window_focus_of_navigable(Web::HTML::CrossProcessId navigable_id)
{
    // window.focus() on a window another process hosts runs there.
    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_focus_window_of_navigable(navigable_id);
}

void WebContentPage::did_request_set_opener_of_navigable(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId opener_navigable_id)
{
    // window.open() on a navigable another process hosts sets the opener of its active browsing context there, to that
    // of a navigable the requesting page hosts.
    if (!hosted_navigable(opener_navigable_id).has_value())
        return;

    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_set_opener_of_navigable(navigable_id, opener_navigable_id);
}

void WebContentPage::did_completely_finish_loading(Web::HTML::CrossProcessId navigable_id)
{
    // Only the process hosting a navigable's active document speaks for it.
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;
    navigable->active_document_completely_finished_loading();
}

void WebContentPage::did_create_child_frame(Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, Web::HTML::ReplicatedNavigableState replicated_state)
{
    auto& host = this->traversable();
    auto& traversable = host.top_level_traversable();

    // A process materializing a frame that exists re-hosts its document. The canonical navigable's browsing context
    // stays as it is.
    if (auto existing_navigable = traversable.find(frame_id); existing_navigable.has_value()) {
        traversable.insert(*this, move(parent_frame_id), move(frame_id), move(replicated_state), existing_navigable->active_browsing_context(), host);
        return;
    }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-new-child-navigable
    // 2. Let group be element's node document's browsing context's top-level browsing context's group.
    auto group = traversable.browsing_context_for_document_creation(*this).group();
    VERIFY(group);

    // 3. Let browsingContext and document be the result of creating a new browsing context and document given element's node document, element, and group.
    auto browsing_context = CanonicalBrowsingContext::create_a_new_browsing_context_and_document(*group, replicated_state.active_document_origin, client());

    // 6. Let documentState be a new document state, with [...]
    // 7. Let navigable be a new navigable.
    // 8. Initialize the navigable navigable given documentState and parentNavigable.
    traversable.insert(*this, move(parent_frame_id), move(frame_id), move(replicated_state), move(browsing_context), host);
}

void WebContentPage::did_set_browser_zoom(double factor)
{
    view().set_zoom(factor);
}

void WebContentPage::did_find_in_page(size_t current_match_index, Optional<size_t> total_match_count)
{
    if (displays_tab()) {
        if (view().on_find_in_page)
            view().on_find_in_page(current_match_index, total_match_count);
    }
}

void WebContentPage::did_request_refresh()
{
    if (displays_tab())
        view().reload();
}

void WebContentPage::did_request_cursor_change(Gfx::Cursor cursor)
{
    view().did_request_cursor_change({}, move(cursor));
}

void WebContentPage::did_update_editing_history_state(bool can_undo, bool can_redo)
{
    // Undo and redo go to the page hosting the tab's focused navigable.
    auto host = view().traversable().focused_navigable_host();
    if (host != *this)
        return;
    view().set_editing_history_state(can_undo, can_redo);
}

void WebContentPage::did_request_tooltip_override(Gfx::IntPoint position, ByteString title)
{
    if (displays_tab()) {
        if (view().on_request_tooltip_override)
            view().on_request_tooltip_override(view().to_widget_position(position), title);
    }
}

void WebContentPage::did_stop_tooltip_override()
{
    if (view().on_stop_tooltip_override)
        view().on_stop_tooltip_override();
}

void WebContentPage::did_enter_tooltip_area(ByteString title)
{
    if (view().on_enter_tooltip_area)
        view().on_enter_tooltip_area(title);
}

void WebContentPage::did_leave_tooltip_area()
{
    if (view().on_leave_tooltip_area)
        view().on_leave_tooltip_area();
}

void WebContentPage::did_hover_link(URL::URL url)
{
    if (view().on_link_hover)
        view().on_link_hover(url);
}

void WebContentPage::did_unhover_link()
{
    if (view().on_link_unhover)
        view().on_link_unhover();
}

void WebContentPage::did_click_link(URL::URL url, ByteString target, unsigned modifiers)
{
    auto open_in_background = modifiers == Web::UIEvents::Mod_PlatformCtrl;
    auto open_in_foreground = modifiers == (Web::UIEvents::Mod_PlatformCtrl | Web::UIEvents::Mod_Shift);
    if (open_in_background || open_in_foreground || target == "_blank"sv) {
        view().open_url_in_new_tab(url, open_in_background ? Web::HTML::ActivateTab::No : Web::HTML::ActivateTab::Yes);
    } else {
        view().load(url);
    }
}

void WebContentPage::did_middle_click_link(URL::URL url, ByteString, unsigned)
{
    view().open_url_in_new_tab(url, Web::HTML::ActivateTab::No);
}

void WebContentPage::did_request_external_url(URL::URL url, URL::Origin initiator_origin, bool has_transient_activation)
{
    view().handle_external_url({}, move(url), move(initiator_origin), has_transient_activation);
}

void WebContentPage::did_inspect_storage(u64 request_id, String storage_items)
{
    if (displays_tab()) {
        auto handler = view().on_received_storage_items.take(request_id);
        if (handler.has_value())
            (*handler)(parse_storage_items(storage_items));
    }
}

void WebContentPage::did_inspect_grid_layouts(String grid_layouts)
{
    if (displays_tab()) {
        if (view().on_received_grid_layouts)
            view().on_received_grid_layouts(parse_json_array(grid_layouts, "grid layouts"sv));
    }
}

void WebContentPage::did_inspect_current_grid(String grid_layout)
{
    if (displays_tab()) {
        if (view().on_received_current_grid)
            view().on_received_current_grid(parse_optional_json_object(grid_layout, "current grid"sv));
    }
}

void WebContentPage::did_inspect_current_flexbox(String flexbox_layout)
{
    if (displays_tab()) {
        if (view().on_received_current_flexbox)
            view().on_received_current_flexbox(parse_optional_json_object(flexbox_layout, "current flexbox"sv));
    }
}

void WebContentPage::did_inspect_indexed_database(u64 request_id, String result)
{
    if (displays_tab())
        view().did_receive_indexed_database_inspection(request_id, parse_json(result, "IndexedDB inspection result"sv));
}

void WebContentPage::did_inspect_accessibility_tree(String accessibility_tree)
{
    if (displays_tab()) {
        if (view().on_received_accessibility_tree)
            view().on_received_accessibility_tree(parse_json(accessibility_tree, "accessibility tree"sv));
    }
}

void WebContentPage::did_get_hovered_node_id(Web::UniqueNodeID node_id)
{
    if (displays_tab()) {
        if (view().on_received_hovered_node_id)
            view().on_received_hovered_node_id(node_id);
    }
}

void WebContentPage::did_get_node_id_at_position(u64 request_id, Web::UniqueNodeID node_id)
{
    if (displays_tab()) {
        view().did_receive_node_picker_hit_test(request_id, node_id);
    }
}

void WebContentPage::did_list_style_sheets(Vector<Web::CSS::StyleSheetIdentifier> stylesheets)
{
    if (displays_tab()) {
        if (view().on_received_style_sheet_list)
            view().on_received_style_sheet_list(stylesheets);
    }
}

void WebContentPage::did_get_style_sheet_source(Web::CSS::StyleSheetIdentifier identifier, URL::URL base_url, Utf16String source)
{
    if (displays_tab()) {
        if (view().on_received_style_sheet_source)
            view().on_received_style_sheet_source(identifier, base_url, source);
    }
}

void WebContentPage::did_list_devtools_sources(u64 request_id, Vector<Web::HTML::ScriptRegistry::Description> sources)
{
    if (displays_tab()) {
        auto handler = view().on_received_devtools_sources.take(request_id);
        if (handler.has_value())
            (*handler)(move(sources));
    }
}

void WebContentPage::did_get_devtools_source(Web::HTML::ScriptRegistry::Identifier source_id, Optional<Web::HTML::ScriptRegistry::Content> source)
{
    if (displays_tab()) {
        auto handler = view().on_received_devtools_source.take(source_id);
        if (handler.has_value())
            (*handler)(move(source));
    }
}

void WebContentPage::did_add_devtools_source(Web::HTML::ScriptRegistry::Description source)
{
    if (displays_tab()) {
        if (view().on_devtools_source_available)
            view().on_devtools_source_available(move(source));
    }
}

void WebContentPage::did_pause_debugger(DebuggerPause pause)
{
    if (displays_tab()) {
        view().did_pause_debugger({});
        if (view().on_debugger_paused)
            view().on_debugger_paused(move(pause));
    }
}

void WebContentPage::did_resume_debugger()
{
    if (displays_tab())
        view().did_resume_debugger({});
}

void WebContentPage::did_complete_debugger_breakpoint_operation(u64 request_id, Optional<String> error)
{
    if (displays_tab())
        view().did_complete_debugger_breakpoint_operation(request_id, move(error));
}

void WebContentPage::did_take_screenshot(Gfx::ShareableBitmap screenshot)
{
    if (displays_tab())
        view().did_receive_screenshot({}, screenshot);
}

void WebContentPage::did_get_internal_page_info(WebView::PageInfoType type, Optional<Core::AnonymousBuffer> info)
{
    if (displays_tab())
        view().did_receive_internal_page_info({}, type, info);
}

void WebContentPage::did_get_selected_text(u64 request_id, ByteString selection)
{
    view().did_receive_selected_text({}, request_id, move(selection));
}

void WebContentPage::did_get_selected_text_for_lookup(u64 request_id, Optional<DictionaryLookup> lookup)
{

    // The page hosting the tab's focused navigable gives the baseline origin in the viewport of its local root.
    if (lookup.has_value() && lookup->baseline_origin.has_value())
        lookup->baseline_origin->translate_by(view().traversable().focused_navigable_host_offset().to_type<int>());
    view().did_receive_selected_text_for_lookup({}, request_id, move(lookup));
}

void WebContentPage::did_select_word_for_dictionary_lookup(u64 request_id, bool selected)
{
    view().did_select_word_for_dictionary_lookup({}, request_id, selected);
}

void WebContentPage::did_cut_selected_text(u64 request_id, ByteString selection)
{
    view().did_cut_selected_text({}, request_id, move(selection));
}

void WebContentPage::did_execute_js_console_input(JsonValue result)
{
    if (displays_tab()) {
        if (view().on_received_js_console_result)
            view().on_received_js_console_result(move(result));
    }
}

void WebContentPage::did_output_js_console_message(ConsoleOutput console_output)
{
    if (displays_tab()) {
        if (view().on_console_message)
            view().on_console_message(move(console_output));
    }
}

void WebContentPage::did_start_network_request(u64 request_id, URL::URL url, ByteString method, Vector<HTTP::Header> request_headers, ByteBuffer request_body, Optional<String> initiator_type, String referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::Request::Priority priority)
{
    if (displays_tab()) {
        if (view().on_network_request_started)
            view().on_network_request_started(request_id, url, method, request_headers, move(request_body), move(initiator_type), move(referrer_policy), is_navigation_request, priority);
    }
}

void WebContentPage::did_receive_network_response_body(u64 request_id, ByteBuffer data)
{
    if (displays_tab()) {
        if (view().on_network_response_body_received)
            view().on_network_response_body_received(request_id, move(data));
    }
}

void WebContentPage::did_finish_network_request(u64 request_id, u64 body_size, Requests::RequestTimingInfo timing_info, Optional<Requests::NetworkError> network_error)
{
    if (displays_tab()) {
        if (view().on_network_request_finished)
            view().on_network_request_finished(request_id, body_size, timing_info, network_error);
    }
}

void WebContentPage::did_request_set_prompt_text(Utf16String message)
{
    if (view().on_request_set_prompt_text)
        view().on_request_set_prompt_text(message);
}

void WebContentPage::did_request_accept_dialog()
{
    if (view().on_request_accept_dialog)
        view().on_request_accept_dialog();
}

void WebContentPage::did_request_dismiss_dialog()
{
    if (view().on_request_dismiss_dialog)
        view().on_request_dismiss_dialog();
}

void WebContentPage::did_request_document_cookie_version_index(i64 document_id, String domain)
{
    if (displays_tab()) {
        if (auto document_index = view().ensure_document_cookie_version_index({}, domain); !document_index.is_error())
            async_set_document_cookie_version_index(document_id, document_index.value());
    }
}

Messages::WebContentClient::DidRequestStorageItemResponse WebContentPage::did_request_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key)
{
    auto* storage_jar = this->storage_jar(storage_endpoint);
    if (!storage_jar)
        return Optional<Utf16String> {};
    return storage_jar->get_item(storage_endpoint, storage_key, bottle_key);
}

Messages::WebContentClient::DidSetStorageItemResponse WebContentPage::did_set_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key, Utf16String value)
{
    auto* storage_jar = this->storage_jar(storage_endpoint);
    if (!storage_jar)
        return WebView::StorageOperationError::QuotaExceededError;
    return storage_jar->set_item(storage_endpoint, storage_key, bottle_key, value);
}

void WebContentPage::did_remove_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key, Utf16String bottle_key)
{
    if (auto* storage_jar = this->storage_jar(storage_endpoint))
        storage_jar->remove_item(storage_endpoint, storage_key, bottle_key);
}

Messages::WebContentClient::DidRequestStorageKeysResponse WebContentPage::did_request_storage_keys(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key)
{
    auto* storage_jar = this->storage_jar(storage_endpoint);
    if (!storage_jar)
        return Vector<Utf16String> {};
    return storage_jar->get_all_keys(storage_endpoint, storage_key);
}

void WebContentPage::did_clear_storage(Web::StorageAPI::StorageEndpointType storage_endpoint, String storage_key)
{
    if (auto* storage_jar = this->storage_jar(storage_endpoint))
        storage_jar->clear_storage_key(storage_endpoint, storage_key);
}

void WebContentPage::did_request_activate_tab()
{
    if (view().on_activate_tab)
        view().on_activate_tab();
}

void WebContentPage::did_change_needs_beforeunload_check(bool needs_beforeunload_check)
{
    m_needs_beforeunload_check = needs_beforeunload_check;
}

void WebContentPage::did_consume_user_activation(Web::HTML::UserActivationConsumption consumption)
{
    auto& page_host = this->traversable();
    page_host.top_level_traversable().for_each_hosting_page([&](WebContentPage& page) {
        if (&page == this)
            return;
        page.async_consume_user_activation(consumption);
    });
}

void WebContentPage::webdriver_user_prompt_handling_complete(u64 request_id, Web::WebDriver::Response response)
{
    if (displays_tab())
        view().did_complete_webdriver_user_prompt_handling({}, request_id, move(response));
}

void WebContentPage::webdriver_did_set_current_browsing_context(u64 command_id, Web::HTML::CrossProcessId navigable_id)
{
    view().did_set_webdriver_current_browsing_context({}, command_id, navigable_id);
}

void WebContentPage::webdriver_command_complete(u64 command_id, Web::WebDriver::Response response)
{
    view().did_complete_webdriver_content_command({}, command_id, move(response));
}

void WebContentPage::did_update_resource_count(i32 count_waiting)
{
    if (displays_tab()) {
        if (view().on_resource_status_change)
            view().on_resource_status_change(count_waiting);
    }
}

void WebContentPage::did_request_restore_window()
{
    if (view().on_restore_window)
        view().on_restore_window();
}

void WebContentPage::did_request_reposition_window(Gfx::IntPoint position, u64 completion_id)
{
    if (view().on_reposition_window)
        view().on_reposition_window(position);
    async_did_complete_window_rect_request(completion_id);
}

void WebContentPage::did_request_resize_window(Gfx::IntSize size, u64 completion_id)
{
    if (view().on_resize_window)
        view().on_resize_window(size);
    async_did_complete_window_rect_request(completion_id);
}

void WebContentPage::did_request_maximize_window(u64 completion_id)
{
    if (view().on_maximize_window)
        view().on_maximize_window();
    async_did_complete_window_rect_request(completion_id);
}

void WebContentPage::did_request_minimize_window()
{
    if (view().on_minimize_window)
        view().on_minimize_window();
}

void WebContentPage::did_request_fullscreen_window()
{
    if (view().on_fullscreen_window)
        view().on_fullscreen_window();
}

void WebContentPage::did_request_exit_fullscreen()
{
    if (view().on_exit_fullscreen_window)
        view().on_exit_fullscreen_window();
}

void WebContentPage::did_request_file(ByteString path, i32 request_id)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        async_handle_file_return(file.error().code(), {}, request_id);
    else
        async_handle_file_return(0, IPC::File::adopt_file(file.release_value()), request_id);
}

void WebContentPage::did_request_color_picker(Color current_color)
{
    view().did_request_color_picker({}, *this, current_color);
}

void WebContentPage::did_request_geolocation_position(u64 request_id)
{
    if (view().on_request_geolocation_position)
        view().on_request_geolocation_position(*this, request_id);
}

void WebContentPage::did_cancel_geolocation_position_request(u64 request_id)
{
    if (view().on_cancel_geolocation_position_request)
        view().on_cancel_geolocation_position_request(*this, request_id);
}

void WebContentPage::did_start_geolocation_position_watch(u64 request_id)
{
    if (view().on_start_geolocation_position_watch)
        view().on_start_geolocation_position_watch(*this, request_id);
}

void WebContentPage::did_stop_geolocation_position_watch(u64 request_id)
{
    if (view().on_stop_geolocation_position_watch)
        view().on_stop_geolocation_position_watch(*this, request_id);
}

void WebContentPage::did_request_file_picker(Web::HTML::FileFilter accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files)
{
    view().did_request_file_picker({}, *this, accepted_file_types, allow_multiple_files);
}

void WebContentPage::did_finish_handling_input_event(u64 event_id, Web::EventResult event_result)
{
    if (displays_tab()) {
        view().did_finish_handling_input_event({}, event_id, event_result);
        return;
    }

    // The view displaying the tab handed the event down; it hears the result.
    if (auto display_page = traversable().display_page(); display_page) {
        if (display_page->is_open() && display_page.ptr() != this)
            display_page->did_finish_handling_input_event(event_id, event_result);
    }
}

void WebContentPage::did_update_input_method_state(Optional<Web::DevicePixelRect> caret_rect, bool is_enabled, i32 cursor_position, i32 anchor_position, Utf16String text_before_cursor, Utf16String text_after_cursor)
{

    // The page hosting the tab's focused navigable describes its text input, in the viewport of its local root.
    auto& traversable = view().traversable();
    auto host = traversable.focused_navigable_host();
    if (host != *this)
        return;
    if (caret_rect.has_value())
        caret_rect->translate_by(traversable.focused_navigable_host_offset());
    view().set_input_method_state({}, { is_enabled, cursor_position, anchor_position, move(text_before_cursor), move(text_after_cursor), caret_rect });
}

void WebContentPage::did_change_theme_color(Gfx::Color color)
{
    if (displays_tab()) {
        if (view().on_theme_color_change)
            view().on_theme_color_change(color);
    }
}

void WebContentPage::did_change_background_color(Gfx::Color color)
{
    if (displays_tab())
        view().did_change_background_color({}, color);
}

void WebContentPage::did_insert_clipboard_item(Web::Clipboard::SystemClipboardItem item, String)
{
    view().insert_clipboard_item(move(item));
}

void WebContentPage::did_change_audio_play_state(Web::HTML::AudioPlayState play_state)
{
    if (displays_tab())
        view().did_change_audio_play_state({}, play_state);
}

void WebContentPage::did_change_screen_wake_lock_state(Web::ScreenWakeLockState wake_lock_state)
{
    if (displays_tab())
        view().did_change_screen_wake_lock_state({}, wake_lock_state);
}

void WebContentPage::did_update_session_history_entry_navigation_api_state(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Web::HTML::StorageSerializationRecord navigation_api_state)
{
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;
    navigable->top_level_traversable().update_session_history_entry_navigation_api_state(*navigable, entry_identity, move(navigation_api_state));
}

void WebContentPage::did_update_session_history_entry_document_state_navigable_target_name(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Utf16String navigable_target_name)
{
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;
    navigable->top_level_traversable().update_session_history_entry_document_state_navigable_target_name(*navigable, entry_identity, move(navigable_target_name));
}

void WebContentPage::did_set_session_history_entry_document_state_reload_pending(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_api_key, bool reload_pending)
{
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;
    navigable->top_level_traversable().set_session_history_entry_document_state_reload_pending(*navigable, navigation_api_key, reload_pending);
}

void WebContentPage::did_change_focused_navigable(Web::HTML::CrossProcessId navigable_id)
{
    // A page moves focus to a navigable it hosts.
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;
    navigable->top_level_traversable().set_focused_navigable(*navigable, *this);
}

void WebContentPage::did_request_key_event_for_testing(Web::KeyEvent event)
{
    view().enqueue_input_event(move(event));
}

void WebContentPage::request_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters parameters)
{
    view().request_history_operation({}, *this, operation_id, move(parameters));
}

void WebContentPage::history_operation_ready(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationReadyResult result)
{
    traversable().did_receive_history_operation_ready(*this, operation_id, move(result));
}

void WebContentPage::history_step_unload_cancelation_result(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown unload_prompt_shown)
{
    traversable().did_receive_history_step_unload_cancelation_result(*this, operation_id, result, unload_prompt_shown);
}

void WebContentPage::beforeunload_check_result(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown unload_prompt_shown)
{
    traversable().did_receive_beforeunload_check_result(*this, operation_id, result, unload_prompt_shown);
}

void WebContentPage::changing_navigable_history_job_ready(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition, Web::HTML::UnloadDisplayedDocument unload_displayed_document)
{
    traversable().did_receive_changing_navigable_history_job_ready(*this, operation_id, navigable_id, disposition, unload_displayed_document);
}

void WebContentPage::changing_navigable_unload_preparation_complete(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id)
{
    traversable().did_receive_changing_navigable_unload_preparation_complete(*this, operation_id, navigable_id);
}

void WebContentPage::descendant_unload_task_complete(Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id)
{
    traversable().did_receive_descendant_unload_task_complete(*this, unload_id, navigable_id);
}

void WebContentPage::request_navigable_document_abort(Web::HTML::CrossProcessId navigable_id)
{
    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_abort_navigable_document(navigable_id);
}

void WebContentPage::request_navigable_document_unfullscreen(Web::HTML::CrossProcessId navigable_id)
{
    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_unfullscreen_navigable_document(navigable_id);
}

void WebContentPage::request_child_navigable_unload(Web::HTML::CrossProcessId navigable_id)
{
    traversable().did_receive_child_navigable_unload_request(*this, navigable_id);
}

void WebContentPage::changing_navigable_continuation_applied(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::ReplicatedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state)
{
    traversable().did_receive_changing_navigable_continuation_applied(*this, operation_id, navigable_id, move(activated_navigable_state), move(previous_entry_persisted_state));
}

void WebContentPage::nonchanging_navigable_history_state_updated(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id)
{
    traversable().did_receive_nonchanging_navigable_history_state_updated(*this, operation_id, navigable_id);
}

void WebContentPage::did_request_close_of_traversable(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId source_navigable_id)
{
    // window.close() from a document another process hosts: the process hosting the traversable's document runs the
    // rest of its steps.
    if (traversable().id() != navigable_id)
        return;

    // The page closing it must host the navigable it closes from.
    if (!hosted_navigable(source_navigable_id).has_value())
        return;

    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_close_traversable_from_script(navigable_id, source_navigable_id);
}

void WebContentPage::did_inspect_dom_tree(String dom_tree)
{
    if (displays_tab()) {
        if (view().on_received_dom_tree)
            view().on_received_dom_tree(parse_json(dom_tree, "DOM tree"sv));
    }
}

void WebContentPage::did_inspect_dom_node(DOMNodeProperties properties)
{
    if (displays_tab()) {
        if (view().on_received_dom_node_properties)
            view().on_received_dom_node_properties(move(properties));
    }
}

void WebContentPage::did_finish_editing_dom_node(Optional<Web::UniqueNodeID> node_id)
{
    if (displays_tab()) {
        if (view().on_finished_editing_dom_node)
            view().on_finished_editing_dom_node(node_id);
    }
}

void WebContentPage::did_mutate_dom(Mutation mutation)
{
    if (displays_tab()) {
        if (view().on_dom_mutation_received)
            view().on_dom_mutation_received(move(mutation));
    }
}

void WebContentPage::did_get_dom_node_html(String html)
{
    if (displays_tab()) {
        if (view().on_received_dom_node_html)
            view().on_received_dom_node_html(move(html));
    }
}

void WebContentPage::did_resolve_dom_node_url(u64 request_id, String resolved_url)
{
    if (displays_tab()) {
        auto handler = view().on_resolved_dom_node_url.take(request_id);
        if (handler.has_value())
            (*handler)(move(resolved_url));
    }
}

void WebContentPage::did_receive_network_response_headers(u64 request_id, u32 status_code, Optional<String> reason_phrase, Vector<HTTP::Header> response_headers, Requests::CameFromCache came_from_cache)
{
    if (displays_tab()) {
        if (view().on_network_response_headers_received)
            view().on_network_response_headers_received(request_id, status_code, reason_phrase, response_headers, came_from_cache);
    }
}

void WebContentPage::did_change_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String url, Optional<Utf16String> key, Optional<Utf16String> old_value, Optional<Utf16String> new_value)
{
    if (displays_tab()) {
        auto host = DevTools::storage_host_for_url(url);
        if (!host.has_value())
            return;

        DevTools::DevToolsDelegate::StorageChange::Type type;
        if (!key.has_value())
            type = DevTools::DevToolsDelegate::StorageChange::Type::Cleared;
        else if (!old_value.has_value())
            type = DevTools::DevToolsDelegate::StorageChange::Type::Added;
        else if (!new_value.has_value())
            type = DevTools::DevToolsDelegate::StorageChange::Type::Deleted;
        else
            type = DevTools::DevToolsDelegate::StorageChange::Type::Changed;

        view().notify_storage_changed({
            .storage_endpoint = storage_endpoint,
            .host = host.release_value(),
            .type = type,
            .key = key.has_value() ? Optional<String> { key->to_utf8() } : Optional<String> {},
        });
    }
}

void WebContentPage::did_update_indexed_database(String update)
{
    if (displays_tab())
        view().notify_indexed_database_changed(parse_json(update, "IndexedDB update"sv));
}

void WebContentPage::did_request_clipboard_entries(u64 request_id)
{
    Vector<Web::Clipboard::SystemClipboardItem> items;
    if (auto item = view().clipboard_item(); !item.system_clipboard_representations.is_empty())
        items.append(move(item));

    async_retrieved_clipboard_entries(request_id, items);
}

void WebContentPage::did_request_set_system_focus(bool has_system_focus)
{
    traversable().set_has_system_focus(has_system_focus, *this);
}

void WebContentPage::did_request_set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    view().set_system_visibility_state(visibility_state);
}

void WebContentPage::did_request_navigation_start(Web::HTML::CrossProcessId navigable_id, Web::NavigationTarget target, URL::URL url, Utf16String navigation_id, Optional<Web::HTML::NavigationStartRequest> start_request)
{
    CanonicalNavigable* target_navigable = &traversable();
    if (target == Web::NavigationTarget::IFrame) {
        auto child_frame = traversable().top_level_traversable().find(navigable_id);
        target_navigable = child_frame.has_value() ? &*child_frame : nullptr;
    }

    // AD-HOC: The local ongoing-navigation check can run before the UI traversal queue sets its canonical value.
    //         Recheck it here so navigation admission and history traversal remain ordered by the UI process.
    auto navigation_is_blocked_by_history_traversal = target_navigable
        && target_navigable->ongoing_navigation_is_traversal();
    if (!target_navigable
        || target_navigable->id() != navigable_id
        || (start_request.has_value() && start_request->navigable_id != navigable_id)
        || navigation_is_blocked_by_history_traversal) {
        async_cancel_navigation_params_creation(navigable_id, navigation_id);
        return;
    }

    auto sequence_number = target_navigable->top_level_traversable().next_sequence_number();
    if (auto const& ongoing_navigation = target_navigable->ongoing_navigation(); ongoing_navigation.has_value()
        && ongoing_navigation->sequence_number != 0
        && ongoing_navigation->navigation_id == navigation_id) {
        // A UI-initiated top-level load records its transaction, under a navigation ID the UI
        // generated, before WebContent enters navigate(). Keep its original admission order now
        // that WebContent has started the navigation.
        sequence_number = ongoing_navigation->sequence_number;
    }

    // A javascript: navigation runs synchronously in the requesting process and never populates an entry.
    // Record its admission without population state, owned by the evaluating process, so its failure or
    // produced document is validated against that process.
    if (!start_request.has_value()) {
        target_navigable->set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
            .url = url,
            .navigation_id = navigation_id,
            .sequence_number = sequence_number,
        });
        target_navigable->set_navigation_population_worker(*this);
        if (target_navigable->is_top_level_traversable()) {
            if (displays_tab())
                begin_top_level_load(move(navigation_id), url);
        }
        return;
    }

    target_navigable->set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
        .url = move(url),
        .navigation_id = navigation_id,
        .start_request = move(start_request),
        .sequence_number = sequence_number,
        .phase = CanonicalNavigable::OngoingNavigation::Phase::AwaitingUnloadCheck,
    });
    target_navigable->set_navigation_population_worker(*this);

    // Navigate, step 21.2: checking if unloading is canceled for navigable's active document's inclusive descendant
    // navigables. The pages hosting the documents the requesting page does not run their checks first; the
    // requesting page's own, with the navigation's bookkeeping, comes last, told whether a prompt was shown.
    Vector<Web::HTML::CrossProcessId> inclusive_descendants;
    target_navigable->for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        inclusive_descendants.append(navigable.id());
        return IterationDecision::Continue;
    });
    target_navigable->top_level_traversable().check_if_unloading_is_canceled(move(inclusive_descendants), *this, Web::HTML::UnloadPromptShown::No,
        [page = NonnullRefPtr<WebContentPage>(*this), navigable_id, navigation_id = move(navigation_id)](Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown unload_prompt_shown) {
            if (result != Web::HTML::HistoryStepResult::Applied) {
                // The navigation parked for its population is not coming; the recorded load ends as a failed one.
                page->async_cancel_navigation_params_creation(navigable_id, navigation_id);
                if (page->is_open())
                    page->did_fail_navigation_population(navigable_id, navigation_id);
                return;
            }
            page->async_run_navigation_unload_check(navigable_id, navigation_id, unload_prompt_shown);
        });
}

void WebContentPage::did_complete_navigation_unload_check(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id)
{
    auto navigable = population_worker_navigable(navigable_id);
    if (!navigable.has_value())
        return;

    auto& ongoing_navigation = navigable->ongoing_navigation();
    if (!ongoing_navigation.has_value()
        || ongoing_navigation->navigation_id != navigation_id
        || ongoing_navigation->phase != CanonicalNavigable::OngoingNavigation::Phase::AwaitingUnloadCheck
        || ongoing_navigation->population_worker != *this
        || !ongoing_navigation->start_request.has_value()) {
        return;
    }

    auto population_request = Web::HTML::create_navigation_population_request(
        ongoing_navigation->start_request.release_value(),
        Application::the().allocate_ui_process_cross_process_id());
    ongoing_navigation->loader = NavigationLoader::create(client().is_private(), move(population_request));
    ongoing_navigation->phase = CanonicalNavigable::OngoingNavigation::Phase::Populating;
    async_create_navigation_params(ongoing_navigation->loader->request());

    // Requesting navigation params starts the fetch, so a view's top-level population begins its recorded
    // load here.
    if (navigable->is_top_level_traversable()) {
        if (displays_tab())
            begin_top_level_load(move(navigation_id), ongoing_navigation->loader->request().history_entry.url);
    }
}

void WebContentPage::did_request_navigation_population(Web::HTML::CrossProcessId navigable_id, Web::NavigationTarget target, Web::HTML::NavigationPopulationRequest request)
{
    auto const& target_url = request.history_entry.url;

    CanonicalNavigable* target_navigable = &traversable();
    Optional<CanonicalNavigable&> child_frame;
    if (target == Web::NavigationTarget::IFrame) {
        child_frame = traversable().top_level_traversable().find(navigable_id);
        target_navigable = child_frame.has_value() ? &*child_frame : nullptr;
    }

    // AD-HOC: A population request can race with the UI traversal queue in the same way as a navigation-start
    //         request, so validate the canonical ongoing-navigation value before admitting it.
    auto navigation_is_blocked_by_history_traversal = target_navigable
        && target_navigable->ongoing_navigation_is_traversal();
    if (!target_navigable
        || target_navigable->id() != navigable_id
        || request.navigable_id != navigable_id
        || navigation_is_blocked_by_history_traversal) {
        async_cancel_navigation_params_creation(navigable_id, request.navigation_id);
        return;
    }

    if (auto const& ongoing_navigation = target_navigable->ongoing_navigation(); ongoing_navigation.has_value()
        && ongoing_navigation->navigation_id == request.navigation_id
        && ongoing_navigation->loader) {
        async_cancel_navigation_params_creation(navigable_id, request.navigation_id);
        return;
    }

    // A reconstructed child navigation was admitted in the populating phase without a request of its own;
    // only the process hosting the child's document may deliver its population request.
    auto continues_reconstructed_child_navigation = target_navigable->ongoing_navigation().has_value()
        && target_navigable->ongoing_navigation()->navigation_id == request.navigation_id
        && target_navigable->ongoing_navigation()->phase == CanonicalNavigable::OngoingNavigation::Phase::Populating
        && !target_navigable->ongoing_navigation()->loader
        && target_navigable->navigation_host_matches(*this);
    if (continues_reconstructed_child_navigation) {
        auto& ongoing_navigation = *target_navigable->ongoing_navigation();
        ongoing_navigation.url = target_url;
        ongoing_navigation.phase = CanonicalNavigable::OngoingNavigation::Phase::Populating;
        ongoing_navigation.loader = NavigationLoader::create(client().is_private(), move(request));
    } else {
        target_navigable->set_ongoing_navigation(CanonicalNavigable::OngoingNavigation {
            .url = target_url,
            .navigation_id = request.navigation_id,
            .sequence_number = target_navigable->top_level_traversable().next_sequence_number(),
            .phase = CanonicalNavigable::OngoingNavigation::Phase::Populating,
            .loader = NavigationLoader::create(client().is_private(), move(request)),
        });
    }

    // The UI process owns the in-parallel population work. Dispatch the document-dependent
    // steps through step 4 to the process with the live source document. The response URL then
    // determines which process receives the task queued by step 5.
    if (!target_navigable->ongoing_navigation()->population_worker)
        target_navigable->set_navigation_population_worker(*this);
    async_create_navigation_params(target_navigable->ongoing_navigation()->loader->request());

    // Requesting navigation params starts the fetch, so a view's top-level population begins its recorded
    // load here.
    if (target_navigable->is_top_level_traversable()) {
        if (displays_tab())
            begin_top_level_load(target_navigable->ongoing_navigation()->navigation_id, target_navigable->ongoing_navigation()->loader->request().history_entry.url);
    }
}

void WebContentPage::did_finish_navigation_params_creation(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id, Optional<Web::HTML::NavigationPopulationResult> result)
{
    auto navigable = population_worker_navigable(navigable_id);
    if (!navigable.has_value()) {
        if (result.has_value())
            NavigationLoader::discard(client().is_private(), *result);
        return;
    }

    if (!navigable->navigation_population_matches(*this, navigation_id)) {
        if (result.has_value())
            NavigationLoader::discard(client().is_private(), *result);
        return;
    }

    auto end_recorded_load_for_canceled_navigation = [&] {
        if (!navigable->is_top_level_traversable())
            return;
        if (displays_tab())
            view().did_cancel_loading(navigation_id);
    };

    if (!result.has_value()) {
        end_recorded_load_for_canceled_navigation();
        navigable->clear_ongoing_navigation();
        return;
    }

    auto& ongoing_navigation = navigable->ongoing_navigation();
    if (!ongoing_navigation.has_value() || !ongoing_navigation->loader) {
        NavigationLoader::discard(client().is_private(), *result);
        end_recorded_load_for_canceled_navigation();
        navigable->clear_ongoing_navigation();
        return;
    }

    // Steps 1-4 have produced final navigation params. Keep the pending entry in
    // sync with redirects before choosing the process that will run step 5.
    ongoing_navigation->phase = CanonicalNavigable::OngoingNavigation::Phase::AwaitingResponseBody;
    ongoing_navigation->loader->did_finish_navigation_params_creation(result.release_value());
    ongoing_navigation->url = ongoing_navigation->loader->request().history_entry.url;

    ongoing_navigation->loader->acquire_response_body([page = NonnullRefPtr<WebContentPage>(*this), navigable_id, navigation_id = move(navigation_id)](bool succeeded) {
        // The page may have closed while the body was fetched; its navigation then has nothing left to finish.
        if (!page->is_open())
            return;
        auto* open_page = page.ptr();
        auto cancel_navigation = [&](CanonicalNavigable& navigable) {
            if (navigable.is_top_level_traversable() && open_page->displays_tab())
                open_page->view().did_cancel_loading(navigation_id);
            navigable.clear_ongoing_navigation();
        };
        if (!succeeded) {
            if (auto navigable = open_page->population_worker_navigable(navigable_id); navigable.has_value())
                cancel_navigation(*navigable);
            return;
        }
        if (open_page->continue_navigation_population_in_selected_process(navigable_id, navigation_id))
            return;
        if (auto navigable = open_page->population_worker_navigable(navigable_id); navigable.has_value()) {
            auto const& ongoing_navigation = navigable->ongoing_navigation();
            if (ongoing_navigation.has_value() && ongoing_navigation->navigation_id == navigation_id)
                cancel_navigation(*navigable);
        }
    });
}

void WebContentPage::did_finish_history_navigation_params_creation(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryNavigationPopulation population)
{
    auto& host = this->traversable();
    host.top_level_traversable().did_finish_history_navigation_params_creation(*this, operation_id, move(population));
}

void WebContentPage::did_fail_navigation_population(Web::HTML::CrossProcessId navigable_id, Utf16String navigation_id)
{
    auto navigable = population_worker_navigable(navigable_id);
    if (!navigable.has_value())
        return;

    // The failure must name the admitted transaction, and must come from a process that owns part of its
    // outcome: the population worker (unload check, javascript: evaluation) or the population host.
    auto& ongoing_navigation = navigable->ongoing_navigation();
    if (!ongoing_navigation.has_value()
        || ongoing_navigation->navigation_id != navigation_id
        || !navigable->navigation_owner_matches(*this)) {
        return;
    }

    // Only a failed population handoff owns the loader's response body.
    if (ongoing_navigation->loader && navigable->navigation_host_matches(*this))
        ongoing_navigation->loader->reclaim_response_body_after_failed_handoff();

    m_history_recorded_url_for_current_load.clear();
    if (navigable->is_top_level_traversable()) {
        view().did_cancel_loading(navigation_id);
        return;
    }
    navigable->clear_ongoing_navigation();
}

void WebContentPage::did_change_replicated_navigable_state(Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedNavigableState state)
{
    // Only the process hosting a navigable's active document speaks for its state.
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;

    // A replacement process's bootstrap about:blank is not the traversable's committed entry; its state must not
    // replace the canonical one.
    if (navigable->is_top_level_traversable()) {
        if (navigable->pending_host_matches(*this))
            return;
    }

    navigable->update_replicated_state(move(state));
}

void WebContentPage::did_change_navigable_container_state(Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedContainerState state)
{
    // Only the page holding a navigable's container speaks for it.
    auto navigable = traversable().top_level_traversable().find(navigable_id);
    if (!navigable.has_value() || navigable->reporting_page() != *this)
        return;
    navigable->update_container_state(move(state));
}

void WebContentPage::did_update_child_frame_viewport(Web::HTML::CrossProcessId frame_id, Web::DevicePixelRect viewport_rect, Web::DevicePixelRect viewport_intersection, double device_pixel_ratio)
{
    if (auto child_frame = traversable().top_level_traversable().find(frame_id); child_frame.has_value())
        child_frame->set_viewport(viewport_rect, viewport_intersection, device_pixel_ratio);
}

void WebContentPage::did_destroy_child_frame(Web::HTML::CrossProcessId frame_id)
{
    if (auto child_frame = traversable().top_level_traversable().find(frame_id); child_frame.has_value())
        SiteIsolationManager::the().remove_child_frame_subtree(*child_frame);
}

Messages::WebContentClient::DidStartDownloadWithoutRequestResponse WebContentPage::did_start_download_without_request(URL::URL url, ByteString suggested_filename, Optional<u64> total_size)
{
    auto destination = choose_download_destination_or_report_error(url, suggested_filename);
    if (!destination.has_value())
        return { Optional<u64> {} };

    auto& file_downloader = Application::the().file_downloader();
    auto download_id = file_downloader.start_download(client().is_private(), url, destination.release_value(), total_size);
    if (!is_download_in_progress(file_downloader, download_id))
        return { Optional<u64> {} };

    client().remember_renderer_owned_download(download_id, m_id);

    file_downloader.set_cancel_callback(download_id, [page = NonnullRefPtr<WebContentPage>(*this), download_id] {
        if (!page->is_open())
            return;

        page->client().forget_renderer_owned_download(download_id);
        page->async_cancel_download(download_id);
    });

    return { download_id };
}

Messages::WebContentClient::DidStartDownloadResponse WebContentPage::did_start_download(Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> navigation_id, URL::URL url, ByteString suggested_filename, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ByteBuffer initial_data)
{
    // A download taking over an in-flight population's response body ends that navigation without a document;
    // the reporting process ends its side when its population output says the download was handled. The body
    // identifiers preserve the original RequestServer transfer lease, so only the process and navigation that
    // received that response may claim it, and only while its population is in flight.
    bool matches_in_flight_navigation = false;
    if (auto navigable = population_worker_navigable(navigable_id); navigable.has_value()) {
        auto const& ongoing_navigation = navigable->ongoing_navigation();
        if (ongoing_navigation.has_value()
            && ongoing_navigation->navigation_id == navigation_id
            && ongoing_navigation->phase == CanonicalNavigable::OngoingNavigation::Phase::Populating
            && ongoing_navigation->loader
            && ongoing_navigation->loader->response_body_matches(request_server_client_id, request_server_request_id)) {
            if (navigable->is_top_level_traversable()) {
                if (displays_tab())
                    view().did_cancel_loading(ongoing_navigation->navigation_id);
            }
            matches_in_flight_navigation = true;
            navigable->clear_ongoing_navigation();
        }
    }
    if (!matches_in_flight_navigation)
        return { Optional<u64> {} };

    auto destination = choose_download_destination_or_report_error(url, suggested_filename);
    if (!destination.has_value())
        return { Optional<u64> {} };

    auto& file_downloader = Application::the().file_downloader();
    auto download_id = file_downloader.adopt_download(client().is_private(), url, destination.release_value(), total_size, request_server_client_id, request_server_request_id, initial_data.bytes());
    if (!is_download_in_progress(file_downloader, download_id))
        return { Optional<u64> {} };

    return { download_id };
}

void WebContentPage::did_receive_download_data(u64 download_id, ByteBuffer data)
{
    if (!client().is_renderer_owned_download(m_id, download_id))
        return;

    Application::the().file_downloader().append_download_data(download_id, data.bytes());
}

void WebContentPage::did_finish_download(u64 download_id)
{
    if (!client().is_renderer_owned_download(m_id, download_id))
        return;

    client().forget_renderer_owned_download(download_id);
    Application::the().file_downloader().finish_download(download_id);
}

void WebContentPage::did_fail_download(u64 download_id, String error)
{
    if (!client().is_renderer_owned_download(m_id, download_id))
        return;

    client().forget_renderer_owned_download(download_id);
    Application::the().file_downloader().fail_download(download_id, move(error));
}

void WebContentPage::did_finish_loading(Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> navigation_id)
{
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;

    if (!navigable->matches_ongoing_navigation(navigation_id))
        return;

    // A replacement process's bootstrap about:blank finishes before the process hosts the committed
    // entry; it must not surface in the view.
    if (navigable->is_top_level_traversable()) {
        if (navigable->pending_host_matches(*this))
            return;
    }

    if (!navigable->is_top_level_traversable()) {
        navigable->clear_active_document_load();
        return;
    }

    if (displays_tab()) {
        auto const& committed_url = view().url();

        if (committed_url.scheme() == "about"sv && committed_url.path_segment_count() == 1) {
            if (auto web_ui = WebUI::create(client(), m_id, MUST(String::from_utf8(committed_url.path_segments().first()))); web_ui.is_error())
                warnln("Could not create WebUI for {}: {}", committed_url, web_ui.error());
            else
                client().m_web_ui = web_ui.release_value();
        }

        auto title = history_title(view().title(), committed_url);

        dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Load finished for page {} at '{}' with title '{}'",
            m_id,
            committed_url,
            title.has_value() ? title->bytes_as_string_view() : "<none>"sv);

        maybe_record_history_visit_for_current_load(committed_url, title, "load finish"sv);
        if (title.has_value())
            client().session().history_store->update_title(committed_url, *title);
        if (view().favicon_hash().has_value())
            client().session().history_store->update_favicon(committed_url, *view().favicon_hash());

        view().did_finish_navigation();

        if (view().on_load_finish)
            view().on_load_finish(committed_url);

        for (auto const& [id, listener] : view().m_navigation_listeners) {
            if (listener.on_load_finish)
                listener.on_load_finish(committed_url);
        }
    }
}

void WebContentPage::did_change_title(Utf16String title)
{
    if (auto process = WebView::Application::the().find_process(client().pid()); process.has_value())
        process->set_title(title);

    if (displays_tab()) {
        if (!title.is_empty()) {
            auto title_utf8 = title.to_utf8();

            maybe_record_history_visit_for_current_load(view().url(), title_utf8, "title change"sv);
            dbgln_if(WEBVIEW_HISTORY_DEBUG, "[History] Title changed for page {} at '{}' to '{}'",
                m_id,
                view().url(),
                title_utf8);

            client().session().history_store->update_title(view().url(), title_utf8);
        }

        if (title.is_empty())
            title = Utf16String::from_utf8(view().url().serialize());

        view().set_title({}, title);
    }
}

void WebContentPage::did_request_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, Web::ContextMenuForInputEventsTarget for_input_events_target)
{
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_page_context_menu({}, target->position, for_input_events_target);
}

void WebContentPage::did_request_link_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, URL::URL url, ByteString, unsigned)
{
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_link_context_menu({}, target->position, move(url));
}

void WebContentPage::did_request_image_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, URL::URL url, ByteString, unsigned, Optional<Gfx::ShareableBitmap> bitmap)
{
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_image_context_menu({}, target->position, move(url), move(bitmap));
}

void WebContentPage::did_request_media_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, ByteString, unsigned, Web::Page::MediaContextMenu menu)
{
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_media_context_menu({}, *this, target->position, move(menu));
}

void WebContentPage::did_get_source(URL::URL url, URL::URL base_url, Utf16String source)
{
    if (auto new_tab = Application::the().open_blank_new_tab(Web::HTML::ActivateTab::Yes); new_tab.has_value()) {
        auto html = highlight_source(url, base_url, source.to_utf8(), Syntax::Language::HTML);
        new_tab->load_html(html);
    }
}

void WebContentPage::did_get_debugger_environments(u64 request_id, Optional<String> error, Vector<DebuggerEnvironment> environments)
{
    if (displays_tab()) {
        auto callback = view().m_pending_debugger_environments_requests.take(request_id);
        if (!callback.has_value()) {
            if (view().m_cancelled_debugger_environments_requests.remove(request_id))
                return;
            client().did_misbehave("did_get_debugger_environments"sv, "unexpected request ID"sv);
            return;
        }
        if (error.has_value())
            (*callback)(Error::from_string_view(error->bytes_as_string_view()));
        else
            (*callback)(move(environments));
    }
}

void WebContentPage::did_evaluate_javascript_in_debugger_frame(u64 request_id, Optional<String> error, DebuggerEvaluationResult result)
{
    if (displays_tab()) {
        auto callback = view().m_pending_debugger_evaluation_requests.take(request_id);
        if (!callback.has_value()) {
            if (view().m_cancelled_debugger_evaluation_requests.remove(request_id))
                return;
            client().did_misbehave("did_evaluate_javascript_in_debugger_frame"sv, "unexpected request ID"sv);
            return;
        }
        if (error.has_value())
            (*callback)(error.release_value());
        else
            (*callback)(move(result));
    }
}

void WebContentPage::did_get_debugger_object_properties(u64 request_id, Optional<String> error, DebuggerObjectProperties properties)
{
    if (displays_tab()) {
        auto callback = view().m_pending_debugger_object_properties_requests.take(request_id);
        if (!callback.has_value()) {
            if (view().m_cancelled_debugger_object_properties_requests.remove(request_id))
                return;
            client().did_misbehave("did_get_debugger_object_properties"sv, "unexpected request ID"sv);
            return;
        }
        if (error.has_value())
            (*callback)(error.release_value());
        else
            (*callback)(move(properties));
    }
}

void WebContentPage::did_get_debugger_source_positions(u64 request_id, Vector<DebuggerSourcePosition> positions)
{
    if (displays_tab()) {
        auto callback = view().m_pending_debugger_source_positions_requests.take(request_id);
        if (!callback.has_value()) {
            if (view().m_cancelled_debugger_source_positions_requests.remove(request_id))
                return;
            client().did_misbehave("did_get_debugger_source_positions"sv, "unexpected request ID"sv);
            return;
        }
        (*callback)(move(positions));
    }
}

void WebContentPage::did_request_alert(Utf16String message)
{
    did_open_dialog(Web::Page::PendingDialog::Alert, message);
    if (view().on_request_alert)
        view().on_request_alert(message);
}

void WebContentPage::did_request_confirm(Utf16String message)
{
    did_open_dialog(Web::Page::PendingDialog::Confirm, message);
    if (view().on_request_confirm)
        view().on_request_confirm(message);
}

void WebContentPage::did_request_prompt(Utf16String message, Utf16String default_)
{
    did_open_dialog(Web::Page::PendingDialog::Prompt, message);
    if (view().on_request_prompt)
        view().on_request_prompt(message, default_);
}

void WebContentPage::did_change_favicon(Gfx::ShareableBitmap favicon)
{
    if (!favicon.is_valid()) {
        dbgln("DidChangeFavicon: Received invalid favicon");
        return;
    }

    if (displays_tab()) {
        maybe_record_history_visit_for_current_load(view().url(), history_title(view().title(), view().url()), "favicon change"sv);
        view().set_favicon({}, *favicon.bitmap());
    }
}

void WebContentPage::did_request_delete_all_cookies(u64 request_id, URL::URL url)
{
    if (!WebContentClient::renderers_may_access_cookies_like_http()) {
        client().did_misbehave("did_request_delete_all_cookies"sv, "not driven by WebDriver"sv);
        return;
    }

    client().session().cookie_jar->delete_all_cookies(url);
    async_did_delete_all_cookies(request_id);
}

Messages::WebContentClient::DidRequestNewWebViewResponse WebContentPage::did_request_new_web_view(Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, Optional<Web::HTML::CrossProcessId> opener_navigable_id, Optional<URL::URL> opener_base_url, Utf16String target_name)
{
    // The opener is a navigable the requesting page hosts. A request naming one it does not is refused before the
    // chrome is asked for a view, so nothing is created for a traversable that never will be.
    Optional<CanonicalNavigable&> opener;
    if (opener_navigable_id.has_value()) {
        opener = hosted_navigable(*opener_navigable_id);
        if (!opener.has_value() || !opener->replicated_state().has_value())
            return { {}, {}, {}, Web::HTML::VisibilityState::Hidden, {} };
    }

    auto new_page_id = Application::the().allocate_page_id();
    String window_handle;
    if (view().on_new_web_view)
        window_handle = view().on_new_web_view(activate_tab, hints, new_page_id);

    auto* new_page = client().page(new_page_id);
    if (!new_page || !new_page->displays_tab())
        return { {}, {}, {}, Web::HTML::VisibilityState::Hidden, move(window_handle) };
    auto& new_view = new_page->view();

    auto root_navigable_id = Application::the().allocate_ui_process_cross_process_id();
    new_view.traversable().set_id(root_navigable_id);

    // An auxiliary traversable's initial about:blank inherits its opener's origin and base URL
    Optional<URL::Origin> opener_origin;
    if (opener.has_value())
        opener_origin = opener->replicated_state()->active_document_origin;

    auto initial_history_entry = Web::HTML::create_initial_session_history_entry_descriptor(
        Application::the().allocate_ui_process_cross_process_id(), move(opener_origin), move(opener_base_url), move(target_name));
    new_view.traversable().create_a_new_top_level_traversable(opener, initial_history_entry, client());
    new_view.update_navigation_action_state();

    return { new_page_id, root_navigable_id, move(initial_history_entry), new_view.traversable().system_visibility_state(), move(window_handle) };
}

void WebContentPage::did_close_browsing_context()
{
    SiteIsolationManager::the().remove_page(*this);
    // NB: Before unregistering, so an acknowledged embedded discard closes an otherwise-unused server immediately.
    m_detached_close_pending = false;
    // Unregistering closes a page that only held part of the tab, so ask first.
    auto displays_tab = this->displays_tab();
    client().unregister_embedded_page(m_id);

    if (displays_tab) {
        auto& view = this->view();
        view.did_close_browsing_context({});
        if (view.on_close)
            view.on_close();
    }

    client().close_server_if_unused();
}

void WebContentPage::did_request_select_dropdown(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)
{
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_select_dropdown({}, *this, target->position, minimum_width, move(items));
}

void WebContentPage::did_request_primary_paste()
{
    auto text = Application::the().clipboard_text(Application::ClipboardType::Selection);
    async_paste(text);
}

void WebContentPage::did_update_primary_selection(String text)
{
    Application::the().set_clipboard_text(move(text), Application::ClipboardType::Selection);
}

void WebContentPage::did_update_session_history_entry_scroll_restoration_mode(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity entry_identity, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;
    if (scroll_restoration_mode != Web::HTML::ScrollRestorationMode::Auto
        && scroll_restoration_mode != Web::HTML::ScrollRestorationMode::Manual) {
        client().did_misbehave("did_update_session_history_entry_scroll_restoration_mode"sv, "invalid scroll restoration mode"sv);
        return;
    }
    navigable->top_level_traversable().update_session_history_entry_scroll_restoration_mode(*navigable, entry_identity, scroll_restoration_mode);
}

void WebContentPage::did_request_webdriver_mouse_event(u64 request_id, Web::HTML::CrossProcessId local_root_id, Web::MouseEvent event)
{
    auto on_handled = [page = NonnullRefPtr<WebContentPage>(*this), request_id]() {
        page->async_did_handle_webdriver_mouse_event(request_id);
    };

    // The event is relative to the viewport of the local root its page dispatches input to.
    if (auto root = view().traversable().find(local_root_id); root.has_value()) {
        auto offset = view().traversable().local_root_offset(*root);
        event.position.translate_by(offset);
        event.screen_position.translate_by(offset);
    }
    view().enqueue_webdriver_mouse_event({}, move(event), move(on_handled));
}

void WebContentPage::request_unload_check(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId check_id)
{
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value()) {
        async_unload_check_result(check_id, Web::HTML::HistoryStepResult::Applied);
        return;
    }
    Vector<Web::HTML::CrossProcessId> inclusive_descendants;
    navigable->for_each_in_inclusive_subtree([&](CanonicalNavigable const& descendant) {
        inclusive_descendants.append(descendant.id());
        return IterationDecision::Continue;
    });
    navigable->top_level_traversable().check_if_unloading_is_canceled(move(inclusive_descendants), {}, Web::HTML::UnloadPromptShown::No,
        [page = NonnullRefPtr<WebContentPage>(*this), check_id](Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown) {
            page->async_unload_check_result(check_id, result);
        });
}

Messages::WebContentClient::StartWorkerAgentResponse WebContentPage::start_worker_agent(Web::HTML::WorkerAgentStartRequest request)
{
    // A page hosting an isolated iframe's document belongs to its tab's view as much as the view's own page does.
    auto agent_id = WorkerProcessManager::the().start_worker_agent(client(), m_id, move(request));
    return { agent_id };
}

Messages::WebContentClient::DidRequestStorageUsageResponse WebContentPage::did_request_storage_usage(String storage_key)
{
    return client().session().storage_jar->usage(storage_key);
}

void WebContentPage::did_post_broadcast_channel_message(Web::HTML::BroadcastChannelMessage message)
{
    WebContentClient::for_each_client([&](auto& client) {
        if (client.pid() == message.source_process_id)
            return IterationDecision::Continue;
        if (client.is_private() != this->client().is_private())
            return IterationDecision::Continue;
        client.async_broadcast_channel_message(message);
        return IterationDecision::Continue;
    });
    WorkerProcessManager::the().broadcast_channel_message_from_web_content(message, client().is_private());
}

void WebContentPage::close_worker_agent(Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    WorkerProcessManager::the().close_worker_agent(client(), agent_id, owner_token);
}

void WebContentPage::did_finish_test(String text)
{
    if (displays_tab() && view().on_test_finish)
        view().on_test_finish(text);
}

void WebContentPage::did_set_test_timeout(double milliseconds)
{
    if (displays_tab() && view().on_set_test_timeout)
        view().on_set_test_timeout(milliseconds);
}

void WebContentPage::did_receive_reference_test_metadata(JsonValue metadata)
{
    if (displays_tab() && view().on_reference_test_metadata)
        view().on_reference_test_metadata(metadata);
}

void WebContentPage::did_simulate_worker_request_server_connection_loss()
{
    VERIFY(Application::web_content_options().is_test_mode == IsTestMode::Yes);
    if (auto result = WorkerProcessManager::the().simulate_request_server_connection_loss_for_testing(client(), m_id); result.is_error()) {
        warnln("Unable to reconnect WebWorker processes to RequestServer: {}", result.error());
        VERIFY_NOT_REACHED();
    }
}

Messages::WebContentTestClient::DidRequestUiProcessSessionHistoryForTestingResponse WebContentPage::did_request_ui_process_session_history_for_testing()
{
    if (displays_tab())
        return { view().ui_process_session_history_for_testing({}) };
    return { "{}"_string };
}

Messages::WebContentTestClient::DidRequestSiteIsolationProcessTreeForTestingResponse WebContentPage::did_request_site_isolation_process_tree_for_testing()
{
    return { SiteIsolationManager::the().dump_process_tree(client(), m_id) };
}

void WebContentPage::did_request_crash_of_remote_frame_processes_for_testing()
{
    traversable().for_each_in_subtree([](CanonicalNavigable& child_frame) {
        if (child_frame.has_remote_host())
            child_frame.remote_host().async_debug_request("crash-current-page"sv, ""sv);
        return IterationDecision::Continue;
    });
}

void WebContentPage::did_reset_session_history_for_testing(Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    if (displays_tab())
        view().did_reset_session_history_for_testing({}, move(active_entry));
}

Messages::WebContentTestClient::DidRequestCaptureSessionHistorySnapshotForTestingResponse WebContentPage::did_request_capture_session_history_snapshot_for_testing()
{
    if (displays_tab())
        return { view().capture_session_history_snapshot_for_testing({}) };
    return { false };
}

Messages::WebContentTestClient::DidRequestRestoreSessionHistorySnapshotForTestingResponse WebContentPage::did_request_restore_session_history_snapshot_for_testing()
{
    if (displays_tab())
        return { view().restore_captured_session_history_snapshot_for_testing({}) };
    return { false };
}

Messages::WebContentTestClient::DidRequestRegisterSessionStoreTabForTestingResponse WebContentPage::did_request_register_session_store_tab_for_testing()
{
    if (displays_tab())
        return { view().register_session_store_tab_for_testing({}) };
    return { false };
}

Messages::WebContentTestClient::DidRequestSessionStoreTabStateForTestingResponse WebContentPage::did_request_session_store_tab_state_for_testing()
{
    if (displays_tab())
        return { view().session_store_tab_state_for_testing({}) };
    return { "{}"_string };
}

Messages::WebContentClient::DidRequestCookieResponse WebContentPage::did_request_cookie(URL::URL url, HTTP::Cookie::Source source)
{
    HTTP::Cookie::VersionedCookie cookie;
    cookie.cookie = client().session().cookie_jar->get_cookie(url, source);
    if (source == HTTP::Cookie::Source::NonHttp)
        cookie.cookie_version = view().document_cookie_version(url);
    return cookie;
}

}
