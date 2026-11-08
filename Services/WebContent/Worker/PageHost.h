/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Rect.h>
#include <LibHTTP/Forward.h>
#include <LibWeb/Page/Page.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/Page/QueuedInputEvent.h>
#include <LibWebCommon/PixelUnits.h>
#include <WebContent/Worker/Forward.h>

namespace WebWorker {

class PageHost final : public Web::PageClient {
    GC_CELL(PageHost, Web::PageClient);
    GC_DECLARE_ALLOCATOR(PageHost);

public:
    static GC::Ref<PageHost> create(ConnectionFromClient& client);

    virtual ~PageHost();

    // NB: A worker has no page in the UI process, so nothing there knows this ID.
    virtual Web::PageId id() const override { return 0; }
    virtual Web::Page& page() override;
    virtual Web::Page const& page() const override;
    virtual bool is_connection_open() const override;
    virtual Gfx::Palette palette() const override;
    virtual Web::DevicePixelRect screen_rect() const override;
    virtual double zoom_level() const override;
    virtual double device_pixel_ratio() const override;
    virtual double device_pixels_per_css_pixel() const override;
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override;
    virtual Web::CSS::PreferredContrast preferred_contrast() const override;
    virtual Web::CSS::PreferredMotion preferred_motion() const override;
    virtual size_t screen_count() const override { return 1; }
    virtual HTTP::Cookie::VersionedCookie page_did_request_cookie(Optional<Web::HTML::EnvironmentId> const&, URL::URL const&, HTTP::Cookie::Source) override;
    virtual Optional<Web::Permissions::PermissionSnapshot> page_did_request_permission_snapshot(URL::Origin const& top_level_origin) override;
    virtual bool page_did_is_known_hsts_host(String const&) override;
    virtual void page_did_report_worker_exception(Utf16String const& message, Utf16String const& filename, u32 lineno, u32 colno) override;
    virtual void page_did_report_violation_of_remote_client(u64 policy_index, String effective_directive, URL::URL resource) override;
    virtual void page_did_post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage const& message) override;
    virtual void page_did_close_shared_worker() override;
    virtual void request_file(Web::FileRequest) override;
    virtual URL::BlobURLEntry::Token page_did_add_blob_url_entry(Web::HTML::EnvironmentSettingsObject const&, Utf16String const& url, Web::FileAPI::SerializedBlobURLEntry const&) override;
    virtual void page_did_remove_blob_url_entries(Web::HTML::EnvironmentSettingsObject const&, Vector<Utf16String> const& urls) override;
    virtual Optional<Web::FileAPI::SerializedBlobURLEntry> page_did_request_blob_url_entry(Utf16String const& url, Optional<URL::BlobURLEntry::Token> token) override;
    virtual RefPtr<Web::HTML::DedicatedWorkerThreadLink> start_dedicated_worker(Web::HTML::WorkerAgentStartRequest&&) override;
    virtual bool supports_compositor() const override { return true; }
    virtual void ensure_compositor_host() override;
    virtual Web::Compositor::CompositorHost* compositor_host() override { return m_compositor_host.ptr(); }
    virtual Web::Compositor::CompositorHost const* compositor_host() const override { return m_compositor_host.ptr(); }
    void compositor_process_lost();
    virtual bool is_headless() const override { return false; }
    // NB: No input reaches a worker, so its queue stays empty.
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { return m_input_event_queue; }
    virtual void report_finished_handling_input_event([[maybe_unused]] Web::PageId page_id, [[maybe_unused]] u64 event_id, [[maybe_unused]] Web::EventResult event_was_handled) override { }
    // NB: A worker's event loop schedules its own rendering, and nothing else here asks for a frame.
    virtual void request_frame() override { }
    virtual double maximum_frames_per_second() const override { return m_maximum_frames_per_second; }
    void set_maximum_frames_per_second(double maximum_frames_per_second) { m_maximum_frames_per_second = maximum_frames_per_second; }
    void did_finish_loading_worker_script(bool worker_is_secure_context);
    void did_fail_loading_worker_script();

private:
    explicit PageHost(ConnectionFromClient&);
    virtual void visit_edges(JS::Cell::Visitor&) override;

    void setup_palette();

    ConnectionFromClient& m_client;
    OwnPtr<Web::Compositor::CompositorHost> m_compositor_host;
    GC::Ref<Web::Page> m_page;
    RefPtr<Gfx::PaletteImpl> m_palette_impl;
    Queue<Web::QueuedInputEvent> m_input_event_queue;
    double m_maximum_frames_per_second { 60.0 };
};

}
