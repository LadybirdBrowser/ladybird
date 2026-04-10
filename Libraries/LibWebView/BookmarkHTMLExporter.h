/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <LibWebView/BookmarkStore.h>

namespace WebView {

ErrorOr<String> export_bookmarks_to_html(ReadonlySpan<BookmarkItem> items);

}
