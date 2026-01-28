/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Containers/Matroska/Reader.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <stddef.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    auto stream = Media::IncrementallyPopulatedStream::create_from_data({ data, size });
    auto matroska_reader_result = Media::Matroska::Reader::from_stream(stream->create_cursor());
    if (matroska_reader_result.is_error())
        return 0;
    (void)matroska_reader_result.value().duration();
    (void)matroska_reader_result.value().track_count();
    return 0;
}
