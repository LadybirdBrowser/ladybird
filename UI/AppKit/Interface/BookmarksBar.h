/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

@class BookmarkFolderPopover;

@interface BookmarksBar : NSView

- (instancetype)init;

- (void)rebuild;

- (void)closeBookmarkFolders;
- (void)bookmarkFolderDidClose:(BookmarkFolderPopover*)folder;

- (void)showContextMenu:(id)control event:(NSEvent*)event;

@property (nonatomic, strong, readonly) NSString* selected_bookmark_menu_item_id;
@property (nonatomic, strong, readonly) NSString* selected_bookmark_menu_target_folder_id;

@end
