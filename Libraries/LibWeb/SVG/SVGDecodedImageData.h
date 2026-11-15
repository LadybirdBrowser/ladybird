/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibCore/Promise.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakHashSet.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/ImagePaint.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/Page/QueuedInputEvent.h>

namespace Web::SVG {

class SVGDecodedImageData final : public HTML::DecodedImageData {
    GC_CELL(SVGDecodedImageData, HTML::DecodedImageData);
    GC_DECLARE_ALLOCATOR(SVGDecodedImageData);

public:
    class SVGPageClient;
    using DecodePromise = Core::Promise<GC::Root<SVGDecodedImageData>>;
    static NonnullRefPtr<DecodePromise> decode(GC::Ref<Page>, URL::URL const&, ReadonlyBytes encoded_svg);
    virtual ~SVGDecodedImageData() override;

    virtual Optional<Gfx::DecodedImageFrame> default_frame(Gfx::IntSize = {}) const override;
    virtual Optional<Gfx::DecodedImageFrame> current_frame(Gfx::IntSize = {}) const override;

    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;
    u64 vector_content_identity() const { return m_vector_content_identity; }

    // Has the image's document keep a rendering of the image for `key`, laid out at the key's CSS size and recorded at
    // css × raster_scale resolution, and answers the document's host, which keeps it, or none where the image has
    // nothing to show.
    Layout::RustFFI::DocumentHost const* render_at_scale(Painting::VectorImageKey const&) const;

    // FIXME: Support SVG animations. :^)
    DOM::Document const& svg_document() const { return *m_document; }

    virtual void visit_edges(Cell::Visitor& visitor) override;
    virtual void finalize() override;
    virtual size_t external_memory_size() const override;

    virtual Optional<Painting::ImagePaint> image_paint(Painting::ImagePaintRequest const&) const override;
    bool has_active_view_box() const;

    // The scheme the SVG document currently answers `prefers-color-scheme` with. Held on the image
    // rather than on the page, because the page is shared with every other image.
    CSS::PreferredColorScheme color_scheme() const { return m_color_scheme; }

private:
    static GC::Ref<SVGDecodedImageData> create(GC::Ref<Page>, GC::Ref<SVGPageClient>, GC::Ref<DOM::Document>, GC::Ref<SVG::SVGSVGElement>);
    SVGDecodedImageData(GC::Ref<Page>, GC::Ref<SVGPageClient>, GC::Ref<DOM::Document>, GC::Ref<SVG::SVGSVGElement>);

    CSS::SizeWithAspectRatio const& natural_size() const;
    Compositor::CompositorHost* host_compositor() const;
    RefPtr<Gfx::Bitmap> render_frame(Gfx::IntSize) const;
    void did_request_frame();
    void invalidate_cached_rendering();
    static u64 next_vector_content_identity();

    mutable HashMap<Gfx::IntSize, Gfx::DecodedImageFrame> m_cached_rendered_frames;

    mutable CSS::PreferredColorScheme m_color_scheme { CSS::PreferredColorScheme::Auto };

    mutable Optional<CSS::SizeWithAspectRatio> m_natural_size;

    GC::Ref<Page> m_page;
    GC::Ref<SVGPageClient> m_page_client;

    GC::Ref<DOM::Document> m_document;
    GC::Ref<SVG::SVGSVGElement> m_root_element;
    u64 m_vector_content_identity { 0 };

    mutable bool m_is_recording_display_list { false };
    bool m_has_pending_client_notification { false };
};

class SVGDecodedImageData::SVGPageClient final : public PageClient {
    GC_CELL(SVGDecodedImageData::SVGPageClient, PageClient);
    GC_DECLARE_ALLOCATOR(SVGDecodedImageData::SVGPageClient);

    friend class ScopedSVGImageDocument;

public:
    static GC::Ref<SVGPageClient> create(Page& page)
    {
        return GC::Heap::the().allocate<SVGPageClient>(page);
    }

    virtual ~SVGPageClient() override = default;

    GC::Ref<Page> m_host_page;
    GC::Ptr<Page> m_svg_page;

