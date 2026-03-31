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

@end
