/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibRegex/Regex.h>
#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);
    auto pattern = String::from_utf8_with_replacement_character(StringView(static_cast<unsigned char const*>(data), size));
    [[maybe_unused]] auto re = Regex<PosixExtended>(pattern);
    return 0;
}
