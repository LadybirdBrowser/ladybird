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
        AtIndex,
    };

public:
    static TabLocation end() { return { Kind::End, nil, 0 }; }
    static TabLocation after_tab(Tab* _Nullable tab) { return { Kind::AfterTab, tab, 0 }; }
    static TabLocation at_index(i64 index) { return { Kind::AtIndex, nil, index }; }

    bool is_after_tab() const { return m_kind == Kind::AfterTab; }
    bool is_at_index() const { return m_kind == Kind::AtIndex; }
    Tab* _Nullable tab() const { return m_tab; }
    i64 index() const { return m_index; }

private:
    TabLocation(Kind kind, Tab* _Nullable tab, i64 index)
        : m_kind(kind)
        , m_tab(tab)
        , m_index(index)
    {
    }

    Kind m_kind;
    Tab* _Nullable m_tab { nil };
    i64 m_index { 0 };
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

- (void)reconcileSessionTopology;
- (void)removeTab:(nonnull TabController*)controller;
- (NSUInteger)tabCount;

- (void)restartPrivateBrowsingSession;

- (void)rebuildBookmarksMenu;

- (void)onDevtoolsEnabled;
- (void)onDevtoolsDisabled;

@end
