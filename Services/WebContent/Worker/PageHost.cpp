/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGC/Heap.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>
#include <WebContent/Worker/ConnectionFromClient.h>
#include <WebContent/Worker/DedicatedWorkerThreadLink.h>
#include <WebContent/Worker/PageHost.h>
#include <WebContent/Worker/WebWorkerCompositorHost.h>
#include <WebContent/Worker/WorkerThread.h>

namespace WebWorker {

GC_DEFINE_ALLOCATOR(PageHost);

GC::Ref<PageHost> PageHost::create(ConnectionFromClient& client)
{
    return GC::Heap::the().allocate<PageHost>(client);
}

PageHost::~PageHost() = default;

Web::Page& PageHost::page()
{
    return *m_page;
}

Web::Page const& PageHost::page() const
{
    return *m_page;
}

Gfx::Palette PageHost::palette() const
{
    return Gfx::Palette(*m_palette_impl);
}

void PageHost::setup_palette()
{
    // FIXME: We don't actually need a palette :thonk:
    auto buffer_or_error = Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme));
    VERIFY(!buffer_or_error.is_error());
    auto buffer = buffer_or_error.release_value();
    auto* theme = buffer.data<Gfx::SystemTheme>();
    theme->color[to_underlying(Gfx::ColorRole::Window)] = Color(Color::Magenta).value();
    theme->color[to_underlying(Gfx::ColorRole::WindowText)] = Color(Color::Cyan).value();
    m_palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(buffer);
}

bool PageHost::is_connection_open() const
{
    return m_client.is_open();
}

Web::DevicePixelRect PageHost::screen_rect() const
{
    return {};
}

double PageHost::zoom_level() const
{
    return 1.0;
}

double PageHost::device_pixel_ratio() const
{
    return 1.0;
}

double PageHost::device_pixels_per_css_pixel() const
{
    return 1.0;
}

Web::CSS::PreferredColorScheme PageHost::preferred_color_scheme() const
{
    return Web::CSS::PreferredColorScheme::Auto;
}

Web::CSS::PreferredContrast PageHost::preferred_contrast() const
{
    return Web::CSS::PreferredContrast::Auto;
}

Web::CSS::PreferredMotion PageHost::preferred_motion() const
{
    return Web::CSS::PreferredMotion::Auto;
}

// NB: A worker's connection stands for one environment, which the UI process knows to be the requesting one.
Optional<Web::Permissions::PermissionSnapshot> PageHost::page_did_request_permission_snapshot(URL::Origin const& top_level_origin)
{
    auto response = m_client.send_sync_but_allow_failure<Messages::WebWorkerClient::DidRequestPermissionSnapshot>(top_level_origin);
    if (!response)
        return {};
    return response->take_snapshot();
}

// NB: The sync requests here fail once the worker is terminated or the UI process is gone. The script that made one no
//     longer runs by then, so what it gets back here does not matter, but it has to get something.
HTTP::Cookie::VersionedCookie PageHost::page_did_request_cookie(Optional<Web::HTML::EnvironmentId> const&, URL::URL const& url, HTTP::Cookie::Source source)
{
    auto response = m_client.send_sync_but_allow_failure<Messages::WebWorkerClient::DidRequestCookie>(url, source);
    if (!response)
        return {};
    return response->take_cookie();
}

bool PageHost::page_did_is_known_hsts_host(String const& domain)
{
    auto response = m_client.send_sync_but_allow_failure<Messages::WebWorkerClient::DidIsKnownHstsHost>(domain);
    return response && response->result();
}

void PageHost::page_did_report_worker_exception(Utf16String const& message, Utf16String const& filename, u32 lineno, u32 colno)
{
    if (auto* link = m_client.dedicated_worker_thread_link())
        link->report_exception_to_owner(message, filename, lineno, colno);
}

// NB: The worker's only client another process hosts is outside settings, whose violations its owner reports.
void PageHost::page_did_report_violation_of_remote_client(u64 policy_index, String effective_directive, URL::URL resource)
{
    if (auto* link = m_client.dedicated_worker_thread_link()) {
        link->report_content_security_policy_violation_to_owner(policy_index, move(effective_directive), resource);
        return;
    }
    m_client.async_did_report_outside_settings_content_security_policy_violation(policy_index, effective_directive, resource);
}

void PageHost::page_did_post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage const& message)
{
    m_client.async_did_post_broadcast_channel_message(message);
}

void PageHost::page_did_close_shared_worker()
{
    (void)m_client.send_sync_but_allow_failure<Messages::WebWorkerClient::DidCloseSharedWorker>();
}

void PageHost::request_file(Web::FileRequest request)
{
    m_client.request_file(move(request));
}

URL::BlobURLEntry::Token PageHost::page_did_add_blob_url_entry(Web::HTML::EnvironmentSettingsObject const& environment, Utf16String const& url, Web::FileAPI::SerializedBlobURLEntry const& entry)
{
    auto response = m_client.send_sync_but_allow_failure<Messages::WebWorkerClient::DidAddBlobUrlEntry>(environment.id, url, entry);
    if (!response)
        return 0;
    return response->token();
}

void PageHost::page_did_remove_blob_url_entries(Web::HTML::EnvironmentSettingsObject const& environment, Vector<Utf16String> const& urls)
{
    (void)m_client.send_sync_but_allow_failure<Messages::WebWorkerClient::DidRemoveBlobUrlEntries>(environment.id, urls);
}

Optional<Web::FileAPI::SerializedBlobURLEntry> PageHost::page_did_request_blob_url_entry(Utf16String const& url, Optional<URL::BlobURLEntry::Token> token)
{
    auto response = m_client.send_sync_but_allow_failure<Messages::WebWorkerClient::DidRequestBlobUrlEntry>(url, token);
    if (!response)
        return {};
    return response->take_entry();
}

RefPtr<Web::HTML::DedicatedWorkerThreadLink> PageHost::start_dedicated_worker(Web::HTML::WorkerAgentStartRequest&& request)
{
    auto started_thread = start_dedicated_worker_thread(request.owner_token);
    if (started_thread.is_error()) {
        dbgln("Unable to start a dedicated worker: {}", started_thread.error());
        return {};
    }
    m_client.async_register_dedicated_worker(move(request), move(started_thread.value().browser_end_of_worker_connection));
    return move(started_thread.value().link);
}

void PageHost::ensure_compositor_host()
{
    if (m_compositor_host)
        return;
    m_compositor_host = create_web_worker_compositor_host(m_client);
}

void PageHost::compositor_process_lost()
{
    page().notify_all_webgl_contexts_lost();
}

void PageHost::did_finish_loading_worker_script(bool worker_is_secure_context)
{
    m_client.async_did_finish_loading_worker_script(worker_is_secure_context);
}

void PageHost::did_fail_loading_worker_script()
{
    if (auto* link = m_client.dedicated_worker_thread_link()) {
        link->report_script_load_failure_to_owner();
        Core::deferred_invoke([&client = m_client] {
            client.close_worker();
        });
        return;
    }
    m_client.async_did_fail_loading_worker_script();
}

PageHost::PageHost(ConnectionFromClient& client)
    : m_client(client)
    , m_page(Web::Page::create(*this))
{
    setup_palette();
}

void PageHost::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

}
