/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Site.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalWindow.h>

namespace WebView {

bool CanonicalEnvironmentSettingsObject::may_use_cookies_of(URL::URL const& url) const
{
    return !origin().is_opaque() && url.origin().is_same_origin(origin());
}

CanonicalWindowEnvironmentSettingsObject::CanonicalWindowEnvironmentSettingsObject(CanonicalWindow& window, Web::HTML::EnvironmentId id)
    : CanonicalEnvironmentSettingsObject(move(id))
    , m_window(window)
{
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-settings-for-window-objects:concept-settings-object-origin
URL::Origin const& CanonicalWindowEnvironmentSettingsObject::origin() const
{
    // Return the origin of window's associated Document.
    return m_window.associated_document().origin();
}

Optional<URL::Origin> CanonicalWindowEnvironmentSettingsObject::top_level_origin() const
{
    // NB: The top-level origin of a window's settings object is that of the top-level document of its associated
    //     Document's browsing context.
    return m_window.associated_document().top_level_origin();
}

CanonicalWorkerEnvironmentSettingsObject::CanonicalWorkerEnvironmentSettingsObject(URL::Origin origin, Optional<URL::Origin> top_level_origin, bool has_cross_site_ancestor, Web::HTML::EnvironmentId id)
    : CanonicalEnvironmentSettingsObject(move(id))
    , m_origin(move(origin))
    , m_top_level_origin(move(top_level_origin))
    , m_has_cross_site_ancestor(has_cross_site_ancestor)
{
}

Optional<Utf16String> network_isolation_top_level_site(CanonicalEnvironmentSettingsObject const& environment)
{
    auto top_level_origin = environment.top_level_origin();
    if (!top_level_origin.has_value())
        return {};
    return URL::Site::serialize_for_partitioning(*top_level_origin);
}

// https://storage.spec.whatwg.org/#obtain-a-storage-key-for-non-storage-purposes
Web::StorageAPI::StorageKey obtain_a_storage_key_for_non_storage_purposes(CanonicalEnvironmentSettingsObject const& environment)
{
    // 1. Let origin be environment’s origin if environment is an environment settings object; otherwise environment’s
    //    creation URL’s origin.
    // 2. Return a tuple consisting of origin.
    return Web::StorageAPI::obtain_a_storage_key_for_non_storage_purposes(environment.origin());
}

// https://storage.spec.whatwg.org/#obtain-a-storage-key
Optional<Web::StorageAPI::StorageKey> obtain_a_storage_key(CanonicalEnvironmentSettingsObject const& environment)
{
    // 1. Let key be the result of running obtain a storage key for non-storage purposes with environment.
    auto key = obtain_a_storage_key_for_non_storage_purposes(environment);

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
