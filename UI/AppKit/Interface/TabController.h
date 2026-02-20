/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Utf16String.h>
#include <LibURL/URL.h>

#import <Cocoa/Cocoa.h>

@class Tab;

@interface TabController : NSWindowController <NSWindowDelegate>

- (instancetype)init;
- (instancetype)initAsChild:(Tab*)parent
                  pageIndex:(u64)page_index;

- (void)loadURL:(URL::URL const&)url;

- (void)onLoadStart:(URL::URL const&)url isRedirect:(BOOL)isRedirect;
- (void)onLoadFinish:(URL::URL const&)url;

- (void)onURLChange:(URL::URL const&)url;
- (void)onTitleChange:(Utf16String const&)title;

- (void)clearHistory;
- (void)bookmarkCurrentPage;

- (void)focusLocationToolbarItem;

@end
