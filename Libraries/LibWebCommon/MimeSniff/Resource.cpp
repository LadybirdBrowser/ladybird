/*
 * Copyright (c) 2023-2024, Kemal Zebari <kemalzebra@gmail.com>.
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/MimeSniff/Resource.h>
#include <LibWebCommon/RustFFI.h>

namespace Web::MimeSniff {

static FFI::SniffingContext to_ffi(SniffingContext sniffing_context)
{
    switch (sniffing_context) {
    case SniffingContext::None:
        return FFI::SniffingContext::None;
    case SniffingContext::Browsing:
        return FFI::SniffingContext::Browsing;
    case SniffingContext::Image:
        return FFI::SniffingContext::Image;
    case SniffingContext::AudioOrVideo:
        return FFI::SniffingContext::AudioOrVideo;
    case SniffingContext::Font:
        return FFI::SniffingContext::Font;
    case SniffingContext::TextOrBinary:
        return FFI::SniffingContext::TextOrBinary;
    }
    VERIFY_NOT_REACHED();
}

MimeType Resource::sniff(ReadonlyBytes data, SniffingConfiguration configuration)
{
    Optional<MimeType> computed_mime_type;
    auto sniff = [&](FFI::FfiMimeType const* supplied_type) {
        FFI::mime_sniff_sniff(data, to_ffi(configuration.sniffing_context), configuration.scheme.bytes(), supplied_type, configuration.no_sniff, &computed_mime_type, [](void* ctx, FFI::FfiMimeType const* mime_type) {
            *static_cast<Optional<MimeType>*>(ctx) = MimeType::from_ffi(*mime_type);
        });
    };

    if (configuration.supplied_type.has_value())
        configuration.supplied_type->lend_to_ffi([&](FFI::FfiMimeType const& supplied_type) { sniff(&supplied_type); });
    else
        sniff(nullptr);

    return computed_mime_type.release_value();
}

}
