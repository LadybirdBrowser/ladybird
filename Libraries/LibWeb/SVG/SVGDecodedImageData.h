/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakHashSet.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

namespace Web::SVG {

class SVGDecodedImageData final : public HTML::DecodedImageData {
    GC_CELL(SVGDecodedImageData, HTML::DecodedImageData);
    GC_DECLARE_ALLOCATOR(SVGDecodedImageData);

public:
    class SVGPageClient;
    static ErrorOr<GC::Ref<SVGDecodedImageData>> create(JS::Realm&, GC::Ref<Page>, URL::URL const&, ReadonlyBytes encoded_svg);
    virtual ~SVGDecodedImageData() override;

    virtual Optional<Gfx::DecodedImageFrame> default_frame(Gfx::IntSize = {}) const override;
    virtual Optional<Gfx::DecodedImageFrame> current_frame(Gfx::IntSize = {}) const override;

    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;

    virtual Optional<Painting::DisplayListResource> record_display_list(Gfx::IntSize, Painting::DisplayListResourceStorage&) const override;

    // FIXME: Support SVG animations. :^)
    DOM::Document const& svg_document() const { return *m_document; }

    virtual void visit_edges(Cell::Visitor& visitor) override;
    virtual size_t external_memory_size() const override;

    virtual void paint(DisplayListRecordingContext&, Gfx::IntRect dst_rect, CSS::ImageRendering) const override;

private:
    SVGDecodedImageData(GC::Ref<Page>, GC::Ref<SVGPageClient>, GC::Ref<DOM::Document>, GC::Ref<SVG::SVGSVGElement>);

    RefPtr<Gfx::PaintingSurface> render_to_surface(Gfx::IntSize) const;
    void prune_cached_display_list_resources() const;
    void append_cached_display_list_resources(Painting::DisplayListResourceSet&) const;
    void did_request_frame();
    void invalidate_cached_rendering();

    // FIXME: Remove this once everything is using surfaces instead.
    mutable HashMap<Gfx::IntSize, Gfx::DecodedImageFrame> m_cached_rendered_frames;

    mutable HashMap<Gfx::IntSize, NonnullRefPtr<Gfx::PaintingSurface>> m_cached_rendered_surfaces;

    struct CachedDisplayList {
        NonnullRefPtr<Painting::DisplayList> display_list;
        Painting::AccumulatedVisualContextTree visual_context_tree;
        // Precomputed by collect_referenced_resources(); the display list is immutable, so this never changes.
        Painting::DisplayListResourceSet referenced_resources;
    };
    mutable HashMap<Gfx::IntSize, CachedDisplayList> m_cached_display_lists;

    GC::Ref<Page> m_page;
    GC::Ref<SVGPageClient> m_page_client;

    GC::Ref<DOM::Document> m_document;
    GC::Ref<SVG::SVGSVGElement> m_root_element;

    mutable bool m_is_recording_display_list { false };
};

class SVGDecodedImageData::SVGPageClient final : public PageClient {
    GC_CELL(SVGDecodedImageData::SVGPageClient, PageClient);
    GC_DECLARE_ALLOCATOR(SVGDecodedImageData::SVGPageClient);

public:
    static GC::Ref<SVGPageClient> create(JS::VM& vm, Page& page)
    {
        return vm.heap().allocate<SVGPageClient>(page);
    }

    virtual ~SVGPageClient() override = default;

    GC::Ref<Page> m_host_page;
    GC::Ptr<Page> m_svg_page;

    virtual u64 id() const override { VERIFY_NOT_REACHED(); }
    virtual HTML::CrossProcessId allocate_navigable_id() override { return m_host_page->client().allocate_navigable_id(); }
    virtual Page& page() override { return *m_svg_page; }
    virtual Page const& page() const override { return *m_svg_page; }
    virtual bool is_connection_open() const override { return false; }
    virtual Gfx::Palette palette() const override { return m_host_page->client().palette(); }
    virtual DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1.0; }
    virtual double device_pixel_ratio() const override { return 1.0; }
    virtual double device_pixels_per_css_pixel() const override { return 1.0; }
    virtual CSS::PreferredColorScheme preferred_color_scheme() const override { return m_host_page->client().preferred_color_scheme(); }
    virtual CSS::PreferredContrast preferred_contrast() const override { return m_host_page->client().preferred_contrast(); }
    virtual CSS::PreferredMotion preferred_motion() const override { return m_host_page->client().preferred_motion(); }
    virtual size_t screen_count() const override { return 1; }
    virtual void request_file(FileRequest) override { }
    virtual Queue<QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event([[maybe_unused]] u64 page_id, [[maybe_unused]] EventResult event_was_handled) override { }
    virtual void request_frame() override;

    virtual bool is_headless() const override { return m_host_page->client().is_headless(); }

    void register_svg_image_data(SVGDecodedImageData&);
    void prune_cached_display_list_resources() const;
    HTML::Window& window() const;
    void begin_recording_display_list() { ++m_display_list_recording_count; }
    void end_recording_display_list();

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
    void prune_cached_display_list_resources_now() const;

    GC::WeakHashSet<SVGDecodedImageData> m_svg_image_data;
    GC::Weak<SVGDecodedImageData> m_current_svg_image_data;
    size_t m_frame_request_suppression_count { 0 };
    size_t m_display_list_recording_count { 0 };
    mutable bool m_has_pending_display_list_resource_prune { false };
};

}
