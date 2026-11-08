/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWebView/ContextMenuRequest.h>
#include <UI/Qt/Actions.h>

namespace Ladybird {

class RemoteView;

// Makes the menu for what the user asked a context menu on in a view.
NonnullRefPtr<Menu> create_context_menu(RemoteView&, WebView::ContextMenuRequest const&);

}
