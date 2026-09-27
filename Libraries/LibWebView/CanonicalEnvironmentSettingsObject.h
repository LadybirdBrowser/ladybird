/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Origin.h>
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object
class WEBVIEW_API CanonicalEnvironmentSettingsObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    CanonicalEnvironmentSettingsObject(CanonicalWindow&, Web::HTML::EnvironmentId id);

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-id
    Web::HTML::EnvironmentId const& id() const { return m_id; }

    URL::Origin const& origin() const;

private:
    CanonicalWindow& m_window;
    Web::HTML::EnvironmentId m_id;
};

WEBVIEW_API Optional<Web::StorageAPI::StorageKey> obtain_a_storage_key(CanonicalEnvironmentSettingsObject const&);

}
