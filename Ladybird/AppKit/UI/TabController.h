/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibURL/URL.h>

#import <System/Cocoa.h>

struct TabSettings {
    BOOL should_show_line_box_borders { NO };
    BOOL scripting_enabled { YES };
    BOOL block_popups { YES };
    BOOL same_origin_policy_enabled { NO };
    ByteString user_agent_name { "Disabled"sv };
};

@interface TabController : NSWindowController <NSWindowDelegate>

- (instancetype)init:(BOOL)block_popups;

- (void)loadURL:(URL::URL const&)url;
- (void)loadHTML:(StringView)html url:(URL::URL const&)url;

- (void)onLoadStart:(URL::URL const&)url isRedirect:(BOOL)isRedirect;

- (void)onURLChange:(URL::URL const&)url;
- (void)onBackNavigationEnabled:(BOOL)back_enabled
       forwardNavigationEnabled:(BOOL)forward_enabled;

- (void)onTitleChange:(ByteString const&)title;

- (void)navigateBack:(id)sender;
- (void)navigateForward:(id)sender;
- (void)reload:(id)sender;
- (void)clearHistory;

- (void)debugRequest:(ByteString const&)request argument:(ByteString const&)argument;

- (void)focusLocationToolbarItem;

@end
