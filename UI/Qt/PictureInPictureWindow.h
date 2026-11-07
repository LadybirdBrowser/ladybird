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

class RemoteView;

NonnullOwnPtr<WebView::PictureInPictureWindow> create_picture_in_picture_window(RemoteView const& owner_view, u64 view_id, Gfx::IntSize video_size);

}
