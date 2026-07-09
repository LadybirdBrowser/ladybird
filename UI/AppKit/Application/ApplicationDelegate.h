/*
 * Copyright (c) 2023-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWebView/PrivateBrowsing.h>

#import <Cocoa/Cocoa.h>

@class Tab;
@class TabController;

class TabLocation {
private:
    enum class Kind {
        End,
        AfterTab,
    };

public:
    static TabLocation end() { return { Kind::End, nil }; }
    static TabLocation after_tab(Tab* _Nullable tab) { return { Kind::AfterTab, tab }; }

    bool is_after_tab() const { return m_kind == Kind::AfterTab; }
    Tab* _Nullable tab() const { return m_tab; }

private:
    TabLocation(Kind kind, Tab* _Nullable tab)
        : m_kind(kind)
        , m_tab(tab)
    {
    }

    Kind m_kind;
    Tab* _Nullable m_tab { nil };
};

@interface ApplicationDelegate : NSObject <NSApplicationDelegate>

- (nullable instancetype)init;

- (nonnull TabController*)createNewTab:(Web::HTML::ActivateTab)activate_tab
                               fromTab:(nullable Tab*)tab;

- (nonnull TabController*)createNewTab:(Optional<URL::URL> const&)url
                               fromTab:(nullable Tab*)tab
                             isPrivate:(WebView::IsPrivate)is_private
                           activateTab:(Web::HTML::ActivateTab)activate_tab
                           tabLocation:(TabLocation)tab_location;

- (nonnull TabController*)createChildTab:(Optional<URL::URL> const&)url
                                 fromTab:(nonnull Tab*)tab
                             activateTab:(Web::HTML::ActivateTab)activate_tab
                               pageIndex:(u64)page_index;

- (void)setActiveTab:(nonnull Tab*)tab;
- (nullable Tab*)activeTab;

- (void)removeTab:(nonnull TabController*)controller;
- (NSUInteger)tabCount;

- (void)restartPrivateBrowsingSession;

- (void)rebuildBookmarksMenu;

- (void)onDevtoolsEnabled;
- (void)onDevtoolsDisabled;

@end
