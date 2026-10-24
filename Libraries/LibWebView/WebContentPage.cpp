/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StatsLog.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibWebCommon/Geolocation/GeolocationPositionErrorCode.h>
#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/WebDriver/Error.h>
#include <LibWebCommon/WebView/SiteIsolation.h>
#include <LibWebView/Application.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/PictureInPictureManager.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/StorageAreaRegistry.h>
#include <LibWebView/StorageJar.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebContentTestClient.h>
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

WebContentServerProxy* WebContentPage::routed_proxy() const
{
    return routed_connection();
}

WebContentClient& WebContentPage::client() const
{
    VERIFY(m_client);
    return *m_client;
}

CanonicalTraversable& WebContentPage::traversable() const
{
    VERIFY(m_traversable);
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

String WebContentPage::dump_process_tree() const
{
    StringBuilder builder;
    Vector<WebContentClient const*> processes;

    auto process_index = [&](WebContentClient const& process) -> size_t {
        for (size_t i = 0; i < processes.size(); ++i) {
            if (processes[i] == &process)
                return i;
        }
        processes.append(&process);
        return processes.size() - 1;
    };

    Function<void(CanonicalNavigable const&, size_t)> dump_frame_tree;
    dump_frame_tree = [&](CanonicalNavigable const& parent, size_t depth) {
        for (size_t i = 0; i < parent.children().size(); ++i) {
            auto const& child_frame = *parent.children()[i];

            builder.append_repeated(' ', depth * 2);
            builder.appendff("iframe#{}: {}", i, child_frame.has_remote_host() ? "remote"sv : "local"sv);
            if (child_frame.has_remote_host())
                builder.appendff(" WebContent#{}", process_index(child_frame.remote_host().client()));
            builder.append('\n');

            dump_frame_tree(child_frame, depth + 1);
        }
    };

    builder.appendff("WebContent#{}\n", process_index(client()));
    dump_frame_tree(traversable(), 1);
    return builder.to_string_without_validation();
}

Optional<CanonicalNavigable&> WebContentPage::hosted_navigable(Web::HTML::CrossProcessId navigable_id) const
{
    auto navigable = traversable().find(navigable_id);
    if (!navigable.has_value() || !traversable().hosts(*navigable, *this))
        return {};
    return *navigable;
}

// The environments a page names are the window environments of the documents it hosts, and of those populated for it
// to host.
void WebContentPage::for_each_hosted_document(Function<IterationDecision(CanonicalDocument&)> const& callback) const
{
    traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        auto decision = IterationDecision::Continue;
        if (traversable().hosts(navigable, *this))
            decision = callback(navigable.active_document());
        navigable.for_each_populated_document_state([&](CanonicalDocumentState const& document_state) {
            auto& document = *document_state.populated_document->document;
            if (decision == IterationDecision::Continue && document.host() == this)
                decision = callback(document);
        });
        return decision;
    });
}

Optional<CanonicalDocument&> WebContentPage::document_with_hosted_environment(Web::HTML::EnvironmentId const& environment_id) const
{
    Optional<CanonicalDocument&> found_document;
    for_each_hosted_document([&](CanonicalDocument& document) {
        if (document.relevant_global_object().relevant_settings_object().id() != environment_id)
            return IterationDecision::Continue;
        found_document = document;
        return IterationDecision::Break;
    });
    return found_document;
}

bool WebContentPage::hosts_an_environment_with_storage_key(Web::StorageAPI::StorageKey const& storage_key) const
{
    bool hosts_one = false;
    for_each_hosted_document([&](CanonicalDocument& document) {
        hosts_one = obtain_a_storage_key_for_non_storage_purposes(document.relevant_global_object().relevant_settings_object()) == storage_key;
        return hosts_one ? IterationDecision::Break : IterationDecision::Continue;
    });
    return hosts_one;
}

Optional<CanonicalEnvironmentSettingsObject const&> WebContentPage::hosted_environment(Web::HTML::EnvironmentId const& environment_id) const
{
    auto document = document_with_hosted_environment(environment_id);
    if (!document.has_value())
        return {};
    return document->relevant_global_object().relevant_settings_object();
}

void WebContentPage::spoof_document_origin_for_testing(Web::HTML::EnvironmentId const& environment_id, URL::Origin origin)
{
    if (auto document = document_with_hosted_environment(environment_id); document.has_value())
        document->set_origin_for_testing(move(origin));
}

// A child frame's container speaks for it only in the page holding the container. Any other page naming it, the
// traversable included, misbehaves.
bool WebContentPage::holds_container_of(CanonicalNavigable const& child_frame, StringView message_name)
{
    if (child_frame.reporting_page() == *this)
        return true;
    client().did_misbehave(message_name, "frame whose container the page does not hold"sv);
    return false;
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

    auto navigation = navigable->navigation_with_id(navigation_id);
    if (!navigation || !navigation->state.has<CanonicalNavigation::AcquiringResponseBody>())
        return false;

    navigation->loader()->obtain_the_response_policies(*navigable);
    navigation->loader()->check_the_responses_adherence_to_its_embedder_policy(*navigable);

    NonnullRefPtr history_entry = *navigation->history_entry;
    auto host_or_error = navigable->obtain_page_to_populate_document(history_entry, *navigation->loader(), *this);
    if (host_or_error.is_error()) {
        warnln("Unable to obtain a page to populate the navigation's document: {}", host_or_error.error());
        return false;
    }
    auto host = host_or_error.release_value().page;

    // Obtaining the page can replace the tab's process, whose change callbacks can end the navigation.
    navigation = navigable->navigation_with_id(navigation_id);
    if (!navigation || !navigation->state.has<CanonicalNavigation::AcquiringResponseBody>())
        return false;

    auto& acquiring = navigation->state.get<CanonicalNavigation::AcquiringResponseBody>();
    auto loader = move(acquiring.loader);
    auto weak_loader = loader->make_weak_ptr();
    navigation->state = CanonicalNavigation::Populating { move(loader) };
    navigation->page = host;

    auto finish_handoff = [host, navigable_id, navigation_id, weak_loader](bool succeeded) {
        if (!weak_loader || !host->is_open())
            return;
        auto navigable = host->traversable().top_level_traversable().find(navigable_id);
        if (!navigable.has_value())
            return;
        auto navigation = navigable->navigation_with_id(navigation_id);
        if (!navigation || !navigation->state.has<CanonicalNavigation::Populating>() || navigation->loader().ptr() != weak_loader.ptr())
            return;
        if (!succeeded) {
            navigable->did_cancel_navigation(navigation_id);
            return;
        }

        // https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-populate-the-history-entry's-document
        // 5. Queue a global task on the navigation and traversal task source, given navigable's active window, to run
        //    these steps:
        // NB: The page to host the document runs the task.
        host->async_populate_navigation(weak_loader->request(), weak_loader->take_result());
    };
    // Wait for the destination RequestServer to acknowledge the import before the host adopts it.
    if (auto client_id = host->client().request_server_site_bindings().client_id(); client_id.has_value()) {
        if (auto request_server = RequestServerManager::the().instance_for_client(*client_id)) {
            weak_loader->move_response_body_to(*request_server, move(finish_handoff));
            return true;
        }
    }
    finish_handoff(false);
    return true;
}

// A navigation's population steps run in the process recorded as its population worker at admission, which is
// the process with the live source document. That is not necessarily the process hosting the target's document.
Optional<CanonicalNavigable&> WebContentPage::population_worker_navigable(Web::HTML::CrossProcessId navigable_id) const
{
    if (auto navigable = hosted_navigable(navigable_id); navigable.has_value())
        return navigable;

    auto navigable = traversable().top_level_traversable().find(navigable_id);
    if (!navigable.has_value() || !navigable->uncommitted_navigation() || navigable->uncommitted_navigation()->worker() != this)
        return {};
    return *navigable;
}

StorageJar* WebContentPage::storage_jar(Web::StorageAPI::StorageEndpointType storage_endpoint) const
{
    if (storage_endpoint == Web::StorageAPI::StorageEndpointType::SessionStorage)
        return &traversable().top_level_traversable().session_storage();
    return client().session().storage_jar.ptr();
}

// The browser process obtains a key a page names again from the environment it was obtained for.
Optional<String> WebContentPage::canonical_storage_key(Web::HTML::EnvironmentId const& environment_id) const
{
    auto environment = hosted_environment(environment_id);
    if (!environment.has_value())
        return {};
    auto storage_key = obtain_a_storage_key(*environment);
    if (!storage_key.has_value())
        return {};
    return storage_key->to_string();
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
void WebContentPage::did_open_dialog(Web::PendingDialog dialog, Utf16String const& message)
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

void WebContentPage::begin_top_level_load(URL::URL const& url)
{
    if (auto process = WebView::Application::process_manager().find_child(client().child_id()); process.has_value())
        process->set_title(OptionalNone {});

    m_history_recorded_url_for_current_load.clear();

    auto& view = this->view();
    view.m_history_visit_transition_for_current_load = view.m_history_visit_transition_for_next_load;
    view.m_history_visit_transition_for_next_load = HistoryVisitTransition::Link;
    view.did_start_navigation();

    view.set_editing_history_state(false, false);

    if (view.on_load_start)
        view.on_load_start();

    for (auto const& [id, listener] : view.m_navigation_listeners) {
        if (listener.on_load_start)
            listener.on_load_start(url);
    }
}

void WebContentPage::request_close()
{
    // The frontend may destroy the view immediately after this for pages that cannot prompt during beforeunload.
    // Keep owning the WebContent close until the page reports that its top-level traversable has been closed.
    m_detached_close_pending = true;
    async_request_close();
}

void WebContentPage::discard()
{
    async_discard_embedded_page();
    // The page stops being a history job endpoint now; queued history work must not start against it. Its
    // client outlives the discard acknowledgement, so a shared process is not closed under the page.
    m_detached_close_pending = true;
    client().unregister_embedded_page(m_id);
}

Web::CompositorContextId WebContentPage::compositor_context_id()
{
    if (!m_is_open)
        return Web::compositor_context_id_for_page(m_id);
    return client().compositor_context_id_for_page(*this);
}

bool WebContentPage::handle_and_dispatch_key_event_in_compositor(Web::KeyEvent const& event, bool allow_keyboard_scroll)
{
    auto posted = Application::the().handle_and_dispatch_key_event_in_compositor(compositor_context_id(), event, allow_keyboard_scroll);
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI posted key event {} for page {} to the compositor: {}", event.id, m_id, posted);
    return posted;
}

void WebContentPage::handle_pinch_event_in_compositor(Web::PinchEvent const& event)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI posted pinch event {} for page {} to the compositor", event.id, m_id);
    Application::the().handle_pinch_event_in_compositor(compositor_context_id(), event);
}

