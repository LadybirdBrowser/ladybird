/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/Origin.h>
#include <LibWebView/Export.h>

namespace WebView {

// Column form of an Optional<URL::Origin>, richer than the lossy Origin::serialize() (which drops the
// tuple domain and renders opaque origins as "null").
struct PersistedOrigin {
    i64 kind { 0 };
    Optional<URL::Origin::OpaqueData::Nonce> nonce {};
    Optional<String> scheme {};
    Optional<String> host {};
    Optional<u16> port {};
    Optional<String> domain {};
};

WEBVIEW_API PersistedOrigin encode_origin(Optional<URL::Origin> const&);
WEBVIEW_API ErrorOr<Optional<URL::Origin>> decode_origin(PersistedOrigin const&);

}
