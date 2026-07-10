/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibURL/URL.h>
#include <LibWebView/PrivateBrowsing.h>

#import <Cocoa/Cocoa.h>

@class Tab;

@interface TabController : NSWindowController <NSWindowDelegate>

- (instancetype)init:(WebView::IsPrivate)is_private;
- (instancetype)initAsChild:(Tab*)parent
                  pageIndex:(u64)page_index;

- (WebView::IsPrivate)isPrivate;

- (void)loadURL:(URL::URL const&)url;

- (void)onLoadStart;
- (void)onLoadFinish;
- (void)onFaviconChange:(NSImage*)favicon;

- (void)onURLChange:(URL::URL const&)url;

- (void)onEnterFullscreenWindow;
- (void)onExitFullscreenWindow;

- (void)focusWebViewWhenActivated;
- (void)focusWebView;
- (void)focusLocationToolbarItem;

@end
