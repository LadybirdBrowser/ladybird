/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalWindow.h>

namespace WebView {

NonnullRefPtr<CanonicalWindow> CanonicalWindow::create(NonnullRefPtr<CanonicalSimilarOriginWindowAgent> agent)
{
    return adopt_ref(*new CanonicalWindow(move(agent)));
}

CanonicalWindow::CanonicalWindow(NonnullRefPtr<CanonicalSimilarOriginWindowAgent> agent)
    : m_agent(move(agent))
{
}

CanonicalWindow::~CanonicalWindow() = default;

CanonicalEnvironmentSettingsObject const& CanonicalWindow::relevant_settings_object() const
{
    VERIFY(m_relevant_settings_object);
    return *m_relevant_settings_object;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#set-up-a-window-environment-settings-object
void CanonicalWindow::set_up_a_window_environment_settings_object(Optional<Web::HTML::EnvironmentId> id)
{
    VERIFY(!m_relevant_settings_object);

    // 3. Let settings object be a new environment settings object whose algorithms are defined as follows:
    // 4. If reservedEnvironment is non-null, then:
    //    1. Set settings object's id to reservedEnvironment's id, target browsing context to reservedEnvironment's
    //       target browsing context, and active service worker to reservedEnvironment's active service worker.
    //    2. Set reservedEnvironment's id to the empty string.
    // 5. Otherwise, set settings object's id to a new unique opaque string, settings object's target browsing context
    //    to null, and settings object's active service worker to null.
    // NB: The id is given when a process created the window's document before the UI process heard of it. The UI
    //     process generates the id of the environment it reserves for a navigation's window too.
    if (!id.has_value())
        id = Web::HTML::EnvironmentId::generate();

    // 7. Set realm's [[HostDefined]] field to settings object.
    m_relevant_settings_object = make<CanonicalEnvironmentSettingsObject>(id.release_value());
}

}
