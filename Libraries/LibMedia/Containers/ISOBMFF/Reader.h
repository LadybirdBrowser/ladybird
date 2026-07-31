/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/IterationDecision.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>

#include "Boxes.h"

namespace Media::ISOBMFF {

class Streamer;

// A box may only extend to the end of the file if it is in a top-level container (ISO/IEC 14496-12 § 4.2), which
// only the caller that read its header knows.
enum class IsTopLevel {
    Yes,
    No,
};

class MEDIA_API Reader {
public:
    static bool supports_codec(CodecID);

    static DecoderErrorOr<BoxHeader> read_box_header(Streamer&);
    static DecoderErrorOr<FullBoxHeader> read_full_box_header(Streamer&);

    // Invokes the consumer for each child box, seeking past the child afterwards so that consumers only need to
    // read the fields they care about.
    static DecoderErrorOr<void> parse_child_boxes(Streamer&, BoxHeader const&, IsTopLevel, Function<DecoderErrorOr<IterationDecision>(BoxHeader const&)> child_consumer);

    static DecoderErrorOr<FileType> parse_file_type_box(Streamer&, BoxHeader const&);
    static DecoderErrorOr<Movie> parse_movie_box(Streamer&, BoxHeader const&);

    // Resolves every sample described by the fragment's Track Run Boxes into a position within the stream and a
    // timestamp, using the contexts sourced from the Movie Box that the fragment belongs to.
    static DecoderErrorOr<MovieFragment> parse_movie_fragment_box(Streamer&, BoxHeader const&, TrackFragmentContexts const&);

private:
    static DecoderErrorOr<NonnullRefPtr<TrackEntry>> parse_track_box(Streamer&, BoxHeader const&);
    static DecoderErrorOr<i64> parse_edit_list_box(Streamer&, BoxHeader const&);
    static DecoderErrorOr<void> parse_media_box(Streamer&, BoxHeader const&, TrackEntry&);
    static DecoderErrorOr<void> parse_data_information_box(Streamer&, BoxHeader const&);
    static DecoderErrorOr<void> parse_sample_table_box(Streamer&, BoxHeader const&, TrackEntry&);
    static DecoderErrorOr<Vector<SampleEntry>> parse_sample_description_box(Streamer&, BoxHeader const&, HandlerType);
    static DecoderErrorOr<SampleEntry> parse_sample_entry(Streamer&, BoxHeader const&, HandlerType);
    static DecoderErrorOr<FixedArray<u8>> parse_elementary_stream_descriptor_box(Streamer&, BoxHeader const&, CodecID&);
    static DecoderErrorOr<void> parse_track_fragment_box(Streamer&, BoxHeader const&, MovieFragmentAddressing&, TrackFragmentContexts const&, MovieFragment&);
};

}
