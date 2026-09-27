/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object
class WEBVIEW_API CanonicalEnvironmentSettingsObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~CanonicalEnvironmentSettingsObject() = default;

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-id
    Web::HTML::EnvironmentId const& id() const { return m_id; }

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-settings-object-origin
    virtual URL::Origin const& origin() const = 0;

    bool is_origin_given_by_its_process(URL::Origin const&) const;
    bool may_use_cookies_of(URL::URL const&) const;

protected:
    explicit CanonicalEnvironmentSettingsObject(Web::HTML::EnvironmentId id)
        : m_id(move(id))
    {
    }

private:
    Web::HTML::EnvironmentId m_id;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#script-settings-for-window-objects
class WEBVIEW_API CanonicalWindowEnvironmentSettingsObject final : public CanonicalEnvironmentSettingsObject {
public:
    CanonicalWindowEnvironmentSettingsObject(CanonicalWindow&, Web::HTML::EnvironmentId id);

    virtual URL::Origin const& origin() const override;

private:
    CanonicalWindow& m_window;
};

// https://html.spec.whatwg.org/multipage/workers.html#script-settings-for-workers
class WEBVIEW_API CanonicalWorkerEnvironmentSettingsObject final : public CanonicalEnvironmentSettingsObject {
public:
    CanonicalWorkerEnvironmentSettingsObject(URL::Origin, Web::HTML::EnvironmentId id);

    virtual URL::Origin const& origin() const override { return m_origin; }

private:
    URL::Origin m_origin;
};

WEBVIEW_API Web::StorageAPI::StorageKey obtain_a_storage_key_for_non_storage_purposes(CanonicalEnvironmentSettingsObject const&);
WEBVIEW_API Optional<Web::StorageAPI::StorageKey> obtain_a_storage_key(CanonicalEnvironmentSettingsObject const&);

}
