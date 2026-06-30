/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibURL/Origin.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>
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

struct PersistedReferrer {
    i64 kind { 0 };
    Optional<String> url {};
};

WEBVIEW_API PersistedReferrer encode_referrer(Web::Fetch::Infrastructure::Request::ReferrerType const&);
WEBVIEW_API ErrorOr<Web::Fetch::Infrastructure::Request::ReferrerType> decode_referrer(PersistedReferrer const&);

WEBVIEW_API i64 encode_referrer_policy(Web::ReferrerPolicy::ReferrerPolicy);
WEBVIEW_API ErrorOr<Web::ReferrerPolicy::ReferrerPolicy> decode_referrer_policy(i64 tag);

struct PersistedResource {
    i64 kind { 0 };
    Optional<Utf16String> string {};
};

// v1 does not persist POST bodies: a POST resource encodes as Empty, so decode never yields one.
WEBVIEW_API PersistedResource encode_resource(Web::HTML::DocumentResource const&);
WEBVIEW_API ErrorOr<Web::HTML::DocumentResource> decode_resource(PersistedResource const&);

WEBVIEW_API i64 encode_scroll_restoration_mode(Web::HTML::ScrollRestorationMode);
WEBVIEW_API ErrorOr<Web::HTML::ScrollRestorationMode> decode_scroll_restoration_mode(i64 tag);

}
