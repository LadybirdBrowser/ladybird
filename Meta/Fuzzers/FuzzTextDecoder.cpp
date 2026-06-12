/*
 * Copyright (c) 2021-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTextCodec/Decoder.h>
#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);

    static constexpr StringView MAGIC_SEPARATOR = "|DATA|"sv;
    StringView data_string_view { data, size };
    auto separator_index = data_string_view.find(MAGIC_SEPARATOR);
    if (!separator_index.has_value())
        return 0;

    auto encoding = data_string_view.substring_view(0, separator_index.value());
    auto encoded_data = data_string_view.substring_view(separator_index.value() + MAGIC_SEPARATOR.length());
    auto decoder = TextCodec::decoder_for(encoding);
    if (!decoder.has_value())
        return 0;

    (void)decoder->to_utf8(encoded_data);
    return 0;
}
