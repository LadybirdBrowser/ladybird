/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <LibGfx/RustFFI.h>

namespace WOFF2 {

static u8* allocate_sfnt(void* context, size_t size)
{
    auto& sfnt = *static_cast<ErrorOr<Core::AnonymousBuffer>*>(context);
    sfnt = Core::AnonymousBuffer::create_with_size(size);
    return sfnt.is_error() ? nullptr : sfnt.value().data<u8>();
}

ErrorOr<Core::AnonymousBuffer> convert_to_ttf(ReadonlyBytes bytes)
{
    ErrorOr<Core::AnonymousBuffer> sfnt = Core::AnonymousBuffer {};
    size_t error_length = 0;
    if (auto const* error = Gfx::FFI::woff2_rust_decode(bytes.data(), bytes.size(), &sfnt, allocate_sfnt, &error_length)) {
        if (sfnt.is_error())
            return sfnt.release_error();
        return Error::from_string_view(StringView { error, error_length });
    }
    return sfnt;
}

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_bytes(ReadonlyBytes bytes)
{
    return Gfx::Typeface::try_load_from_anonymous_buffer(TRY(convert_to_ttf(bytes)));
}

}
