/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibMedia/Forward.h>

namespace Media::Codecs {

class FLAC {
public:
    struct FrameInfo {
        u64 sample_number;
        u16 block_size;
    };

    static bool is_sync_code(u16);
    static Optional<FrameInfo> parse_frame_header(MediaStreamCursor&, u16 sync_code, u16 fixed_block_size);
};

}