bool WebContentPage::handle_and_dispatch_mouse_event_in_compositor(Web::MouseEvent const& event)
{
    auto posted = Application::the().handle_and_dispatch_mouse_event_in_compositor(compositor_context_id(), event);
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI posted mouse event {} (type {}) for page {} to the compositor: {}",
        event.id, to_underlying(event.type), m_id, posted);
    return posted;
}

void WebContentPage::did_consume_input_event_in_compositor(u64 event_id)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor consumed input event {} for page {}", event_id, m_id);
    if (displays_tab()) {
        view().did_consume_input_event_in_compositor({}, event_id);
        return;
    }

    // The view displaying the tab handed the event down; it hears the result.
    if (auto display_page = traversable().display_page(); display_page) {
        if (display_page->is_open() && display_page.ptr() != this)
            display_page->did_consume_input_event_in_compositor(event_id);
    }
}

void WebContentPage::did_not_dispatch_input_event_through_compositor(u64 event_id)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor did not dispatch input event {} for page {}; sending it to WebContent directly", event_id, m_id);
    if (displays_tab()) {
        view().did_not_dispatch_input_event_through_compositor({}, event_id);
        return;
    }

    if (auto display_page = traversable().display_page(); display_page) {
        if (display_page->is_open() && display_page.ptr() != this)
            display_page->did_not_dispatch_input_event_through_compositor(event_id);
    }
}

void WebContentPage::did_dispatch_input_event_through_compositor(u64 event_id, bool async_scroll_performed_default_action)
{
    if (displays_tab()) {
        view().did_dispatch_input_event_through_compositor({}, event_id, async_scroll_performed_default_action);
        return;
    }

    if (auto display_page = traversable().display_page(); display_page) {
        if (display_page->is_open() && display_page.ptr() != this)
            display_page->did_dispatch_input_event_through_compositor(event_id, async_scroll_performed_default_action);
    }
}

void WebContentPage::did_present_bitmap(Gfx::IntRect content_rect, Gfx::IntRect damage_rect, i32 bitmap_id, double device_pixel_ratio)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI compositor IPC did_paint for page {} bitmap {} rect={}x{} at {},{}",
        m_id, bitmap_id, content_rect.width(), content_rect.height(), content_rect.x(), content_rect.y());
    if (displays_tab()) {
        view().server_did_paint({}, bitmap_id, content_rect.size(), damage_rect, device_pixel_ratio);
        return;
    }
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI dropping did_paint for page {} bitmap {}: no view", m_id, bitmap_id);
    release_presented_bitmap(bitmap_id);
}

void WebContentPage::did_present_backing_stores(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI received {} backing stores for page {}", backing_stores.size(), m_id);
    if (!displays_tab()) {
        // The page can begin to display the tab, when the view installs them. The compositor reserves the first
        // published buffer for the UI to install as its front buffer.
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI keeping {} backing stores for page {} until it displays the tab", backing_stores.size(), m_id);
        if (m_presented_backing_stores.has_value() && !m_presented_backing_stores->bitmap_ids.is_empty())
            release_presented_bitmap(m_presented_backing_stores->bitmap_ids[0]);
        m_presented_backing_stores = PresentedBackingStores { move(bitmap_ids), move(backing_stores) };
        return;
    }
    view().did_allocate_backing_stores({}, move(bitmap_ids), move(backing_stores));
}

void WebContentPage::did_add_backing_stores(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI received {} additional backing stores for page {}", backing_stores.size(), m_id);
    if (displays_tab()) {
        view().did_add_backing_stores({}, move(bitmap_ids), move(backing_stores));
        return;
    }
    if (!m_presented_backing_stores.has_value())
        return;
    m_presented_backing_stores->bitmap_ids.extend(move(bitmap_ids));
    m_presented_backing_stores->backing_stores.extend(move(backing_stores));
}

void WebContentPage::did_retire_backing_stores(ReadonlySpan<i32> bitmap_ids)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] UI retiring backing stores {} for page {}", bitmap_ids, m_id);
    if (displays_tab()) {
        view().did_retire_backing_stores({}, bitmap_ids);
        return;
    }
    if (!m_presented_backing_stores.has_value())
        return;
    // The first store is the one the view installs as its front buffer, which the compositor never retires.
    for (size_t i = m_presented_backing_stores->bitmap_ids.size(); i-- > 1;) {
        if (!bitmap_ids.contains_slow(m_presented_backing_stores->bitmap_ids[i]))
            continue;
        m_presented_backing_stores->bitmap_ids.remove(i);
        m_presented_backing_stores->backing_stores.remove(i);
    }
}

Optional<WebContentPage::PresentedBackingStores> WebContentPage::take_presented_backing_stores()
{
    auto backing_stores = move(m_presented_backing_stores);
    m_presented_backing_stores.clear();
    return backing_stores;
}

