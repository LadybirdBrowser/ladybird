/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/VideoFrameHolds.h>
#include <LibCompositing/Video/VideoFramePresentationPage.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibTest/TestCase.h>

using Compositing::VideoFramePresentationPage;

static NonnullRefPtr<Media::VideoFrame> create_pooled_frame(Media::VideoFramePool& pool)
{
    static constexpr Gfx::IntSize frame_size { 2, 2 };
    auto subsampling = Media::Subsampling::yuv420();
    auto layout = MUST(Media::frame_plane_layout(frame_size, 8, subsampling));
    auto acquired = pool.try_acquire(layout.total_byte_count).release_value();
    return make_ref_counted<Media::VideoFrame>(AK::Duration::zero(), AK::Duration::from_milliseconds(33), Gfx::Size<u32>(frame_size), 8, subsampling, Media::CodingIndependentCodePoints {}, move(acquired.storage));
}

// Every frame kept for a hold keeps its buffer open in the Compositor, so a WebContent holding one frame after another
// must not make it keep them all.
TEST_CASE(holds_keep_only_so_many_frames_past_their_presentation)
{
    auto web_content_page = MUST(VideoFramePresentationPage::create());
    auto compositor_page = MUST(VideoFramePresentationPage::create(web_content_page->buffer()));
    Media::VideoSinkHandle handle { 1 };

    Compositor::VideoFrameHolds holds;
    holds.add_sink(handle, compositor_page);

    auto pool = MUST(Media::VideoFramePool::create());
    Array frames { create_pooled_frame(*pool), create_pooled_frame(*pool) };

    // Each frame is held while it is presented, then kept for its hold once the next one replaces it.
    Vector<u64> held_generations;
    for (size_t i = 0; i <= Compositor::VideoFrameHolds::MAX_RETAINED_FRAME_COUNT + 1; i++) {
        holds.present_frame(handle, frames[i % frames.size()], {});
        if (i > Compositor::VideoFrameHolds::MAX_RETAINED_FRAME_COUNT)
            break;
        held_generations.append(web_content_page->hold_current_generation().value());
    }

    EXPECT(holds.held_frame(handle, held_generations.first()) != nullptr);
    EXPECT(holds.held_frame(handle, held_generations[Compositor::VideoFrameHolds::MAX_RETAINED_FRAME_COUNT - 1]) != nullptr);
    EXPECT(holds.held_frame(handle, held_generations.last()) == nullptr);

    // The hold that kept no frame is still released as any other, and a released frame makes room for another.
    EXPECT(!holds.release_hold(handle, held_generations.last()).is_error());
    EXPECT(!holds.release_hold(handle, held_generations.first()).is_error());
    holds.present_frame(handle, frames[0], {});
    auto generation = web_content_page->hold_current_generation().value();
    holds.present_frame(handle, frames[1], {});
    EXPECT(holds.held_frame(handle, generation) != nullptr);
}
