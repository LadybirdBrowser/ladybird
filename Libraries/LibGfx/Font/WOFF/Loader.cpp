/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Resource.h>
#include <LibGfx/Font/WOFF/Loader.h>
#include <LibGfx/RustFFI.h>

namespace WOFF {

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_resource(Core::Resource const& resource, unsigned index)
{
    return try_load_from_bytes(resource.data(), index);
}

static u8* allocate_sfnt(void* context, size_t size)
{
    auto& sfnt = *static_cast<ErrorOr<Core::AnonymousBuffer>*>(context);
    sfnt = Core::AnonymousBuffer::create_with_size(size);
    return sfnt.is_error() ? nullptr : sfnt.value().data<u8>();
}

ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_from_bytes(ReadonlyBytes bytes, unsigned index)
{
    ErrorOr<Core::AnonymousBuffer> sfnt = Core::AnonymousBuffer {};
    size_t error_length = 0;
    if (auto const* error = Gfx::FFI::woff_rust_decode(bytes.data(), bytes.size(), &sfnt, allocate_sfnt, &error_length)) {
        if (sfnt.is_error())
            return sfnt.release_error();
        return Error::from_string_view(StringView { error, error_length });
    }
    return Gfx::Typeface::try_load_from_anonymous_buffer(sfnt.release_value(), index);
}

}
