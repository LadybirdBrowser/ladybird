/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <Ladybird/Types.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWebView/CookieJar.h>

#import <System/Cocoa.h>

@class Tab;
@class TabController;

@interface ApplicationDelegate : NSObject <NSApplicationDelegate>

- (nullable instancetype)init:(Vector<URL::URL>)initial_urls
                newTabPageURL:(URL::URL)new_tab_page_url
                withCookieJar:(NonnullOwnPtr<WebView::CookieJar>)cookie_jar
            webContentOptions:(Ladybird::WebContentOptions const&)web_content_options
      webdriverContentIPCPath:(StringView)webdriver_content_ipc_path
                  allowPopups:(BOOL)allow_popups;

- (nonnull TabController*)createNewTab:(Optional<URL::URL> const&)url
                               fromTab:(nullable Tab*)tab
                           activateTab:(Web::HTML::ActivateTab)activate_tab;

- (nonnull TabController*)createNewTab:(StringView)html
                                   url:(URL::URL const&)url
                               fromTab:(nullable Tab*)tab
                           activateTab:(Web::HTML::ActivateTab)activate_tab;

- (void)setActiveTab:(nonnull Tab*)tab;
- (nullable Tab*)activeTab;

- (void)removeTab:(nonnull TabController*)controller;

- (WebView::CookieJar&)cookieJar;
- (Ladybird::WebContentOptions const&)webContentOptions;
- (Optional<StringView> const&)webdriverContentIPCPath;
- (Web::CSS::PreferredColorScheme)preferredColorScheme;
- (Web::CSS::PreferredContrast)preferredContrast;
- (Web::CSS::PreferredMotion)preferredMotion;
- (WebView::SearchEngine const&)searchEngine;

@end
