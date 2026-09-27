/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalWindow.h>

namespace WebView {

// HACK: A process keeps opaque origins of its own, so it may give any of them for an environment whose origin is
//       opaque. This is transitional until every process takes its opaque origins from the UI process.
bool CanonicalEnvironmentSettingsObject::is_origin_given_by_its_process(URL::Origin const& origin) const
{
    if (this->origin().is_opaque())
        return origin.is_opaque();
    return origin.is_same_origin(this->origin());
}

bool CanonicalEnvironmentSettingsObject::may_use_cookies_of(URL::URL const& url) const
{
    return !origin().is_opaque() && url.origin().is_same_origin(origin());
}

CanonicalEnvironmentSettingsObject::CanonicalEnvironmentSettingsObject(CanonicalWindow& window, Web::HTML::EnvironmentId id)
    : m_window(window)
    , m_id(move(id))
{
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-settings-for-window-objects:concept-settings-object-origin
URL::Origin const& CanonicalEnvironmentSettingsObject::origin() const
{
    // Return the origin of window's associated Document.
    return m_window.associated_document().origin();
}

// https://storage.spec.whatwg.org/#obtain-a-storage-key
Optional<Web::StorageAPI::StorageKey> obtain_a_storage_key(CanonicalEnvironmentSettingsObject const& environment)
{
    // 1. Let key be the result of running obtain a storage key for non-storage purposes with environment.
    auto key = Web::StorageAPI::obtain_a_storage_key_for_non_storage_purposes(environment.origin());

    // AD-HOC: file:// URLs are opaque, but other browsers support storage on file:// URLs.
    if (key.origin.is_opaque_file_origin())
        key.origin = URL::Origin { "file"_string, String {}, {} };

    // 2. If key’s origin is an opaque origin, then return failure.
    if (key.origin.is_opaque())
        return {};

    // FIXME: 3. If the user has disabled storage, then return failure.

    // 4. Return key.
    return key;
}

}
