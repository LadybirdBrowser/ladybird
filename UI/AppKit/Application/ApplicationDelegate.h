/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/HTML/ActivateTab.h>

#import <Cocoa/Cocoa.h>

@class Tab;
@class TabController;

@interface ApplicationDelegate : NSObject <NSApplicationDelegate>

- (nullable instancetype)init;

- (nonnull TabController*)createNewTab:(Optional<URL::URL> const&)url
                               fromTab:(nullable Tab*)tab
                           activateTab:(Web::HTML::ActivateTab)activate_tab;

- (nonnull TabController*)createNewTab:(StringView)html
                                   url:(URL::URL const&)url
                               fromTab:(nullable Tab*)tab
                           activateTab:(Web::HTML::ActivateTab)activate_tab;

- (nonnull TabController*)createChildTab:(Optional<URL::URL> const&)url
                                 fromTab:(nonnull Tab*)tab
                             activateTab:(Web::HTML::ActivateTab)activate_tab
                               pageIndex:(u64)page_index;

- (void)setActiveTab:(nonnull Tab*)tab;
- (nullable Tab*)activeTab;

- (void)removeTab:(nonnull TabController*)controller;

- (Web::CSS::PreferredColorScheme)preferredColorScheme;
- (Web::CSS::PreferredContrast)preferredContrast;
- (Web::CSS::PreferredMotion)preferredMotion;

@end
