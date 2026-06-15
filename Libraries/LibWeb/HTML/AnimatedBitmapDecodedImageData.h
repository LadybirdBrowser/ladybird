/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <LibGC/Ptr.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibWeb/HTML/AnimatedDecodedImageData.h>
#include <LibWeb/HTML/DecodedImageData.h>

namespace Web::HTML {

class AnimatedBitmapDecodedImageData final : public AnimatedDecodedImageData {
    GC_CELL(AnimatedBitmapDecodedImageData, AnimatedDecodedImageData);
    GC_DECLARE_ALLOCATOR(AnimatedBitmapDecodedImageData);
    friend class Web::Internals::Internals;

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    static GC::Ref<AnimatedBitmapDecodedImageData> create(
        JS::Realm&,
        DOM::Document&,
        i64 session_id,
        u32 frame_count,
        u32 loop_count,
        Gfx::IntSize,
        Gfx::ColorSpace,
        Vector<u32> durations,
        Vector<NonnullRefPtr<Gfx::Bitmap>> initial_bitmaps);

    virtual ~AnimatedBitmapDecodedImageData() override;
    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual Optional<Gfx::DecodedImageFrame> default_frame(Gfx::IntSize = {}) const override;
    virtual Optional<Gfx::DecodedImageFrame> current_frame(Gfx::IntSize = {}) const override;

    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;

    virtual void paint(DisplayListRecordingContext&, Gfx::IntRect dst_rect, CSS::ImageRendering) const override;

    void receive_frames(Vector<NonnullRefPtr<Gfx::Bitmap>>, u32 start_frame_index);

    i64 session_id() const { return m_session_id; }

    static void deliver_frames_for_session(i64 session_id, Vector<NonnullRefPtr<Gfx::Bitmap>>);
    static void install_frame_delivery_callback();

private:
    static HashMap<i64, GC::RawPtr<AnimatedBitmapDecodedImageData>>& session_registry();

    static constexpr u32 BUFFER_POOL_SIZE = 8;
    static constexpr u32 REQUEST_BATCH_SIZE = 4;

    struct BufferSlot {
        Optional<u32> frame_index;
        Optional<Gfx::DecodedImageFrame> frame;
        u64 generation { 0 };
    };

    AnimatedBitmapDecodedImageData(
        i64 session_id,
        u32 frame_count,
        u32 loop_count,
        Gfx::IntSize,
        Gfx::ColorSpace,
        Vector<u32> durations,
        GC::Ref<Platform::Timer>,
        GC::Ref<DOM::DocumentObserver>);

    virtual size_t external_memory_size() const override;

    virtual void reset_animation() override;
    virtual void start_animation() override;
    virtual void stop_animation() override;

    void advance_animation();
    bool animation_has_completed() const;

    int frame_duration(size_t frame_index) const;
    Optional<Gfx::DecodedImageFrame> frame(size_t frame_index, Gfx::IntSize = {}) const;

    BufferSlot const* find_slot(u32 frame_index) const;
    BufferSlot& evict_oldest_slot();
    void maybe_request_more_frames(size_t current_frame_index);

    i64 m_session_id;
    u32 m_frame_count;
    u32 m_loop_count;
    Gfx::IntSize m_size;
    Gfx::ColorSpace m_color_space;
    Vector<u32> m_durations;

    Array<BufferSlot, BUFFER_POOL_SIZE> m_buffer_slots;
    mutable Optional<Gfx::DecodedImageFrame> m_last_displayed_frame;
    u64 m_write_generation { 0 };
    bool m_request_in_flight { false };
    u32 m_current_frame_index { 0 };
    u32 m_last_requested_start_frame { 0 };
    u32 m_loops_completed { 0 };
    GC::Ref<Platform::Timer> m_animation_timer;
};

}
