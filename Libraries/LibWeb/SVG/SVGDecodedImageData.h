/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Page/Page.h>

namespace Web::SVG {

class SVGDecodedImageData final : public HTML::DecodedImageData {
    GC_CELL(SVGDecodedImageData, HTML::DecodedImageData);
    GC_DECLARE_ALLOCATOR(SVGDecodedImageData);

public:
    class SVGPageClient;
    static ErrorOr<GC::Ref<SVGDecodedImageData>> create(JS::Realm&, GC::Ref<Page>, URL::URL const&, ByteBuffer encoded_svg);
    virtual ~SVGDecodedImageData() override;

    virtual RefPtr<Gfx::ImmutableBitmap> bitmap(size_t frame_index, Gfx::IntSize) const override;

    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;

    // FIXME: Support SVG animations. :^)
    virtual int frame_duration(size_t) const override { return 0; }
    virtual size_t frame_count() const override { return 1; }
    virtual size_t loop_count() const override { return 0; }
    virtual bool is_animated() const override { return false; }

    DOM::Document const& svg_document() const { return *m_document; }

    virtual void visit_edges(Cell::Visitor& visitor) override;

private:
    SVGDecodedImageData(GC::Ref<Page>, GC::Ref<SVGPageClient>, GC::Ref<DOM::Document>, GC::Ref<SVG::SVGSVGElement>);

    RefPtr<Gfx::Bitmap> render(Gfx::IntSize) const;

    mutable HashMap<Gfx::IntSize, NonnullRefPtr<Gfx::ImmutableBitmap>> m_cached_rendered_bitmaps;

    GC::Ref<Page> m_page;
    GC::Ref<SVGPageClient> m_page_client;

    GC::Ref<DOM::Document> m_document;
    GC::Ref<SVG::SVGSVGElement> m_root_element;
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

    virtual Page& page() override { return *m_svg_page; }
    virtual Page const& page() const override { return *m_svg_page; }
    virtual bool is_connection_open() const override { return false; }
    virtual Gfx::Palette palette() const override { return m_host_page->client().palette(); }
    virtual DevicePixelRect screen_rect() const override { return {}; }
    virtual double device_pixels_per_css_pixel() const override { return 1.0; }
    virtual CSS::PreferredColorScheme preferred_color_scheme() const override { return m_host_page->client().preferred_color_scheme(); }
    virtual CSS::PreferredContrast preferred_contrast() const override { return m_host_page->client().preferred_contrast(); }
    virtual CSS::PreferredMotion preferred_motion() const override { return m_host_page->client().preferred_motion(); }
    virtual void request_file(FileRequest) override { }
    virtual void paint_next_frame() override { }
    virtual void process_screenshot_requests() override { }
    virtual void paint(DevicePixelRect const&, Painting::BackingStore&, Web::PaintOptions = {}) override { }
    virtual bool is_ready_to_paint() const override { return true; }
    virtual Queue<QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event([[maybe_unused]] u64 page_id, [[maybe_unused]] EventResult event_was_handled) override { }

    virtual DisplayListPlayerType display_list_player_type() const override { return m_host_page->client().display_list_player_type(); }
    virtual bool is_headless() const override { return m_host_page->client().is_headless(); }

private:
    explicit SVGPageClient(Page& host_page)
        : m_host_page(host_page)
    {
    }

    virtual void visit_edges(Visitor&) override;
};

}
