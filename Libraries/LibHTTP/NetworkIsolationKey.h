/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>

namespace HTTP {

// The context a request is made in. It partitions the network state the request uses, so that one site cannot detect
// what another site has loaded. This follows Chrome's network isolation key: the site of the top-level document, the
// site of the document making the request, and three flags.
struct NetworkIsolationKey {
    // The serialized site of the top-level document, as URL::Site::serialize_for_partitioning() gives it.
    Utf16String top_level_site;

    // The serialized site of the document making the request, or of the document a navigation request creates. This is
    // empty if that document's origin is opaque: such a document is a site of its own that nothing else can address, so
    // its requests do not use the disk cache.
    Optional<Utf16String> frame_site;

    // Whether the request is for the document of a child navigable.
    bool is_subframe_document { false };

    // Whether the request is a top-level navigation that no document of the same site initiated.
    bool is_cross_site_main_frame_navigation { false };

    // Whether the document making the request, or the one a navigation request creates, has an ancestor of another
    // site. Such a document is a third-party context even under a top-level document of its own site.
    bool has_cross_site_ancestor { false };

    // The partition of the disk cache the request uses, or nothing if the request must not use the disk cache.
    Optional<Utf16String> disk_cache_partition() const;

    bool operator==(NetworkIsolationKey const&) const = default;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, HTTP::NetworkIsolationKey const&);

template<>
ErrorOr<HTTP::NetworkIsolationKey> decode(Decoder&);

}
