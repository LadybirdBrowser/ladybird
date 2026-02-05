/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/ActivateTab.h>

#import <Cocoa/Cocoa.h>

@class Tab;
@class TabController;

@interface ApplicationDelegate : NSObject <NSApplicationDelegate>

- (nullable instancetype)init;

- (nonnull TabController*)createNewTab:(Web::HTML::ActivateTab)activate_tab
                               fromTab:(nullable Tab*)tab;

- (nonnull TabController*)createNewTab:(Optional<URL::URL> const&)url
                               fromTab:(nullable Tab*)tab
                           activateTab:(Web::HTML::ActivateTab)activate_tab;

- (nonnull TabController*)createChildTab:(Optional<URL::URL> const&)url
                                 fromTab:(nonnull Tab*)tab
                             activateTab:(Web::HTML::ActivateTab)activate_tab
                               pageIndex:(u64)page_index;

- (void)setActiveTab:(nonnull Tab*)tab;
- (nullable Tab*)activeTab;

- (void)removeTab:(nonnull TabController*)controller;

- (void)onDevtoolsEnabled;
- (void)onDevtoolsDisabled;

@end
