/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Forward.h>

#import <Cocoa/Cocoa.h>

@interface LocationSearchField : NSSearchField

- (void)setLoading:(BOOL)loading;
- (void)setFavicon:(NSImage*)favicon;
- (void)setShowsPageIcon:(BOOL)showsPageIcon;
- (void)setBookmarkAction:(WebView::Action&)action;
- (void)setZoomAction:(WebView::Action&)action;

@property (nonatomic, copy) void (^willBeginEditing)(void);

@end
