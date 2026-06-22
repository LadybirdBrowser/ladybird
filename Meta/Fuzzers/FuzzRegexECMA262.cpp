/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibRegex/ECMAScriptRegex.h>
#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);
    auto pattern = Utf16String::from_utf8_with_replacement_character(StringView(static_cast<unsigned char const*>(data), size));
    [[maybe_unused]] auto re = regex::ECMAScriptRegex::compile(pattern.utf16_view(), {});
    return 0;
}
