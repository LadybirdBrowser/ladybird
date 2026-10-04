/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/NetworkIsolationKey.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace HTTP {

Optional<Utf16String> NetworkIsolationKey::disk_cache_partition() const
{
    if (!frame_site.has_value())
        return {};

    // NB: A serialized site never contains a space, so the parts cannot run into each other.
    return Utf16String::formatted("{} {} {}{}{}", top_level_site, *frame_site, is_subframe_document ? "s"sv : ""sv, is_cross_site_main_frame_navigation ? "cn"sv : ""sv, has_cross_site_ancestor ? "x"sv : ""sv);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, HTTP::NetworkIsolationKey const& key)
{
    TRY(encoder.encode(key.top_level_site));
    TRY(encoder.encode(key.frame_site));
    TRY(encoder.encode(key.is_subframe_document));
    TRY(encoder.encode(key.is_cross_site_main_frame_navigation));
    TRY(encoder.encode(key.has_cross_site_ancestor));
    return {};
}

template<>
ErrorOr<HTTP::NetworkIsolationKey> decode(Decoder& decoder)
{
    auto top_level_site = TRY(decoder.decode<Utf16String>());
    auto frame_site = TRY(decoder.decode<Optional<Utf16String>>());
    auto is_subframe_document = TRY(decoder.decode<bool>());
    auto is_cross_site_main_frame_navigation = TRY(decoder.decode<bool>());
    auto has_cross_site_ancestor = TRY(decoder.decode<bool>());

    auto is_valid_site = [](Utf16String const& site) { return !site.is_empty() && !site.contains(u' '); };
    if (!is_valid_site(top_level_site) || (frame_site.has_value() && !is_valid_site(*frame_site)))
        return Error::from_string_literal("Network isolation key with an invalid site");

    return HTTP::NetworkIsolationKey {
        .top_level_site = move(top_level_site),
        .frame_site = move(frame_site),
        .is_subframe_document = is_subframe_document,
        .is_cross_site_main_frame_navigation = is_cross_site_main_frame_navigation,
        .has_cross_site_ancestor = has_cross_site_ancestor,
    };
}

}
