/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Vector.h>
#include <LibWebView/BookmarkStore.h>

namespace WebView {

ErrorOr<Vector<BookmarkItem>> import_bookmarks_from_html(StringView html);

}
