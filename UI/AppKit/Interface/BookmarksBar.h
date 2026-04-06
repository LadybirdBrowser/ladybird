/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Point.h>
#include <LibWebView/Forward.h>

#import <Cocoa/Cocoa.h>

@class BookmarkFolderPopover;

@interface BookmarksBar : NSView

- (instancetype)init;

- (void)rebuild;

- (void)closeBookmarkFolders;
- (void)bookmarkFolderDidClose:(BookmarkFolderPopover*)folder;

- (void)showContextMenu:(id)control event:(NSEvent*)event;
- (void)showContextMenu:(Gfx::IntPoint)content_position
                   view:(NSView*)view
           bookmarkItem:(Optional<WebView::BookmarkItem const&>)item
         targetFolderID:(Optional<String const&>)target_folder_id;

@property (nonatomic, strong, readonly) NSString* selected_bookmark_menu_item_id;
@property (nonatomic, strong, readonly) NSString* selected_bookmark_menu_target_folder_id;

@end
