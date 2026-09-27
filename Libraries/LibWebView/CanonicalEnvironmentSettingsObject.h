/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebView/Export.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/webappapis.html#environment-settings-object
class WEBVIEW_API CanonicalEnvironmentSettingsObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit CanonicalEnvironmentSettingsObject(Web::HTML::EnvironmentId id);

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-id
    Web::HTML::EnvironmentId const& id() const { return m_id; }

private:
    Web::HTML::EnvironmentId m_id;
};

}
