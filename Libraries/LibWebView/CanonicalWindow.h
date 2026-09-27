/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Utf16String.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#window
class WEBVIEW_API CanonicalWindow final : public RefCounted<CanonicalWindow> {
public:
    static NonnullRefPtr<CanonicalWindow> create(NonnullRefPtr<CanonicalSimilarOriginWindowAgent>);

    ~CanonicalWindow();

    // NB: The agent of the Window's realm.
    CanonicalSimilarOriginWindowAgent& agent() const { return m_agent; }

    CanonicalEnvironmentSettingsObject const& relevant_settings_object() const;
    void set_up_a_window_environment_settings_object(Optional<Web::HTML::EnvironmentId> id);

private:
    explicit CanonicalWindow(NonnullRefPtr<CanonicalSimilarOriginWindowAgent>);

    NonnullRefPtr<CanonicalSimilarOriginWindowAgent> m_agent;
    OwnPtr<CanonicalEnvironmentSettingsObject> m_relevant_settings_object;
};

}
