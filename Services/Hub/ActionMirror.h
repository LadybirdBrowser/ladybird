/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/WeakPtr.h>
#include <Hub/HubClientEndpoint.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Menu.h>

namespace Hub {

// Lets the UI show the Hub's actions and menus. The UI knows an action by a handle, sees it change, and activates it.
class ActionMirror {
public:
    static ActionMirror& the();

    ActionState state_of(WebView::Action&);
    Vector<MenuEntry> entries_of(WebView::Menu&);
    ApplicationActions application_actions();
    ViewActions view_actions(WebView::ViewImplementation&);

    void activate(u64 handle, Optional<bool> checked);
    void retire(u64 handle, WebView::Action const*);

private:
    u64 handle_of(WebView::Action&);

    HashMap<u64, WeakPtr<WebView::Action>> m_actions;
    HashMap<WebView::Action const*, u64> m_handles;
    u64 m_next_handle { 1 };
};

}
