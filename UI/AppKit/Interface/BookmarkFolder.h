/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Forward.h>

#import <Cocoa/Cocoa.h>

@class BookmarksBar;

@interface BookmarkFolderPopover : NSPopover

- (instancetype)init:(WebView::Menu&)menu
        bookmarksBar:(BookmarksBar*)bookmarks_bar
        parentFolder:(BookmarkFolderPopover*)parent_folder;

- (void)showRelativeToView:(NSView*)view preferredEdge:(NSRectEdge)preferred_edge;

- (void)openChildFolder:(WebView::Menu&)menu relativeToView:(NSView*)view;
- (void)closeChildFolder;

- (void)close;

@end
