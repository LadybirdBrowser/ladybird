/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/SharedMemoryValue.h>
#include <AK/Vector.h>
#include <LibCompositing/Video/VideoFrameHold.h>
#include <LibCompositing/Video/VideoFramePresentationPage.h>
#include <LibTest/TestCase.h>

using Compositing::VideoFrameHold;
using Compositing::VideoFramePresentationPage;

struct PagePair {
    NonnullRefPtr<VideoFramePresentationPage> web_content;
    NonnullRefPtr<VideoFramePresentationPage> compositor;
};

// Both processes map the same buffer, which two pages over one buffer stand in for.
static PagePair create_page_pair()
{
    auto web_content = MUST(VideoFramePresentationPage::create());
    auto compositor = MUST(VideoFramePresentationPage::create(web_content->buffer()));
    return { move(web_content), move(compositor) };
}

TEST_CASE(holds_are_counted_on_the_generation_they_were_taken_on)
{
    auto pages = create_page_pair();
    EXPECT_EQ(pages.compositor->replace_generation(1).generation, 0u);

    EXPECT_EQ(pages.web_content->hold_current_generation(), 1u);
    EXPECT_EQ(pages.web_content->hold_current_generation(), 1u);

    auto replaced = pages.compositor->replace_generation(2);
    EXPECT_EQ(replaced.generation, 1u);
    EXPECT_EQ(replaced.hold_count, 2u);
    EXPECT_EQ(pages.web_content->current_generation(), 2u);
}

TEST_CASE(holds_are_released_in_place_only_while_their_generation_is_current)
{
    auto pages = create_page_pair();
    pages.compositor->replace_generation(1);
    EXPECT_EQ(pages.web_content->hold_current_generation(), 1u);
    EXPECT_EQ(pages.web_content->release_hold_if_current(1), VideoFramePresentationPage::ReleaseResult::Released);
    EXPECT_EQ(pages.web_content->release_hold_if_current(1), VideoFramePresentationPage::ReleaseResult::NotHeld);

    EXPECT_EQ(pages.web_content->hold_current_generation(), 1u);
    EXPECT_EQ(pages.compositor->replace_generation(2).hold_count, 1u);
    EXPECT_EQ(pages.web_content->release_hold_if_current(1), VideoFramePresentationPage::ReleaseResult::NotCurrent);
}

TEST_CASE(a_retired_sink_cannot_be_held)
{
    auto pages = create_page_pair();
    pages.compositor->replace_generation(VideoFramePresentationPage::retired_generation);
    EXPECT(!pages.web_content->hold_current_generation().has_value());
}

TEST_CASE(an_unsent_hold_is_released_in_place)
{
    auto pages = create_page_pair();
    pages.compositor->replace_generation(1);
    Vector<u64> released_through_compositor;
    {
        auto hold = VideoFrameHold::hold_current_frame(pages.web_content, Media::VideoSinkHandle { 1 }, [&](auto, u64 generation) {
            released_through_compositor.append(generation);
        });
        EXPECT(hold);
        EXPECT_EQ(hold->generation(), 1u);
    }
    EXPECT(released_through_compositor.is_empty());
    EXPECT_EQ(pages.compositor->replace_generation(2).hold_count, 0u);
}

TEST_CASE(a_sent_hold_is_released_through_the_compositor)
{
    auto pages = create_page_pair();
    pages.compositor->replace_generation(1);
    Vector<u64> released_through_compositor;
    {
        auto hold = VideoFrameHold::hold_current_frame(pages.web_content, Media::VideoSinkHandle { 1 }, [&](auto, u64 generation) {
            released_through_compositor.append(generation);
        });
        hold->note_sent_to_compositor();
    }
    EXPECT_EQ(released_through_compositor, (Vector<u64> { 1 }));
    EXPECT_EQ(pages.compositor->replace_generation(2).hold_count, 1u);
}

TEST_CASE(an_unsent_hold_on_a_replaced_generation_is_released_through_the_compositor)
{
    auto pages = create_page_pair();
    pages.compositor->replace_generation(1);
    Vector<u64> released_through_compositor;
    {
        auto hold = VideoFrameHold::hold_current_frame(pages.web_content, Media::VideoSinkHandle { 1 }, [&](auto, u64 generation) {
            released_through_compositor.append(generation);
        });
        EXPECT_EQ(pages.compositor->replace_generation(2).hold_count, 1u);
    }
    EXPECT_EQ(released_through_compositor, (Vector<u64> { 1 }));
}

// The Compositor writes the presented frame record that WebContent reads, so WebContent must not believe a time that
// is not a Duration or dimensions no decoded frame could have.
TEST_CASE(forged_presented_frames_are_not_believed)
{
    auto pages = create_page_pair();

    VideoFramePresentationPage::PresentedFrame presented_frame;
    presented_frame.presented_frame_count = 1;
    presented_frame.width = 1920;
    presented_frame.height = 1080;
    SharedMemoryCodec<AK::Duration>::Representation forged_time { 3, 2'000'000'000 };
    presented_frame.media_time = bit_cast<SharedMemoryValue<AK::Duration>>(forged_time);
    pages.compositor->publish_presented_frame(presented_frame);

    auto read_back = pages.web_content->presented_frame();
    EXPECT(read_back.has_value());
    EXPECT_EQ(static_cast<AK::Duration>(read_back->media_time), AK::Duration::zero());
    EXPECT_EQ(read_back->width, 1920u);

    presented_frame.width = 1u << 20;
    pages.compositor->publish_presented_frame(presented_frame);
    EXPECT(!pages.web_content->presented_frame().has_value());
}
