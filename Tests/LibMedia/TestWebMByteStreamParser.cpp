/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibMedia/MediaSourceExtensions/WebMByteStreamParser.h>
#include <LibMedia/ReadonlyBytesCursor.h>

#include "TestMediaCommon.h"

static Vector<Media::CodedFrame> parse_every_coded_frame(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto data = MUST(file->read_until_eof());
    auto cursor = adopt_ref(*new Media::ReadonlyBytesCursor(data.bytes()));
    Media::MediaSourceExtensions::WebMByteStreamParser parser;

    Vector<Media::CodedFrame> coded_frames;
    while (true) {
        // The segment parser loop waits for more data where the bytes run out.
        if (auto skip_result = parser.skip_ignored_bytes(*cursor); skip_result.is_error()) {
            EXPECT_EQ(skip_result.error().category(), Media::DecoderErrorCategory::EndOfStream);
            return coded_frames;
        }
        if (cursor->position() >= data.size())
            return coded_frames;

        auto segment_type = MUST(parser.sniff_segment_type(*cursor));
        if (segment_type == Media::MediaSourceExtensions::SegmentType::InitializationSegment) {
            MUST(parser.parse_initialization_segment(*cursor));
            continue;
        }
        VERIFY(segment_type == Media::MediaSourceExtensions::SegmentType::MediaSegment);
        while (true) {
            auto position = cursor->position();
            auto result = MUST(parser.parse_media_segment(*cursor));
            auto made_progress = !result.coded_frames.is_empty() || cursor->position() != position;
            for (auto& demuxed_coded_frame : result.coded_frames)
                coded_frames.append(move(demuxed_coded_frame.coded_frame));
            if (result.completed_segment)
                break;
            // All of the file is already given to the parser, so a cluster it cannot finish will never finish.
            if (!made_progress) {
                FAIL("The file ends inside a cluster");
                return coded_frames;
            }
        }
    }
}

// The file was encoded from hevc_hdr10_plus_in_sei.mkv, so its frames carry the same HDR10+, in BlockAdditions.
TEST_CASE(coded_frames_carry_the_hdr10_plus_scene_peaks_of_their_block_additions)
{
    ScenePeakChecker scene_peaks { hevc_hdr10_plus_in_sei_scene_peaks };
    auto coded_frames = parse_every_coded_frame("./vp9_hdr10_plus_in_block_additions.webm"sv);
    EXPECT_EQ(coded_frames.size(), hevc_hdr10_plus_in_sei_scene_peaks.size());
    for (auto const& coded_frame : coded_frames)
        scene_peaks.check(coded_frame.presentation_timestamp(), coded_frame.maximum_scene_luminance());
    scene_peaks.expect_every_frame_checked();
}
