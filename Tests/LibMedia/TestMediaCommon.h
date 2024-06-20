/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibTest/TestCase.h>

#include <AK/Function.h>
#include <LibMedia/Containers/Matroska/Reader.h>
#include <LibMedia/VideoDecoder.h>
#include <LibMedia/VideoFrame.h>

template<typename T>
static inline void decode_video(StringView path, size_t expected_frame_count, T create_decoder)
{
    auto matroska_reader = MUST(Media::Matroska::Reader::from_file(path));
    u64 video_track = 0;
    MUST(matroska_reader.for_each_track_of_type(Media::Matroska::TrackEntry::TrackType::Video, [&](Media::Matroska::TrackEntry const& track_entry) -> Media::DecoderErrorOr<IterationDecision> {
        video_track = track_entry.track_number();
        return IterationDecision::Break;
    }));
    VERIFY(video_track != 0);

    auto iterator = MUST(matroska_reader.create_sample_iterator(video_track));
    size_t frame_count = 0;
    NonnullOwnPtr<Media::VideoDecoder> decoder = create_decoder(iterator);

    auto last_timestamp = Duration::min();

    while (frame_count <= expected_frame_count) {
        auto block_result = iterator.next_block();
        if (block_result.is_error() && block_result.error().category() == Media::DecoderErrorCategory::EndOfStream) {
            VERIFY(frame_count == expected_frame_count);
            return;
        }

        auto block = block_result.release_value();
        for (auto const& frame : block.frames()) {
            MUST(decoder->receive_sample(block.timestamp(), frame));
            while (true) {
                auto frame_result = decoder->get_decoded_frame();
                if (frame_result.is_error()) {
                    if (frame_result.error().category() == Media::DecoderErrorCategory::NeedsMoreInput)
                        break;
                    VERIFY_NOT_REACHED();
                }
                EXPECT(last_timestamp <= frame_result.value()->timestamp());
                last_timestamp = frame_result.value()->timestamp();
            }
            frame_count++;
        }
    }

    VERIFY_NOT_REACHED();
}
