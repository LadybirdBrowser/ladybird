/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibCompositing/PageId.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Forward.h>

#import <Cocoa/Cocoa.h>
#import <Interface/LadybirdWebViewWindow.h>

@class BookmarksBar;
@class LadybirdWebView;

@interface Tab : LadybirdWebViewWindow

- (instancetype)init:(WebView::IsPrivate)is_private;
- (instancetype)initAsChild:(Tab*)parent
                pageProcess:(WebView::WebContentClient&)page_process
                  pageIndex:(Compositing::PageId)page_index;

- (WebView::IsPrivate)isPrivate;

- (BookmarksBar*)bookmarksBar;

- (void)rebuildBookmarksBar;
- (void)updateBookmarksBarDisplay:(bool)show_bookmarks_bar;

@end
