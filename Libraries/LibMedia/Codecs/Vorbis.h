/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Time.h>

struct AVVorbisParseContext;

namespace Media::Codecs {

class Vorbis {
public:
    class Parser {
        AK_MAKE_NONCOPYABLE(Parser);

    public:
        static Optional<Parser> create(ReadonlyBytes codec_initialization_data);

        Parser(Parser&&);
        Parser& operator=(Parser&&);
        ~Parser();

        void reset() const;
        Optional<u32> parse_packet_duration_in_samples(ReadonlyBytes packet) const;
        Optional<AK::Duration> parse_packet_duration(ReadonlyBytes packet, u32 sample_rate) const;

    private:
        explicit Parser(AVVorbisParseContext*);

        AVVorbisParseContext* m_context { nullptr };
    };
};

}