    virtual Web::PageId id() const override { return m_host_page->client().id(); }
    virtual HTML::CrossProcessId allocate_cross_process_id() override { return m_host_page->client().allocate_cross_process_id(); }
    virtual HTML::CrossProcessId allocate_navigable_id() override { return m_host_page->client().allocate_navigable_id(); }
    virtual Page& page() override { return *m_svg_page; }
    virtual Page const& page() const override { return *m_svg_page; }
    virtual bool is_connection_open() const override { return false; }
    virtual Gfx::Palette palette() const override { return m_host_page->client().palette(); }
    virtual DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1.0; }
    virtual double device_pixel_ratio() const override { return 1.0; }
    virtual double device_pixels_per_css_pixel() const override { return m_device_pixels_per_css_pixel; }
    void set_device_pixels_per_css_pixel(double value) { m_device_pixels_per_css_pixel = value; }
    virtual CSS::PreferredColorScheme preferred_color_scheme() const override { return m_host_page->client().preferred_color_scheme(); }
    virtual CSS::PreferredContrast preferred_contrast() const override { return m_host_page->client().preferred_contrast(); }
    virtual CSS::PreferredMotion preferred_motion() const override { return m_host_page->client().preferred_motion(); }
    virtual size_t screen_count() const override { return 1; }
    virtual void request_file(FileRequest) override { }
    virtual Queue<QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event([[maybe_unused]] Web::PageId page_id, [[maybe_unused]] u64 event_id, [[maybe_unused]] EventResult event_was_handled) override { }
    virtual void request_frame() override;
    virtual void request_frame_for(DOM::Document const&) override;

    virtual bool is_headless() const override { return m_host_page->client().is_headless(); }
    virtual NonnullRefPtr<Media::AudioOutputFactory> create_audio_output_factory() override;
    virtual NonnullOwnPtr<MediaClient::RemotePlaybackManager> create_media_playback_manager() override;
    virtual void rebind_media_playback_manager(MediaClient::RemotePlaybackManager&) override;
    virtual NonnullRefPtr<Core::Promise<NonnullRefPtr<Audio::RecordStream>>> create_audio_record_stream(StringView device_id, Function<void(ReadonlyBytes, Audio::SampleSpecification const&)>) override;

    void register_svg_image_data(SVGDecodedImageData&);
    HTML::Window& window() const;

    GC::Ptr<SVGDecodedImageData> current_svg_image_data() const { return m_current_svg_image_data.ptr(); }
    void set_current_svg_image_data(GC::Ptr<SVGDecodedImageData>);
    void suppress_frame_requests() { ++m_frame_request_suppression_count; }
    void unsuppress_frame_requests()
    {
        VERIFY(m_frame_request_suppression_count > 0);
        --m_frame_request_suppression_count;
    }

private:
    explicit SVGPageClient(Page& host_page)
        : m_host_page(host_page)
    {
    }

    virtual bool is_svg_page_client() const override { return true; }

    virtual void visit_edges(Visitor&) override;

    double m_device_pixels_per_css_pixel { 1.0 };
    GC::WeakHashSet<SVGDecodedImageData> m_svg_image_data;
    GC::Weak<SVGDecodedImageData> m_current_svg_image_data;
    size_t m_frame_request_suppression_count { 0 };
};

class WEB_API ScopedSVGImageDocument {
    AK_MAKE_NONCOPYABLE(ScopedSVGImageDocument);

public:
    enum class FrameRequests : u8 {
        Suppress,
        RouteToCurrentImage
    };

    ScopedSVGImageDocument(DOM::Document&, FrameRequests);
    ScopedSVGImageDocument(ScopedSVGImageDocument&&);
    ~ScopedSVGImageDocument();

    [[nodiscard]] static Optional<ScopedSVGImageDocument> create_if_needed(GC::Ptr<DOM::Document const>, FrameRequests);

private:
    GC::Ref<SVGDecodedImageData::SVGPageClient> m_page_client;
    GC::Ref<HTML::LocalNavigable> m_navigable;
    GC::Ref<HTML::Window> m_window;
    GC::Ref<DOM::Document> m_previous_document;
    GC::Weak<SVGDecodedImageData> m_previous_current_image_data;
    bool m_should_unsuppress_frame_requests { false };
    bool m_is_active { true };
};

}

// NB: The promise resolves to a root, and keeps the callbacks of whoever waits for it.
GC_UNTRACED_CAPTURE_WITH_ROOTED_CALLBACKS(Web::SVG::SVGDecodedImageData::DecodePromise);
