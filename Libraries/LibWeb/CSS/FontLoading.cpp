/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/WOFF/Loader.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <LibWeb/CSS/FontLoading.h>
#include <LibWeb/Platform/ThreadPoolWork.h>

namespace Web::CSS {

static constexpr u32 woff_signature = 0x774F4646;
static constexpr u32 woff2_signature = 0x774F4632;

static u32 font_signature(ByteBuffer const& data)
{
    if (data.size() < 4)
        return 0;
    return (static_cast<u32>(data[0]) << 24) | (static_cast<u32>(data[1]) << 16) | (static_cast<u32>(data[2]) << 8) | data[3];
}

bool requires_off_thread_vector_font_preparation(ByteBuffer const& data)
{
    return font_signature(data) == woff2_signature;
}

// https://drafts.csswg.org/css-fonts-4/#fetch-a-font
// 2. ISSUE: Load a font from stream according to its type.
// NB: The type is the one the data has, whatever the response's Content-Type says.
ErrorOr<NonnullRefPtr<Gfx::Typeface const>> try_load_vector_font(ByteBuffer const& data)
{
    switch (font_signature(data)) {
    case woff_signature:
        return WOFF::try_load_from_bytes(data);
    case woff2_signature:
        return WOFF2::try_load_from_bytes(data);
    default:
        return Gfx::Typeface::try_load_from_temporary_memory(data);
    }
}

void prepare_vector_font_data_off_thread(ByteBuffer data, Function<void(ErrorOr<Core::AnonymousBuffer>)>&& on_complete)
{
    auto convert = [data = move(data)] {
        return WOFF2::convert_to_ttf(data);
    };
    Platform::run_on_thread_pool<ErrorOr<Core::AnonymousBuffer>>(move(convert), move(on_complete));
}

}
