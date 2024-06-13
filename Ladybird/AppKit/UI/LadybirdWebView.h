/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGfx/Forward.h>
#include <LibURL/Forward.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/PreferredContrast.h>
#include <LibWeb/CSS/PreferredMotion.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWebView/Forward.h>

#import <System/Cocoa.h>

@protocol LadybirdWebViewObserver <NSObject>

- (String const&)onCreateNewTab:(URL::URL const&)url
                    activateTab:(Web::HTML::ActivateTab)activate_tab;

- (String const&)onCreateNewTab:(StringView)html
                            url:(URL::URL const&)url
                    activateTab:(Web::HTML::ActivateTab)activate_tab;

- (void)loadURL:(URL::URL const&)url;
- (void)onLoadStart:(URL::URL const&)url isRedirect:(BOOL)is_redirect;
- (void)onLoadFinish:(URL::URL const&)url;

- (void)onURLChange:(URL::URL const&)url;
- (void)onBackNavigationEnabled:(BOOL)back_enabled
       forwardNavigationEnabled:(BOOL)forward_enabled;

- (void)onTitleChange:(ByteString const&)title;
- (void)onFaviconChange:(Gfx::Bitmap const&)bitmap;
- (void)onAudioPlayStateChange:(Web::HTML::AudioPlayState)play_state;

- (void)onFindInPageResult:(size_t)current_match_index
           totalMatchCount:(Optional<size_t> const&)total_match_count;

@end

@interface LadybirdWebView : NSClipView <NSMenuDelegate>

- (instancetype)init:(id<LadybirdWebViewObserver>)observer;

- (void)loadURL:(URL::URL const&)url;
- (void)loadHTML:(StringView)html;

- (void)navigateBack;
- (void)navigateForward;
- (void)reload;

- (WebView::ViewImplementation&)view;
- (String const&)handle;

- (void)handleResize;
- (void)handleDevicePixelRatioChange;
- (void)handleScroll;
- (void)handleVisibility:(BOOL)is_visible;

- (void)setPreferredColorScheme:(Web::CSS::PreferredColorScheme)color_scheme;
- (void)setPreferredContrast:(Web::CSS::PreferredContrast)contrast;
- (void)setPreferredMotion:(Web::CSS::PreferredMotion)motion;

- (void)findInPage:(NSString*)query
    caseSensitivity:(CaseSensitivity)case_sensitivity;
- (void)findInPageNextMatch;
- (void)findInPagePreviousMatch;

- (void)zoomIn;
- (void)zoomOut;
- (void)resetZoom;
- (float)zoomLevel;

- (void)debugRequest:(ByteString const&)request argument:(ByteString const&)argument;

- (void)viewSource;

@end
