/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWebView/Forward.h>
#include <LibWebView/PictureInPictureWindow.h>

namespace Ladybird {

NonnullOwnPtr<WebView::PictureInPictureWindow> create_picture_in_picture_window(WebView::CanonicalTraversable&, WebView::ViewImplementation const& owner_view, Gfx::IntSize video_size);

}