void WebContentPage::release_presented_bitmap(i32 bitmap_id)
{
    auto context_id = Web::compositor_context_id_for_page(m_id);
    if (!m_is_open || client().page_id_for_compositor_context_id(context_id) != m_id)
        return;

    Application::the().notify_compositor_presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

void WebContentPage::fail_renderer_owned_downloads()
{
    auto& file_downloader = Application::the().file_downloader();
    for (auto download_id : exchange(m_renderer_owned_downloads, {}))
        file_downloader.fail_download(download_id, "Download process exited"_string);
}

void WebContentPage::close()
{
    PictureInPictureManager::the().close_window(*this);
    m_is_open = false;
    // NB: Nothing finishes the input this page held, including a page discarded while its process hosts other pages.
    if (m_traversable) {
        if (auto view = m_traversable->view(); view.has_value())
            view->did_lose_input_event_endpoint({}, *this);
    }
    m_needs_beforeunload_check = true;
    m_history_recorded_url_for_current_load.clear();
    if (m_client)
        m_client->update_audio_capture_permission();
    auto owed_replies = move(m_owed_replies);
    for (auto owed : owed_replies)
        owe_reply(owed);
}

void WebContentPage::owe_reply(OwedReply owed)
{
    if (m_is_open) {
        m_owed_replies.set(owed);
        return;
    }
    Core::deferred_invoke([traversable = m_traversable, owed] {
        if (traversable)
            traversable->did_not_receive_reply(owed);
    });
}

bool WebContentPage::take_owed_reply(OwedReply owed)
{
    return m_owed_replies.remove(owed);
}

static bool may_navigate_to_a_local_file(WebContentClient& requesting_client);

void WebContentPage::did_request_navigation_of_navigable(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId source_navigable_id, Web::HTML::CrossProcessId source_document_id, Web::HTML::PreparedNavigationDescriptor navigation)
{
    if (navigation.url.scheme() == "file"sv && !may_navigate_to_a_local_file(client()))
        return;

    auto target = traversable().find(navigable_id);
    if (!target.has_value())
        return;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
    // NB: The page ran the steps up to 7 for a sourceDocument it hosts. The UI process runs those guarding navigable
    //     again, with the sourceDocument and sandboxing flags it holds. sourceDocument can be in another tab, in this
    //     process's page for it.
    Optional<CanonicalNavigable&> source;
    client().for_each_page([&](WebContentPage& page) {
        auto navigable = page.traversable().find(source_navigable_id);
        if (!navigable.has_value() || navigable->active_document().id() != source_document_id || navigable->active_document().host() != &page)
            return IterationDecision::Continue;
        source = *navigable;
        return IterationDecision::Break;
    });
    if (!source.has_value())
        return;
    auto& source_document = source->active_document();
    auto const& fetch_client = navigation.source_snapshot_params.fetch_client;
    if (!fetch_client.has_value() || fetch_client->id != source_document.relevant_global_object().relevant_settings_object().id()) {
        client().did_misbehave("did_request_navigation_of_navigable"sv, "fetch client is not the source document's environment"sv);
        return;
    }

    // 6. Otherwise:
    //    2. If sourceDocument's node navigable is not allowed by sandboxing to navigate navigable given
    //       sourceSnapshotParams:
    //       2. Return.
    Web::InitiatorSourceSnapshot source_snapshot_params {
        .sandboxing_flags = navigation.source_snapshot_params.sandboxing_flags | source_document.active_sandboxing_flag_set(),
        .has_transient_activation = navigation.source_snapshot_params.has_transient_activation,
    };
    if (!source->allowed_by_sandboxing_to_navigate(*target, source_snapshot_params))
        return;

    //    3. Set initiatorOriginSnapshot to sourceDocument's origin.
    navigation.initiator_origin_snapshot = source_document.origin();

    target->begin_navigation(move(navigation));
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
    auto navigable = traversable().find(navigable_id);
    if (!navigable.has_value() || !holds_container_of(*navigable, "did_request_focusing_steps_for_navigable"sv))
        return;

    // The focusing steps for a navigable container go on in the process hosting its content navigable's document.
    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    // The requesting page has moved focus to the container, so the tab's focused navigable is the content navigable
    // from here on. Recording it before the host runs the steps keeps a report the requesting page makes meanwhile
    // ahead of the host's, which only confirms this.
    traversable().set_focused_navigable(*navigable, *this);
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
    // of a navigable the requesting process hosts. The request comes through the page holding the target's tab, which
    // is not the opener's when the two are different tabs.
    if (!client().hosted_navigable(opener_navigable_id).has_value())
        return;

    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_set_opener_of_navigable(navigable_id, opener_navigable_id);
}

void WebContentPage::did_completely_finish_loading(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId document_id)
{
    // Only the process hosting a document speaks for it.
    auto navigable = traversable().find(navigable_id);
    if (!navigable.has_value())
        return;
    auto document = navigable->document_with_id(document_id);
    if (!document || document->host() != this)
        return;
    navigable->document_completely_finished_loading(*document);
}

void WebContentPage::did_create_populated_document_with_an_origin_of_its_own(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId document_id, Web::HTML::PopulatedDocumentOrigin origin, Web::HTML::EnvironmentId environment_id)
{
    if (auto navigable = traversable().find(navigable_id); navigable.has_value())
        navigable->did_create_populated_document_with_an_origin_of_its_own(*this, document_id, origin, environment_id);
}

void WebContentPage::did_create_child_frame(Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId container_document_id, Web::HTML::CrossProcessId frame_id, Web::HTML::HostedNavigableState hosted_state, Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry, URL::Origin origin, Web::HTML::EnvironmentId environment_id, Web::HTML::CrossProcessId document_id)
{
    auto& traversable = this->traversable();

    // The parent can be gone by the time its frame is reported: its own container was removed meanwhile.
    auto parent_navigable = traversable.find(parent_frame_id);
    if (!parent_navigable.has_value())
        return;

    auto container_document = parent_navigable->document_with_id(container_document_id);
    if (!container_document || container_document->host() != this) {
        client().did_misbehave("did_create_child_frame"sv, "frame created in a document the page does not hold"sv);
        return;
    }

    if (traversable.find(frame_id).has_value()) {
        client().did_misbehave("did_create_child_frame"sv, "frame created with an existing navigable's id"sv);
        return;
    }

    hosted_state.container.document_active_sandboxing_flag_set = container_document->active_sandboxing_flag_set();
    hosted_state.container.document_embedder_policy = container_document->embedder_policy();
    hosted_state.container.document_permissions_policy = container_document->permissions_policy();
    traversable.adopt_nested_history_for_created_child(*parent_navigable, *container_document, frame_id);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-new-child-navigable
    // NB: The process creating the navigable reports the entry that initializing it, in step 8, created.
    auto initial_entry = CanonicalSessionHistoryEntry::create_from_descriptor(Web::HTML::create_session_history_entry_descriptor(move(initial_history_entry), 0));
    if (initial_entry.is_error()) {
        client().did_misbehave("did_create_child_frame"sv, "invalid initial session history entry"sv);
        return;
    }

    // 2. Let group be element's node document's browsing context's top-level browsing context's group.
    auto group = container_document->browsing_context().top_level_browsing_context().group();

    // 3. Let browsingContext and document be the result of creating a new browsing context and document given element's node document, element, and group.
    auto document = CanonicalBrowsingContext::create_a_new_browsing_context_and_document(container_document.ptr(), hosted_state.container, *group, document_id, move(environment_id), move(origin)).document;

    // 6. Let documentState be a new document state, with
    //    document: document
    //    initiator origin: document's origin
    //    origin: document's origin
    //    navigable target name: targetName
    //    about base URL: document's about base URL
    // NB: The process creating the navigable holds documentState's other fields, and reports its ID with the navigable's
    //     state.
    auto& document_state = *initial_entry.value()->document_state;
    document_state.initiator_origin = document->origin();
    document_state.origin = document->origin();

    // 7. Let navigable be a new navigable.
    // 8. Initialize the navigable navigable given documentState and parentNavigable.
    traversable.insert(*this, *parent_navigable, *container_document, frame_id, move(hosted_state), initial_entry.release_value(), move(document));
}

void WebContentPage::did_set_browser_zoom(double factor)
{
    view().set_zoom(factor);
}

void WebContentPage::did_set_device_pixel_ratio_for_testing(double device_pixel_ratio)
{
    view().set_device_pixel_ratio_for_testing(*this, device_pixel_ratio);
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

void WebContentPage::did_set_text_fragment_indication_visibility(bool visible)
{
    if (displays_tab())
        view().set_text_fragment_indication_visibility({}, visible);
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
    view().did_enter_tooltip_area({}, move(title));
}

void WebContentPage::did_leave_tooltip_area()
{
    view().did_leave_tooltip_area({});
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

static Optional<URL::Origin> initiator_origin_snapshot(URL::Origin const& given_origin, Web::HTML::NavigationSourceSnapshot const& source_snapshot_params, WebContentClient& hosting_client);

// A page reaches local file content only by navigating there from local file content, or from the browser's UI: a
// process holding none could otherwise come to host a file: document, and with it read every file the user can.
static bool may_navigate_to_a_local_file(WebContentClient& requesting_client)
{
    return requesting_client.may_read_local_files();
}

// A navigation a page asked the browser's UI to start for it, with the initiator origin the UI process takes from the
// fetch client — as it does for a navigation the page starts itself, so a process names only a source it hosts.
static Optional<Web::HTML::PreparedNavigationDescriptor> navigation_from_page(WebContentClient& requesting_client, Web::HTML::PreparedNavigationDescriptor navigation)
{
    // A page's own click or context menu names a document the page's process hosts, so only that process vouches for it.
    auto initiator_origin = initiator_origin_snapshot(navigation.initiator_origin_snapshot, navigation.source_snapshot_params, requesting_client);
    if (!initiator_origin.has_value())
        return {};
    if (navigation.url.scheme() == "file"sv && !may_navigate_to_a_local_file(requesting_client))
        return {};
    navigation.initiator_origin_snapshot = initiator_origin.release_value();
    return navigation;
}

void WebContentPage::did_click_link(Web::HTML::PreparedNavigationDescriptor navigation, ByteString target, unsigned modifiers)
{
    auto verified_navigation = navigation_from_page(client(), move(navigation));
    if (!verified_navigation.has_value())
        return;
    // NB: A link the browser does not handle itself is an external application's. The page asking to open it is not
    //     the browser's UI, so the request takes the page's path, which honors only input the UI process saw.
    if (!is_url_handled_internally(verified_navigation->url)) {
        view().handle_external_url({}, verified_navigation->url, verified_navigation->initiator_origin_snapshot, verified_navigation->source_snapshot_params.has_transient_activation && has_transient_user_activation());
        return;
    }
    auto open_in_background = modifiers == Web::UIEvents::Mod_PlatformCtrl;
    auto open_in_foreground = modifiers == (Web::UIEvents::Mod_PlatformCtrl | Web::UIEvents::Mod_Shift);
    if (open_in_background || open_in_foreground || target == "_blank"sv) {
        // NB: The renderer sends a link to open in a new tab only for the user's own click, which the view delivered.
        //     That click opens the link whatever activation the page used up for other things, such as a popup its
        //     click handler opened.
        if (!view().take_input_to_open_link_in_new_tab({}))
            return;
        view().open_navigation_in_new_tab(verified_navigation.release_value(), open_in_background ? Web::HTML::ActivateTab::No : Web::HTML::ActivateTab::Yes);
    } else {
        view().load(verified_navigation.release_value());
    }
}

void WebContentPage::did_middle_click_link(Web::HTML::PreparedNavigationDescriptor navigation, ByteString, unsigned)
{
    auto verified_navigation = navigation_from_page(client(), move(navigation));
    if (!verified_navigation.has_value())
        return;
    // NB: A link the browser does not handle itself is an external application's. The page asking to open it is not
    //     the browser's UI, so the request takes the page's path, which honors only input the UI process saw.
    if (!is_url_handled_internally(verified_navigation->url)) {
        view().handle_external_url({}, verified_navigation->url, verified_navigation->initiator_origin_snapshot, verified_navigation->source_snapshot_params.has_transient_activation && has_transient_user_activation());
        return;
    }
    // NB: The renderer sends a link to open in a new tab only for the user's own click, which the view delivered.
    //     That click opens the link whatever activation the page used up for other things.
    if (!view().take_input_to_open_link_in_new_tab({}))
        return;
    view().open_navigation_in_new_tab(verified_navigation.release_value(), Web::HTML::ActivateTab::No);
}

void WebContentPage::did_request_external_url(URL::URL url, URL::Origin initiator_origin, bool has_transient_activation)
{
    // NB: The page's own claim of activation counts only along with input the UI process delivered to it.
    view().handle_external_url({}, move(url), move(initiator_origin), has_transient_activation && has_transient_user_activation());
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

void WebContentPage::did_get_accessibility_tree(Vector<AccessibilityNodeData> nodes)
{
    if (displays_tab()) {
        if (view().on_accessibility_tree_received)
            view().on_accessibility_tree_received(move(nodes));
    }
}

void WebContentPage::did_accessibility_focus_change(i64 focused_node_id)
{
    if (displays_tab()) {
        if (view().on_accessibility_focus_changed)
            view().on_accessibility_focus_changed(focused_node_id);
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

void WebContentPage::did_list_devtools_sources(u64 request_id, Vector<Web::HTML::ScriptRegistryDescription> sources)
{
    if (displays_tab()) {
        auto handler = view().on_received_devtools_sources.take(request_id);
        if (handler.has_value())
            (*handler)(move(sources));
    }
}

void WebContentPage::did_get_devtools_source(Web::HTML::ScriptRegistryIdentifier source_id, Optional<Web::HTML::ScriptRegistryContent> source)
{
    if (displays_tab()) {
        auto handler = view().on_received_devtools_source.take(source_id);
        if (handler.has_value())
            (*handler)(move(source));
    }
}

void WebContentPage::did_add_devtools_source(Web::HTML::ScriptRegistryDescription source)
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

void WebContentPage::did_block_resource()
{
    view().did_block_resource({});
}

void WebContentPage::did_start_network_request(u64 request_id, URL::URL url, ByteString method, Vector<HTTP::Header> request_headers, ByteBuffer request_body, Optional<String> initiator_type, String referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::RequestPriority priority)
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

void WebContentPage::did_request_document_cookie_version_index(Web::HTML::EnvironmentId environment_id, i64 document_id, String domain)
{
    auto environment = hosted_environment(environment_id);
    if (!environment.has_value() || environment->origin().is_opaque() || domain != environment->origin().host().serialize().to_ascii_lowercase())
        return;

    if (displays_tab()) {
        if (auto document_index = view().ensure_document_cookie_version_index({}, domain); !document_index.is_error())
            async_set_document_cookie_version_index(document_id, document_index.value());
    }
}

void WebContentPage::clear_storage_for_testing(Web::StorageAPI::StorageEndpointType storage_endpoint, Web::HTML::EnvironmentId const& environment_id)
{
    auto canonical_key = canonical_storage_key(environment_id);
    auto* storage_jar = this->storage_jar(storage_endpoint);
    if (canonical_key.has_value() && storage_jar)
        storage_jar->clear_storage_key(storage_endpoint, *canonical_key);
}

Messages::WebContentClient::ConnectToStorageAreaResponse WebContentPage::connect_to_storage_area(Web::StorageAPI::StorageEndpointType storage_endpoint, Web::HTML::EnvironmentId environment_id)
{
    auto canonical_key = canonical_storage_key(environment_id);
    auto* storage_jar = this->storage_jar(storage_endpoint);
    if (!canonical_key.has_value() || !storage_jar)
        return { {}, {}, {}, 0 };

    auto connection = StorageAreaRegistry::the().connect(client(), *storage_jar, storage_endpoint, *canonical_key);
    if (!connection.has_value())
        return { {}, {}, {}, 0 };

    Vector<Utf16String> keys;
    Vector<Utf16String> values;
    keys.ensure_capacity(connection->items.size());
    values.ensure_capacity(connection->items.size());
    for (auto& item : connection->items) {
        keys.unchecked_append(move(item.key));
        values.unchecked_append(move(item.value));
    }
    return { connection->id, move(keys), move(values), connection->usage };
}

void WebContentPage::did_change_needs_beforeunload_check(bool needs_beforeunload_check)
{
    m_needs_beforeunload_check = needs_beforeunload_check;
}

// https://html.spec.whatwg.org/multipage/interaction.html#transient-activation-duration
static constexpr auto transient_activation_duration = AK::Duration::from_seconds(5);

void WebContentPage::give_user_activation()
{
    m_last_user_activation = MonotonicTime::now();
}

bool WebContentPage::has_transient_user_activation() const
{
    // NB: Automation lets a renderer synthesize the input that gives it activation.
    if (WebContentClient::renderers_may_synthesize_input())
        return true;
    if (m_last_user_activation.has_value() && MonotonicTime::now() - *m_last_user_activation < transient_activation_duration)
        return true;
    auto view = traversable().view();
    return view.has_value() && view->is_delivering_user_activation_to(*this);
}

// NB: The UI process consumes its activation only where the renderer consumes the activation of its windows too, so
//     the two agree on what is left of it. As the renderer does, it consumes the activation every page of the tab has
//     from input they finished handling, along with that of the input the page is handling, which the renderer gave
//     the activation it consumes. A page whose activation comes only from input it has yet to finish handling consumes
//     that of the earliest such input. Input queued behind keeps its activation either way, as the renderer gives
//     activation anew for each press it handles.
bool WebContentPage::consume_transient_user_activation()
{
    if (!has_transient_user_activation())
        return false;

    auto has_activation_of_handled_input = m_last_user_activation.has_value() && MonotonicTime::now() - *m_last_user_activation < transient_activation_duration;
    auto& top_level_traversable = traversable().top_level_traversable();
    top_level_traversable.for_each_hosting_page([](WebContentPage& page) {
        page.m_last_user_activation.clear();
    });
    if (auto view = top_level_traversable.view(); view.has_value()) {
        if (has_activation_of_handled_input)
            view->consume_user_activation_of_input_handled_by({}, *this);
        else
            view->consume_user_activation_of_input_in_flight_to({}, *this);
    }
    return true;
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

// Only WebDriver's window commands and the internals object of a test ask for a change to the window.
static bool may_change_window(WebContentPage& page, StringView message)
{
    if (WebContentClient::renderers_may_synthesize_input())
        return true;
    page.client().did_misbehave(message, "not driven by WebDriver or a test"sv);
    return false;
}

void WebContentPage::did_request_restore_window()
{
    if (!may_change_window(*this, "did_request_restore_window"sv))
        return;
    if (view().on_restore_window)
        view().on_restore_window();
}

void WebContentPage::did_request_reposition_window(Gfx::IntPoint position, u64 completion_id)
{
    if (!may_change_window(*this, "did_request_reposition_window"sv))
        return;
    if (view().on_reposition_window)
        view().on_reposition_window(position);
    async_did_complete_window_rect_request(completion_id);
}

void WebContentPage::did_request_resize_window(Gfx::IntSize size, u64 completion_id)
{
    if (!may_change_window(*this, "did_request_resize_window"sv))
        return;
    if (view().on_resize_window)
        view().on_resize_window(size);
    async_did_complete_window_rect_request(completion_id);
}

void WebContentPage::did_request_maximize_window(u64 completion_id)
{
    if (!may_change_window(*this, "did_request_maximize_window"sv))
        return;
    if (view().on_maximize_window)
        view().on_maximize_window();
    async_did_complete_window_rect_request(completion_id);
}

void WebContentPage::did_request_minimize_window()
{
    if (!may_change_window(*this, "did_request_minimize_window"sv))
        return;
    if (view().on_minimize_window)
        view().on_minimize_window();
}

void WebContentPage::did_request_fullscreen_window()
{
    // Element fullscreen needs and consumes the activation of the user's input, which the renderer checks. A
    // compromised renderer skips that check. The process of an isolated iframe asks for the window of its tab itself.
    if (!consume_transient_user_activation()) {
        async_deny_fullscreen_window();
        return;
    }
    if (view().on_fullscreen_window)
        view().on_fullscreen_window();
}

void WebContentPage::did_request_exit_fullscreen()
{
    if (view().on_exit_fullscreen_window)
        view().on_exit_fullscreen_window();
}

void WebContentPage::did_exit_picture_in_picture()
{
    PictureInPictureManager::the().close_window(*this);
}

void WebContentPage::did_change_picture_in_picture_video_size(Gfx::IntSize video_size)
{
    PictureInPictureManager::the().video_size_did_change(*this, video_size);
}

void WebContentPage::did_change_picture_in_picture_player_controls_visibility(bool shown)
{
    view().did_change_picture_in_picture_player_controls_visibility({}, shown);
}

// A navigation's population steps run in the process with its source document, which need not have hosted local file
// content. For a navigation to a file: URL the UI process admitted, that process reads the file the URL names, as it does
// for a history step to an entry of one.
bool WebContentPage::populates_a_navigation_to_local_file(ByteString const& path) const
{
    if (traversable().page_populates_a_history_entry_for_local_file(*this, path))
        return true;

    bool result = false;
    traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        auto navigation = navigable.uncommitted_navigation();
        result = navigation
            && navigation->worker() == this
            && navigation->url.scheme() == "file"sv
            && navigation->url.file_path() == path;
        return result ? IterationDecision::Break : IterationDecision::Continue;
    });
    return result;
}

void WebContentPage::did_request_file(ByteString path, i32 request_id)
{
    // NB: The renderer's sandbox keeps it out of the local file system, so this is the only way it can read a file.
    //     Answer only a process that has hosted local file content, or one reading the file a navigation it populates
    //     names: any other could read every file the user can.
    if (!client().may_read_local_files() && !populates_a_navigation_to_local_file(path)) {
        async_handle_file_return(EACCES, {}, request_id);
        return;
    }

    auto file = open_local_file_for_renderer(path);
    if (file.is_error())
        async_handle_file_return(file.error().code(), {}, request_id);
    else
        async_handle_file_return(0, file.release_value(), request_id);
}

void WebContentPage::did_request_color_picker(Color current_color)
{
    view().did_request_color_picker({}, *this, current_color);
}

bool WebContentPage::hosts_a_document_allowed_to_use(Web::PermissionsPolicy::PolicyControlledFeature feature) const
{
    auto is_allowed = false;
    traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        auto const& document = navigable.active_document();
        if (document.host() != this)
            return IterationDecision::Continue;
        auto value = Web::PermissionsPolicy::check_permissions_policy(document.permissions_policy(), feature, document.origin(), document.origin());
        is_allowed = value == Web::PermissionsPolicy::FeatureValue::Enabled;
        return is_allowed ? IterationDecision::Break : IterationDecision::Continue;
    });
    return is_allowed;
}

void WebContentPage::did_request_geolocation_position(u64 request_id)
{
    if (!hosts_a_document_allowed_to_use(Web::PermissionsPolicy::PolicyControlledFeature::Geolocation)) {
        async_geolocation_position_response(request_id, {}, to_underlying(Web::Geolocation::GeolocationPositionErrorCode::PermissionDenied));
        return;
    }
    if (view().on_request_geolocation_position)
        view().on_request_geolocation_position(*this, request_id);
}

void WebContentPage::did_request_permission(u64 request_id, Utf16String name)
{
    if (name != "microphone"_utf16) {
        client().did_misbehave("did_request_permission"sv, "unknown permission name"sv);
        return;
    }

    if (!view().on_request_permission) {
        async_permission_request_completed(request_id, false);
        return;
    }

    auto url = view().url();
    view().on_request_permission(name, url.origin(), [page = NonnullRefPtr(*this), request_id, name, url](bool granted) {
        if (!page->is_open())
            return;
        // The AudioServer hears of the permission before the page does, so the page's first request finds it in place.
        if (granted && name == "microphone"_utf16) {
            if (auto result = page->client().grant_audio_capture(url); result.is_error()) {
                warnln("Unable to grant audio capture: {}", result.error());
                granted = false;
            }
        }
        page->async_permission_request_completed(request_id, granted);
    });
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
    relay_finished_input_event(*this, event_id, event_result);
}

void WebContentPage::relay_finished_input_event(WebContentPage const& reporting_page, u64 event_id, Web::EventResult event_result)
{
    if (displays_tab()) {
        view().did_finish_handling_input_event({}, reporting_page, event_id, event_result);
        return;
    }

    // The view displaying the tab handed the event down; it hears the result.
    if (auto display_page = traversable().display_page(); display_page) {
        if (display_page->is_open() && display_page.ptr() != this)
            display_page->relay_finished_input_event(reporting_page, event_id, event_result);
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

// NB: The system clipboard belongs to the user, so a page reaches it only right after the user's own input to it, as
//     with a copy or paste shortcut, a click on a page control, or the browser's Paste command.
void WebContentPage::did_insert_clipboard_item(Web::Clipboard::SystemClipboardItem item, String)
{
    if (!has_transient_user_activation())
        return;
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
    // NB: The event enters the tab's input path as the user's own, reaching the focused navigable in whatever process
    //     hosts it and granting user activation, so only automation may ask for one.
    if (!WebContentClient::renderers_may_synthesize_input()) {
        client().did_misbehave("did_request_key_event_for_testing"sv, "not driven by WebDriver or a test"sv);
        return;
    }
    // Only the compositor sets this, on its way to WebContent.
    event.async_scroll_performed_default_action = false;
    view().enqueue_input_event(move(event));
}

void WebContentPage::request_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters parameters)
{
    // Only the process hosting a navigable's active document appends its synchronous navigation steps.
    if (auto const* same_document = parameters.get_pointer<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>(); same_document && !hosted_navigable(same_document->navigable_id).has_value()) {
        async_complete_history_operation(operation_id, {}, traversable().session_history().size());
        return;
    }
    if (auto const* resume = parameters.get_pointer<Web::ResumeTraverseHistoryOperationParameters>(); resume && !traversable().session_history().get_the_used_step(resume->target_step).has_value()) {
        client().did_misbehave("request_history_operation"sv, "resumed traversal to a step before every used step"sv);
        return;
    }
    traversable().enqueue_history_operation(operation_id, move(parameters), *this);
}

void WebContentPage::revoke_history_operation(Web::HTML::CrossProcessId operation_id)
{
    traversable().revoke_history_operation(*this, operation_id);
}

void WebContentPage::beforeunload_check_result(Web::HTML::CrossProcessId operation_id, Web::HTML::CheckIfUnloadingIsCanceledResult result, Web::HTML::UnloadPromptShown unload_prompt_shown)
{
    traversable().did_receive_beforeunload_check_result(*this, operation_id, result, unload_prompt_shown);
}

void WebContentPage::changing_navigable_history_job_ready(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition, bool update_only, Optional<Web::HTML::CrossProcessId> target_document_id)
{
    traversable().did_receive_changing_navigable_history_job_ready(*this, operation_id, navigable_id, disposition, update_only, target_document_id);
}

void WebContentPage::descendant_unload_task_complete(Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id)
{
    traversable().did_receive_descendant_unload_task_complete(*this, unload_id, navigable_id);
}

void WebContentPage::request_navigable_document_abort(Web::HTML::CrossProcessId navigable_id)
{
    auto navigable = traversable().find(navigable_id);
    if (!navigable.has_value() || !holds_container_of(*navigable, "request_navigable_document_abort"sv))
        return;
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

// The container of a navigable is in the page hosting its parent, which runs the remaining steps of the request.
// The request waits in the page hosting the requesting navigable's document until every page above has answered.
void WebContentPage::request_navigable_container_fullscreen(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId requesting_navigable_id, Web::Fullscreen::RequestType request_type)
{
    if (auto host = page_hosting_container_of(navigable_id))
        host->async_fullscreen_navigable_container(navigable_id, requesting_navigable_id, request_type);
    else
        navigable_container_fullscreen_complete(requesting_navigable_id);
}

void WebContentPage::navigable_container_fullscreen_complete(Web::HTML::CrossProcessId requesting_navigable_id)
{
    if (auto host = page_hosting_navigable(requesting_navigable_id))
        host->async_container_fullscreen_complete(requesting_navigable_id);
}

void WebContentPage::request_navigable_container_unfullscreen(Web::HTML::CrossProcessId navigable_id)
{
    if (auto host = page_hosting_container_of(navigable_id))
        host->async_unfullscreen_navigable_container(navigable_id);
    else
        navigable_container_unfullscreen_complete(navigable_id);
}

void WebContentPage::navigable_container_unfullscreen_complete(Web::HTML::CrossProcessId navigable_id)
{
    if (auto host = page_hosting_navigable(navigable_id))
        host->async_container_unfullscreen_complete(navigable_id);
}

// The document of the tab's top-level traversable runs the steps, wherever a close request reached fullscreen.
void WebContentPage::request_fully_exit_fullscreen()
{
    view().exit_fullscreen();
}

RefPtr<WebContentPage> WebContentPage::page_hosting_container_of(Web::HTML::CrossProcessId navigable_id) const
{
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value() || !navigable->parent())
        return {};
    auto host = traversable().page_hosting(*navigable->parent());
    if (!host || !host->is_open())
        return {};
    return host;
}

RefPtr<WebContentPage> WebContentPage::page_hosting_navigable(Web::HTML::CrossProcessId navigable_id) const
{
    auto navigable = traversable().find(navigable_id);
    if (!navigable.has_value())
        return {};
    auto host = traversable().page_hosting(*navigable);
    if (!host || !host->is_open())
        return {};
    return host;
}

void WebContentPage::request_child_navigable_unload(Web::HTML::CrossProcessId navigable_id)
{
    if (auto navigable = traversable().find(navigable_id); navigable.has_value() && !holds_container_of(*navigable, "request_child_navigable_unload"sv))
        return;
    traversable().did_receive_child_navigable_unload_request(*this, navigable_id);
}

void WebContentPage::changing_navigable_continuation_applied(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::HostedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state)
{
    traversable().did_receive_changing_navigable_continuation_applied(*this, operation_id, navigable_id, move(activated_navigable_state), move(previous_entry_persisted_state));
}

void WebContentPage::nonchanging_navigable_history_state_updated(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id)
{
    traversable().did_receive_nonchanging_navigable_history_state_updated(*this, operation_id, navigable_id);
}

void WebContentPage::did_request_close_of_traversable(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId source_navigable_id, Web::HTML::NavigationSourceSnapshot source_snapshot)
{
    // window.close() from a document another process hosts: the process hosting the traversable's document runs the
    // rest of its steps.
    if (traversable().id() != navigable_id)
        return;

    // The process closing it must host the navigable it closes from. The request comes through the page holding the
    // traversable's tab, which is not the source's when the two are different tabs.
    Optional<CanonicalNavigable&> source;
    RefPtr<WebContentPage> source_page;
    client().for_each_page([&](WebContentPage& page) {
        source = page.hosted_navigable(source_navigable_id);
        if (!source.has_value())
            return IterationDecision::Continue;
        source_page = page;
        return IterationDecision::Break;
    });
    if (!source.has_value())
        return;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-window-close
    // 6. If all the following are true:
    //    - thisTraversable is script-closable;
    //    - the incumbent global object's browsing context is familiar with browsingContext; and
    //    - the incumbent global object's navigable is allowed by sandboxing to navigate thisTraversable, given sourceSnapshotParams,
    // NB: The requesting process found these true. The UI process checks them again with the state it holds, including
    //     the sandboxing flags of the incumbent global object's associated Document and the activation of the input it
    //     delivered to the page hosting it.
    Web::InitiatorSourceSnapshot source_snapshot_params {
        .sandboxing_flags = source_snapshot.sandboxing_flags | source->active_document().active_sandboxing_flag_set(),
        .has_transient_activation = source_snapshot.has_transient_activation && source_page->has_transient_user_activation(),
    };
    if (!traversable().is_script_closable() || !source->is_familiar_with(traversable()) || !source->allowed_by_sandboxing_to_navigate(traversable(), source_snapshot_params))
        return;

    auto endpoint = endpoint_hosting_navigable_represented_by(navigable_id);
    if (!endpoint)
        return;
    endpoint->async_close_traversable_from_script(navigable_id);
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

void WebContentPage::did_receive_network_response_headers(u64 request_id, u32 status_code, Optional<String> reason_phrase, Vector<HTTP::Header> response_headers, Requests::CacheState cache_state)
{
    if (displays_tab()) {
        if (view().on_network_response_headers_received)
            view().on_network_response_headers_received(request_id, status_code, reason_phrase, response_headers, cache_state);
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
    if (!has_transient_user_activation()) {
        async_retrieved_clipboard_entries(request_id, items);
        return;
    }
    if (auto item = view().clipboard_item(); !item.system_clipboard_representations.is_empty())
        items.append(move(item));

    async_retrieved_clipboard_entries(request_id, items);
}

void WebContentPage::did_request_spell_check(u64 request_id, Utf16String text)
{
    view().did_request_spell_check({}, *this, request_id, move(text));
}

void WebContentPage::did_request_set_system_focus(bool has_system_focus)
{
    traversable().set_has_system_focus(has_system_focus, *this);
}

void WebContentPage::did_request_set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    view().set_system_visibility_state(visibility_state);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
// NB: initiatorOriginSnapshot is sourceDocument's origin. The process names sourceDocument's relevant settings object as
//     the fetch client, an environment that a process hosts, and the UI process takes the origin from it. Without a
//     sourceDocument, the process gives a new opaque origin, which no document may hold yet.
// The origin of the environment a navigation names as its fetch client, from the process asking, which hosts it.
static Optional<URL::Origin> initiator_origin_snapshot(URL::Origin const& given_origin, Web::HTML::NavigationSourceSnapshot const& source_snapshot_params, WebContentClient& hosting_client)
{
    if (!source_snapshot_params.fetch_client.has_value()) {
        if (!given_origin.is_opaque() || CanonicalTraversable::is_origin_held_by_a_document(given_origin))
            return {};
        return given_origin;
    }
    auto source_settings = hosting_client.hosted_environment(source_snapshot_params.fetch_client->id);
    if (!source_settings.has_value())
        return {};
    return source_settings->origin();
}

void WebContentPage::did_request_navigation_start(Web::HTML::CrossProcessId navigable_id, Web::NavigationTarget target, URL::URL url, Utf16String navigation_id, Optional<Web::HTML::NavigationStartRequest> start_request)
{
    CanonicalNavigable* target_navigable = &traversable();
    if (target == Web::NavigationTarget::IFrame) {
        auto child_frame = traversable().top_level_traversable().find(navigable_id);
        target_navigable = child_frame.has_value() ? &*child_frame : nullptr;
    }

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
    // 18. If navigable's ongoing navigation is "traversal", then:
    //     2. Return.
    // NB: The page ran this step against the ongoing navigation it knew of, which the traversal queue can have set to
    //     "traversal" since.
    auto navigation_is_blocked_by_history_traversal = target_navigable
        && target_navigable->ongoing_navigation().has<CanonicalNavigable::Traversal>();
    Optional<URL::Origin> initiator_origin;
    if (start_request.has_value())
        initiator_origin = initiator_origin_snapshot(start_request->initiator_origin, start_request->source_snapshot_params, client());
    if (!target_navigable
        || target_navigable->id() != navigable_id
        || !traversable().hosts(*target_navigable, *this)
        || (start_request.has_value() && start_request->navigable_id != navigable_id)
        || (start_request.has_value() && !initiator_origin.has_value())
        || navigation_is_blocked_by_history_traversal) {
        async_cancel_navigation_params_creation(navigable_id, navigation_id);
        return;
    }
    // NB: Population takes the start request's URL, so it must be the URL this check and the admission record name.
    if (start_request.has_value() && start_request->url != url) {
        client().did_misbehave("did_request_navigation_start"sv, "start request names another URL"sv);
        return;
    }
    if (start_request.has_value() && url.scheme() == "file"sv && !may_navigate_to_a_local_file(client())) {
        async_cancel_navigation_params_creation(navigable_id, navigation_id);
        return;
    }
    if (start_request.has_value())
        start_request->initiator_origin = initiator_origin.release_value();

    if (!start_request.has_value()) {
        if (url.scheme() != "javascript"sv) {
            client().did_misbehave("did_request_navigation_start"sv, "navigation without a start request is not to a javascript: URL"sv);
            return;
        }
        target_navigable->begin_evaluating_javascript_url(url, navigation_id, *this);
        return;
    }

    auto retry = prepare_navigation_to_retry(*start_request);
    target_navigable->set_ongoing_navigation(CanonicalNavigation::Start {
        .url = move(url),
        .navigation_id = navigation_id,
        .retry = move(retry),
        .sequence_number = target_navigable->top_level_traversable().next_sequence_number(),
        .state = CanonicalNavigation::CheckingIfUnloadingIsCanceled { start_request.release_value() },
        .page = this,
    });
    begin_navigation_unload_check(*target_navigable, navigation_id);
}

// Navigate, step 23.1: checking if unloading is canceled for navigable's active document's inclusive descendant
// navigables. The pages hosting the documents this page, the population worker, does not run their checks first; this
// page's own, with the navigation's bookkeeping, comes last, told whether a prompt was shown.
void WebContentPage::begin_navigation_unload_check(CanonicalNavigable& target_navigable, Utf16String const& navigation_id)
{
    Vector<Web::HTML::CrossProcessId> inclusive_descendants;
    target_navigable.for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        inclusive_descendants.append(navigable.id());
        return IterationDecision::Continue;
    });
    (void)target_navigable.top_level_traversable().check_if_unloading_is_canceled(move(inclusive_descendants), {}, {}, *this, Web::HTML::UnloadPromptShown::No,
        [page = NonnullRefPtr<WebContentPage>(*this), navigable_id = target_navigable.id(), navigation_id](Web::HTML::CheckIfUnloadingIsCanceledResult result, Web::HTML::UnloadPromptShown unload_prompt_shown) {
            if (result != Web::HTML::CheckIfUnloadingIsCanceledResult::Continue) {
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

    auto ongoing_navigation = navigable->navigation_with_id(navigation_id);
    auto* checking = ongoing_navigation ? ongoing_navigation->state.get_pointer<CanonicalNavigation::CheckingIfUnloadingIsCanceled>() : nullptr;
    if (!checking || ongoing_navigation->worker() != this)
        return;

    auto population_request = Web::HTML::create_navigation_population_request(
        move(checking->start_request),
        Application::the().allocate_ui_process_cross_process_id());
    population_request.target_snapshot_params.sandboxing_flags |= navigable->snapshot_target_snapshot_params().sandboxing_flags;
    ongoing_navigation->history_entry = MUST(CanonicalSessionHistoryEntry::create_from_descriptor(Web::HTML::create_session_history_entry_descriptor(population_request.history_entry, 0)));
    ongoing_navigation->state = CanonicalNavigation::CreatingNavigationParams { NavigationLoader::create(client().is_private(), move(population_request)) };
    async_create_navigation_params(ongoing_navigation->loader()->request());

    // Requesting navigation params starts the fetch, so a view's top-level population begins its recorded
    // load here.
    if (navigable->is_top_level_traversable()) {
        if (displays_tab())
            begin_top_level_load(ongoing_navigation->loader()->request().history_entry.url);
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

    auto ongoing_navigation = navigable->navigation_with_id(navigation_id);
    auto* creating = ongoing_navigation ? ongoing_navigation->state.get_pointer<CanonicalNavigation::CreatingNavigationParams>() : nullptr;
    if (!creating || ongoing_navigation->worker() != this) {
        if (result.has_value())
            NavigationLoader::discard(client().is_private(), *result);
        return;
    }

    if (!result.has_value()) {
        navigable->did_cancel_navigation(navigation_id);
        return;
    }

    // Steps 1-4 have produced final navigation params. Keep the pending entry in
    // sync with redirects before choosing the process that will run step 5.
    auto loader = move(creating->loader);
    ongoing_navigation->state = CanonicalNavigation::AcquiringResponseBody { move(loader) };
    NonnullRefPtr acquiring_loader = *ongoing_navigation->loader();
    acquiring_loader->did_finish_navigation_params_creation(result.release_value());
    if (ongoing_navigation->history_entry->update_from_descriptor(Web::HTML::create_session_history_entry_descriptor(acquiring_loader->request().history_entry, 0)).is_error()) {
        auto rejected_result = acquiring_loader->take_result();
        NavigationLoader::discard(client().is_private(), rejected_result);
        navigable->did_cancel_navigation(navigation_id);
        client().did_misbehave("did_finish_navigation_params_creation"sv, "replacement document state forms a cycle"sv);
        return;
    }
    ongoing_navigation->url = ongoing_navigation->history_entry->url;

    acquiring_loader->acquire_response_body([page = NonnullRefPtr<WebContentPage>(*this), navigable_id, navigation_id = move(navigation_id)](bool succeeded) {
        // The page may have closed while the body was fetched; its navigation then has nothing left to finish.
        if (!page->is_open())
            return;
        if (succeeded && page->continue_navigation_population_in_selected_process(navigable_id, navigation_id))
            return;
        if (auto navigable = page->population_worker_navigable(navigable_id); navigable.has_value())
            navigable->did_cancel_navigation(navigation_id);
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
    auto ongoing_navigation = navigable->navigation_with_id(navigation_id);
    if (!ongoing_navigation
        || ongoing_navigation->is_finalizing()
        || (ongoing_navigation->worker() != this && ongoing_navigation->host() != this)) {
        return;
    }

    // Only a failed population handoff owns the loader's response body.
    if (ongoing_navigation->state.has<CanonicalNavigation::Populating>() && ongoing_navigation->host() == this)
        ongoing_navigation->loader()->reclaim_response_body_after_failed_handoff();

    m_history_recorded_url_for_current_load.clear();
    navigable->did_cancel_navigation(move(navigation_id));
}

void WebContentPage::did_change_hosted_navigable_state(Web::HTML::CrossProcessId navigable_id, Web::HTML::HostedNavigableState state)
{
    // Only the process hosting a navigable's active document speaks for its state.
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value())
        return;

    // A page standing in for a document the navigable lost, as the page displaying the tab after its process crashed
    // does, does not speak for that document.
    if (navigable->active_document().host() != this)
        return;

    navigable->update_hosted_state(move(state));
}

void WebContentPage::did_set_opener_browsing_context(Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::CrossProcessId> opener_navigable_id)
{
    // Only the process hosting a navigable's active document speaks for its browsing context.
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value() || navigable->active_document().host() != this)
        return;
    navigable->did_set_opener_browsing_context(opener_navigable_id);
}

void WebContentPage::did_change_navigable_container_state(Web::HTML::CrossProcessId navigable_id, Web::HTML::ReplicatedContainerState state)
{
    auto navigable = traversable().find(navigable_id);
    if (!navigable.has_value() || !holds_container_of(*navigable, "did_change_navigable_container_state"sv))
        return;
    navigable->update_container_state(move(state));
}

void WebContentPage::did_update_frame_stats_displayed(Web::HTML::CrossProcessId frame_id, bool displayed)
{
    if (!Core::StatsLog::the())
        return;
    auto child_frame = traversable().find(frame_id);
    if (!child_frame.has_value() || !holds_container_of(*child_frame, "did_update_frame_stats_displayed"sv))
        return;
    child_frame->set_stats_displayed(displayed);
}

void WebContentPage::did_update_child_frame_viewport(Web::HTML::CrossProcessId frame_id, Web::DevicePixelRect viewport_rect, Web::DevicePixelRect viewport_intersection, double device_pixel_ratio)
{
    auto child_frame = traversable().find(frame_id);
    if (!child_frame.has_value() || !holds_container_of(*child_frame, "did_update_child_frame_viewport"sv))
        return;
    child_frame->set_viewport(viewport_rect, viewport_intersection, device_pixel_ratio);
}

// The page found the pointer over a child frame another process hosts, and hands the event down to that process. The
// view displaying the tab waits for the event until the process it went to finishes it.
void WebContentPage::did_forward_mouse_event_to_child_frame(Web::HTML::CrossProcessId frame_id, Web::MouseEvent event)
{
    auto child_frame = traversable().find(frame_id);
    if (!child_frame.has_value() || !holds_container_of(*child_frame, "did_forward_mouse_event_to_child_frame"sv) || !child_frame->has_remote_host()) {
        did_finish_handling_input_event(event.id, Web::EventResult::Dropped);
        return;
    }

    auto& host = child_frame->remote_host();
    view().did_forward_input_event({}, *this, event.id, host);
    host.async_mouse_event_in_hosted_root(frame_id, move(event));
}

void WebContentPage::did_destroy_child_frame(Web::HTML::CrossProcessId frame_id)
{
    auto child_frame = traversable().find(frame_id);
    if (!child_frame.has_value() || !holds_container_of(*child_frame, "did_destroy_child_frame"sv))
        return;
    traversable().remove(*child_frame);
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

    m_renderer_owned_downloads.set(download_id);

    file_downloader.set_cancel_callback(download_id, [page = NonnullRefPtr<WebContentPage>(*this), download_id] {
        if (!page->is_open())
            return;
        page->m_renderer_owned_downloads.remove(download_id);
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
        auto ongoing_navigation = navigation_id.has_value() ? navigable->navigation_with_id(*navigation_id) : nullptr;
        if (ongoing_navigation
            && (ongoing_navigation->state.has<CanonicalNavigation::CreatingNavigationParams>() || ongoing_navigation->state.has<CanonicalNavigation::Populating>())
            && ongoing_navigation->loader()->response_body_matches(request_server_client_id, request_server_request_id)) {
            matches_in_flight_navigation = true;
            navigable->did_cancel_navigation(*navigation_id);
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
    if (!m_renderer_owned_downloads.contains(download_id))
        return;

    Application::the().file_downloader().append_download_data(download_id, data.bytes());
}

void WebContentPage::did_finish_download(u64 download_id)
{
    if (!m_renderer_owned_downloads.contains(download_id))
        return;

    m_renderer_owned_downloads.remove(download_id);
    Application::the().file_downloader().finish_download(download_id);
}

void WebContentPage::did_fail_download(u64 download_id, String error)
{
    if (!m_renderer_owned_downloads.contains(download_id))
        return;

    m_renderer_owned_downloads.remove(download_id);
    Application::the().file_downloader().fail_download(download_id, move(error));
}

void WebContentPage::did_finish_loading(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId document_id)
{
    // NB: Only the page hosting the active document finishes loading it. A page standing in for a document the
    //     navigable lost, as the page displaying the tab after its process crashed does, finishes loading the stand-in;
    //     that must not surface in the view.
    auto navigable = traversable().find(navigable_id);
    if (!navigable.has_value() || navigable->active_document().host() != this)
        return;

    if (navigable->active_document().id() != document_id)
        return;

    if (navigable->tracked_load_navigation_id() != navigable->active_document_load().navigation_id)
        return;

    navigable->clear_active_document_load();
    if (!navigable->is_top_level_traversable())
        return;

    if (displays_tab()) {
        auto const& committed_url = view().url();

        // The WebUI channel goes to the process the UI process gave the WebUI's document to, never by a URL a process
        // reports. A page that loads anything else loses its channel.
        if (auto const& web_ui_host = navigable->active_document().web_ui_host(); web_ui_host.has_value() && client().hosted_web_ui() == web_ui_host) {
            if (auto web_ui = WebUI::create(client(), m_id, *web_ui_host); web_ui.is_error())
                warnln("Could not create WebUI for {}: {}", committed_url, web_ui.error());
            else
                client().set_web_ui(web_ui.release_value());
        } else if (auto const& web_ui = client().web_ui(); web_ui && web_ui->page_id() == m_id) {
            client().set_web_ui(nullptr);
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
    if (auto process = WebView::Application::process_manager().find_child(client().child_id()); process.has_value())
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

void WebContentPage::did_request_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, Web::ContextMenuForInputEventsTarget for_input_events_target, Optional<Utf16String> misspelled_word, Optional<u64> text_fragment_generation_id)
{
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_page_context_menu({}, *this, target->position, for_input_events_target, move(misspelled_word), text_fragment_generation_id);
}

void WebContentPage::did_request_spelling_menu_action_for_testing(Web::SpellingMenuActionForTesting action, u32 guess_index)
{
    // NB: This activates the tab's own menu actions, so only automation may ask for it.
    if (!WebContentClient::renderers_may_synthesize_input()) {
        client().did_misbehave("did_request_spelling_menu_action_for_testing"sv, "not driven by WebDriver or a test"sv);
        return;
    }
    view().activate_spelling_menu_action_for_testing({}, *this, action, guess_index);
}

void WebContentPage::did_request_link_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, Web::HTML::PreparedNavigationDescriptor navigation, ByteString, unsigned, Optional<u64> text_fragment_generation_id)
{
    auto verified_navigation = navigation_from_page(client(), move(navigation));
    if (!verified_navigation.has_value())
        return;
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_link_context_menu({}, target->position, verified_navigation.release_value(), text_fragment_generation_id);
}

void WebContentPage::did_generate_text_fragment_url(u64 request_id, Optional<URL::URL> url)
{
    view().did_generate_text_fragment_url(request_id, move(url));
}

void WebContentPage::did_request_image_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, Web::HTML::PreparedNavigationDescriptor navigation, ByteString, unsigned, Optional<Gfx::ShareableBitmap> bitmap)
{
    auto verified_navigation = navigation_from_page(client(), move(navigation));
    if (!verified_navigation.has_value())
        return;
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_image_context_menu({}, target->position, verified_navigation.release_value(), move(bitmap));
}

void WebContentPage::did_request_media_context_menu(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, ByteString, unsigned, Web::MediaContextMenu menu, Optional<Web::HTML::PreparedNavigationDescriptor> navigation)
{
    Optional<Web::HTML::PreparedNavigationDescriptor> verified_navigation;
    if (navigation.has_value()) {
        verified_navigation = navigation_from_page(client(), navigation.release_value());
        if (!verified_navigation.has_value())
            return;
    }
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_media_context_menu({}, *this, target->position, move(menu), move(verified_navigation));
}

void WebContentPage::did_get_highlighted_source(String html)
{
    if (auto new_tab = Application::the().open_blank_new_tab(Web::HTML::ActivateTab::Yes); new_tab.has_value())
        new_tab->load_html(html);
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
    did_open_dialog(Web::PendingDialog::Alert, message);
    view().did_open_user_prompt({});
    if (view().on_request_alert)
        view().on_request_alert(message);
}

void WebContentPage::did_request_before_unload(String source)
{
    did_open_dialog(Web::PendingDialog::BeforeUnload, {});
    auto weak_traversable = m_traversable;
    auto origin_page_id = id();
    auto close_prompt = [weak_traversable, origin_page_id](bool accepted) {
        if (!weak_traversable)
            return;

        weak_traversable->for_each_hosting_page([&](WebContentPage& page) {
            if (page.id() != origin_page_id)
                page.async_did_open_dialog_in_another_process(Web::PendingDialog::None, Utf16View {});
            else
                page.routed_proxy()->async_before_unload_closed(origin_page_id, accepted);
        });
    };

    if (view().on_request_before_unload) {
        view().on_request_before_unload(source, move(close_prompt));
        return;
    }

    close_prompt(true);
}

void WebContentPage::did_request_confirm(Utf16String message)
{
    did_open_dialog(Web::PendingDialog::Confirm, message);
    view().did_open_user_prompt({});
    if (view().on_request_confirm)
        view().on_request_confirm(message);
}

void WebContentPage::did_request_prompt(Utf16String message, Utf16String default_)
{
    did_open_dialog(Web::PendingDialog::Prompt, message);
    view().did_open_user_prompt({});
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

Messages::WebContentClient::DidRequestNewWebViewResponse WebContentPage::did_request_new_web_view(Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, Optional<Web::HTML::CrossProcessId> opener_navigable_id, Optional<URL::URL> opener_base_url, Utf16String target_name, Web::HTML::SandboxingFlagSet popup_sandboxing_flag_set)
{
    // The opener is a navigable the requesting page hosts. A request naming one it does not is refused before the
    // chrome is asked for a view, so nothing is created for a traversable that never will be.
    Optional<CanonicalNavigable&> opener;
    if (opener_navigable_id.has_value()) {
        opener = hosted_navigable(*opener_navigable_id);
        if (!opener.has_value())
            return { {}, {}, {}, {}, {}, {}, Web::HTML::VisibilityState::Hidden, {} };
    }

    Optional<CanonicalNavigable&> picture_in_picture_navigable;
    if (hints.picture_in_picture_window.has_value()) {
        picture_in_picture_navigable = hosted_navigable(hints.picture_in_picture_window->navigable_id);
        if (!picture_in_picture_navigable.has_value())
            return { {}, {}, {}, {}, {}, {}, Web::HTML::VisibilityState::Hidden, {} };

        // https://w3c.github.io/picture-in-picture/#request-picture-in-picture
        // A window needs and consumes the activation of the user's input, which the renderer checks, unless the document
        // asking for it has one open. A compromised renderer skips that check, so whatever the popup setting, the UI
        // process checks the activation of input it delivered, and the window it opened for the navigable's document.
        if (!PictureInPictureManager::the().document_has_open_window(*picture_in_picture_navigable) && !consume_transient_user_activation())
            return { {}, {}, {}, {}, {}, {}, Web::HTML::VisibilityState::Hidden, {} };
    } else {
        // The popup blocker runs in the renderer, which a compromised renderer skips. Each popup takes the activation of
        // input the UI process delivered to the page, as the renderer's popup consumes the activation of its window.
        if (Application::the().blocks_pop_ups() && !consume_transient_user_activation())
            return { {}, {}, {}, {}, {}, {}, Web::HTML::VisibilityState::Hidden, {} };
    }

    auto root_navigable_id = Application::the().allocate_ui_process_cross_process_id();
    auto initial_history_entry = Web::HTML::create_initial_session_history_entry_descriptor(
        Application::the().allocate_ui_process_cross_process_id(), move(opener_base_url), move(target_name));
    auto document_id = Application::the().allocate_ui_process_cross_process_id();
    auto& traversable = CanonicalTraversable::create_a_new_top_level_traversable(root_navigable_id, opener, move(initial_history_entry), document_id);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#the-rules-for-choosing-a-navigable
    // If sandboxingFlagSet's sandbox propagates to auxiliary browsing contexts flag is set, then all the flags that are
    // set in sandboxingFlagSet must be set in chosen's active browsing context's popup sandboxing flag set.
    // NB: sandboxingFlagSet is currentNavigable's active document's active sandboxing flag set, which the UI process
    //     holds for an opener. The requesting process gives it for a popup without one.
    if (opener.has_value()) {
        auto sandboxing_flag_set = opener->active_document().active_sandboxing_flag_set();
        if (has_flag(sandboxing_flag_set, Web::HTML::SandboxingFlagSet::SandboxPropagatesToAuxiliaryBrowsingContexts))
            popup_sandboxing_flag_set |= sandboxing_flag_set;
    }
    traversable.active_browsing_context().set_popup_sandboxing_flag_set(popup_sandboxing_flag_set);

    // 10. Set chosen's is created by web content to true.
    if (!picture_in_picture_navigable.has_value())
        traversable.set_is_created_by_web_content(true);

    auto new_page_id = Application::the().allocate_page_id();
    client().open_page_for_new_top_level_traversable(new_page_id, traversable);

    String window_handle;
    if (picture_in_picture_navigable.has_value()) {
        window_handle = PictureInPictureManager::the().open_window(*this, *picture_in_picture_navigable, traversable, hints.picture_in_picture_window->video_size);
    } else if (view().on_new_web_view) {
        window_handle = view().on_new_web_view(activate_tab, hints, traversable);
    }

    if (!traversable.view().has_value()) {
        client().discard_page_of_undisplayed_top_level_traversable(new_page_id);
        CanonicalTraversable::remove_from_user_agent_top_level_traversable_set(traversable);
        return { {}, {}, {}, {}, {}, {}, Web::HTML::VisibilityState::Hidden, move(window_handle) };
    }
    traversable.represent_group_everywhere();

    auto environment_id = traversable.active_document().relevant_global_object().relevant_settings_object().id();
    Optional<u64> browsing_context_group_id = traversable.active_browsing_context().group()->id();
    return { new_page_id, root_navigable_id, traversable.active_session_history_entry()->descriptor(), move(environment_id), document_id, browsing_context_group_id, traversable.system_visibility_state(), move(window_handle) };
}

void WebContentPage::did_close_browsing_context()
{
    traversable().remove_page(*this);
    // NB: Before unregistering, so an acknowledged embedded discard closes an otherwise-unused server immediately.
    m_detached_close_pending = false;
    close();
    client().unregister_embedded_page(m_id);

    if (displays_tab()) {
        auto& view = this->view();
        view.did_close_browsing_context({});
        if (view.on_close)
            view.on_close();
    }
}

void WebContentPage::did_complete_close_preflight(u64 request_id, bool approved)
{
    if (displays_tab())
        view().did_complete_close_preflight({}, request_id, approved);
}

void WebContentPage::did_request_select_dropdown(Web::HTML::CrossProcessId local_root_id, Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items)
{
    if (auto target = view_position(local_root_id, content_position); target.has_value())
        target->view.did_request_select_dropdown({}, *this, target->position, minimum_width, move(items));
}

void WebContentPage::did_request_primary_paste()
{
    if (!has_transient_user_activation())
        return;
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
    // NB: As for a key event for testing, the event is dispatched as the user's own.
    if (!WebContentClient::renderers_may_synthesize_input()) {
        client().did_misbehave("did_request_webdriver_mouse_event"sv, "not driven by WebDriver or a test"sv);
        return;
    }
    auto on_handled = [page = NonnullRefPtr<WebContentPage>(*this), request_id]() {
        page->async_did_handle_webdriver_mouse_event(request_id);
    };

    // The event is relative to the viewport of the local root its page dispatches input to.
    if (auto root = view().traversable().find(local_root_id); root.has_value()) {
        auto offset = view().traversable().local_root_offset(*root);
        event.position.translate_by(offset);
        event.screen_position.translate_by(offset);
    }
    // Only the compositor sets these, on its way to WebContent.
    event.async_scroll_performed_default_action = false;
    event.scrollbar_dragged_by_compositor = {};
    view().enqueue_webdriver_mouse_event({}, move(event), move(on_handled));
}

void WebContentPage::request_unload_check(Web::HTML::CrossProcessId navigable_id, Web::HTML::CrossProcessId check_id)
{
    auto navigable = hosted_navigable(navigable_id);
    if (!navigable.has_value()) {
        async_unload_check_result(check_id, Web::HTML::CheckIfUnloadingIsCanceledResult::Continue);
        return;
    }
    Vector<Web::HTML::CrossProcessId> inclusive_descendants;
    navigable->for_each_in_inclusive_subtree([&](CanonicalNavigable const& descendant) {
        inclusive_descendants.append(descendant.id());
        return IterationDecision::Continue;
    });
    (void)navigable->top_level_traversable().check_if_unloading_is_canceled(move(inclusive_descendants), {}, {}, {}, Web::HTML::UnloadPromptShown::No,
        [page = NonnullRefPtr<WebContentPage>(*this), check_id](Web::HTML::CheckIfUnloadingIsCanceledResult result, Web::HTML::UnloadPromptShown) {
            page->async_unload_check_result(check_id, result);
        });
}

Messages::WebContentClient::StartWorkerAgentResponse WebContentPage::start_worker_agent(Web::HTML::WorkerAgentStartRequest request)
{
    // A page hosting an isolated iframe's document belongs to its tab's view as much as the view's own page does.
    auto outside_settings = hosted_environment(request.outside_settings.id);
    auto agent_id = WorkerProcessManager::the().start_worker_agent(client(), m_id, outside_settings, move(request));
    return { agent_id };
}

Messages::WebContentClient::DidRequestStorageUsageResponse WebContentPage::did_request_storage_usage(Web::HTML::EnvironmentId environment_id)
{
    auto canonical_key = canonical_storage_key(environment_id);
    if (!canonical_key.has_value())
        return 0;
    return client().session().storage_jar->usage(*canonical_key);
}

void WebContentPage::did_post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage message)
{
    if (auto settings = hosted_environment(message.environment_id); settings.has_value())
        WorkerProcessManager::the().post_broadcast_channel_message(move(message), *settings, client().pid(), client().is_private());
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
    return { dump_process_tree() };
}

void WebContentPage::did_request_crash_of_remote_frame_processes_for_testing()
{
    traversable().for_each_in_subtree([](CanonicalNavigable& child_frame) {
        if (child_frame.has_remote_host())
            child_frame.remote_host().async_debug_request("crash-current-page"sv, ""sv);
        return IterationDecision::Continue;
    });
}

// The tab's Stop and Reload buttons.
void WebContentPage::did_request_stop_loading_for_testing()
{
    if (displays_tab())
        view().stop_loading();
}

void WebContentPage::did_request_reload_for_testing(bool bypass_cache)
{
    if (displays_tab())
        view().reload(bypass_cache ? BypassCache::Yes : BypassCache::No);
}

// The tab's Back and Forward buttons, which traverse the history with no source document.
void WebContentPage::did_request_traverse_history_by_delta_for_testing(i32 delta)
{
    if (displays_tab())
        view().traverse_the_history_by_delta(delta);
}

void WebContentPage::reset_session_history_for_testing()
{
    auto* client = routed_connection();
    if (!client || !client->test_connection())
        return;
    client->transport().flush();
    client->test_connection()->async_reset_session_history_for_testing(m_id);
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

// A document populated for a navigable is one the browser process created for it, and has not made active yet.
Messages::WebContentTestClient::DidRequestHasPopulatedDocumentForTestingResponse WebContentPage::did_request_has_populated_document_for_testing(Web::HTML::CrossProcessId navigable_id) const
{
    auto navigable = traversable().top_level_traversable().find(navigable_id);
    if (!navigable.has_value())
        return false;
    bool has_populated_document = false;
    navigable->for_each_populated_document_state([&](auto const&) {
        has_populated_document = true;
    });
    return has_populated_document;
}

Messages::WebContentTestClient::DidRequestSessionStoreTabStateForTestingResponse WebContentPage::did_request_session_store_tab_state_for_testing()
{
    if (displays_tab())
        return { view().session_store_tab_state_for_testing({}) };
    return { "{}"_string };
}

// https://w3c.github.io/FileAPI/#add-an-entry
Messages::WebContentClient::DidAddBlobUrlEntryResponse WebContentPage::did_add_blob_url_entry(Web::HTML::EnvironmentId environment_id, Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry)
{
    // 3. Let entry be a new blob URL entry consisting of object and the current settings object.
    auto environment = hosted_environment(environment_id);
    if (!environment.has_value())
        return URL::BlobURLEntry::Token { 0 };
    entry.origin = environment->origin();

    // 4. Set store[url] to entry.
    // NB: A document created from the URL in another process reads local files as local file content does, so the
    //     store remembers whether local file content added the entry.
    auto added_by_local_file_content = client().may_read_local_files() ? AddedByLocalFileContent::Yes : AddedByLocalFileContent::No;
    return client().session().blob_url_store->add_entry(move(url), move(entry), WeakPtr<WebContentClient> { client() }, added_by_local_file_content);
}

// https://w3c.github.io/FileAPI/#dfn-revokeObjectURL
void WebContentPage::did_remove_blob_url_entries(Web::HTML::EnvironmentId environment_id, Vector<Utf16String> urls)
{
    auto environment = hosted_environment(environment_id);
    if (!environment.has_value())
        return;
    client().session().blob_url_store->remove_entries(urls, environment->origin(), WeakPtr<WebContentClient> { client() });
}

Optional<bool> WebContentPage::hosted_environment_has_cross_site_ancestor(Web::HTML::EnvironmentId const& environment_id) const
{
    auto is_of_environment = [&](CanonicalDocument const& document) {
        return document.relevant_global_object().relevant_settings_object().id() == environment_id;
    };

    Optional<bool> result;
    traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        if (traversable().hosts(navigable, *this) && is_of_environment(navigable.active_document())) {
            result = navigable.active_document_has_cross_site_ancestor();
            return IterationDecision::Break;
        }

        // NB: A document being populated has a cross-site ancestor if its navigable's parent's active document has one,
        //     or is of another site.
        navigable.for_each_populated_document_state([&](CanonicalDocumentState const& document_state) {
            auto const& document = *document_state.populated_document->document;
            if (result.has_value() || document.host() != this || !is_of_environment(document))
                return;
            auto const* parent = navigable.parent();
            result = parent && (parent->active_document_has_cross_site_ancestor() || !parent->active_document().origin().is_same_site(document.origin()));
        });
        return result.has_value() ? IterationDecision::Break : IterationDecision::Continue;
    });
    return result;
}

// Script reaches cookies through an environment the page hosts, and only those of that environment's origin. This is
// the environment's cookie partition context, or nothing if the page hosts no such environment, or if the environment's
// top-level document has an opaque origin and so is a third-party context of no site, which has no cookies.
static Optional<HTTP::Cookie::PartitionContext> cookie_partition_context_for(WebContentPage const& page, Optional<Web::HTML::EnvironmentId> const& environment_id, URL::URL const& url)
{
    if (!environment_id.has_value())
        return {};

    auto environment = page.hosted_environment(*environment_id);
    if (!environment.has_value() || !environment->may_use_cookies_of(url))
        return {};

    auto top_level_site = network_isolation_top_level_site(*environment);
    if (!top_level_site.has_value())
        return {};

    return HTTP::Cookie::PartitionContext {
        .top_level_site = top_level_site.release_value(),
        .has_cross_site_ancestor = page.hosted_environment_has_cross_site_ancestor(*environment_id).value_or(true),
    };
}

void WebContentPage::did_set_cookie(Optional<Web::HTML::EnvironmentId> environment_id, URL::URL url, HTTP::Cookie::ParsedCookie cookie, HTTP::Cookie::Source source)
{
    // NB: Only WebDriver sets cookies like HTTP does, for the top-level documents it drives.
    if (source == HTTP::Cookie::Source::Http) {
        if (!WebContentClient::renderers_may_access_cookies_like_http()) {
            client().did_misbehave("did_set_cookie"sv, "HTTP cookie source"sv);
            return;
        }
        client().session().cookie_jar->set_cookie(url, cookie, source, {});
        return;
    }

    auto partition_context = cookie_partition_context_for(*this, environment_id, url);
    if (!partition_context.has_value())
        return;

    client().session().cookie_jar->set_cookie(url, cookie, source, partition_context);
}

Messages::WebContentClient::DidRequestAllCookiesCookiestoreResponse WebContentPage::did_request_all_cookies_cookiestore(Web::HTML::EnvironmentId const& environment_id, URL::URL url)
{
    auto partition_context = cookie_partition_context_for(*this, environment_id, url);
    if (!partition_context.has_value())
        return Vector<HTTP::Cookie::Cookie> {};

    return client().session().cookie_jar->get_all_cookies_cookiestore(url, partition_context);
}

Messages::WebContentClient::DidRequestCookieResponse WebContentPage::did_request_cookie(Optional<Web::HTML::EnvironmentId> const& environment_id, URL::URL url, HTTP::Cookie::Source source)
{
    // NB: Only WebDriver reads cookies like HTTP does, for the top-level documents it drives.
    Optional<HTTP::Cookie::PartitionContext> partition_context;
    if (source == HTTP::Cookie::Source::NonHttp) {
        partition_context = cookie_partition_context_for(*this, environment_id, url);
        if (!partition_context.has_value())
            return HTTP::Cookie::VersionedCookie {};
    }

    HTTP::Cookie::VersionedCookie cookie;
    cookie.cookie = client().session().cookie_jar->get_cookie(url, source, partition_context);
    if (source == HTTP::Cookie::Source::NonHttp)
        cookie.cookie_version = view().document_cookie_version(url);
    return cookie;
}

}
