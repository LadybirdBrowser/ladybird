/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Page/Page.h>

namespace Web::WebAudio::Render {

class AudioWorkletPageClient final : public Web::PageClient {
    GC_CELL(AudioWorkletPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(AudioWorkletPageClient);

public:
    static GC::Ref<AudioWorkletPageClient> create(JS::VM& vm);

    virtual ~AudioWorkletPageClient() override = default;

    GC::Ref<Web::Page> page_ref() const
    {
        VERIFY(m_page);
        return *m_page;
    }

    virtual u64 id() const override { return 0; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { return Gfx::Palette(*m_palette_impl); }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1.0; }
    virtual double device_pixel_ratio() const override { return 1.0; }
    virtual double device_pixels_per_css_pixel() const override { return 1.0; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::Auto; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { return m_input_event_queue; }
    virtual void report_finished_handling_input_event([[maybe_unused]] u64 page_id, [[maybe_unused]] Web::EventResult event_was_handled) override { }
    virtual void request_file([[maybe_unused]] Web::FileRequest request) override { }
    virtual Web::DisplayListPlayerType display_list_player_type() const override { return Web::DisplayListPlayerType::SkiaCPU; }
    virtual bool is_headless() const override { return true; }

private:
    AudioWorkletPageClient() = default;

    virtual void visit_edges(JS::Cell::Visitor& visitor) override;
    void setup_palette();

    GC::Ptr<Web::Page> m_page;
    RefPtr<Gfx::PaletteImpl> m_palette_impl;
    Queue<Web::QueuedInputEvent> m_input_event_queue;
};

}
