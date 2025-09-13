/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringUtils.h>
#include <LibGfx/Forward.h>
#include <LibURL/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWeb/HTML/AudioPlayState.h>
#include <LibWebView/Forward.h>

#import <Cocoa/Cocoa.h>

@protocol LadybirdWebViewObserver <NSObject>

- (String const&)onCreateNewTab:(Optional<URL::URL> const&)url
                    activateTab:(Web::HTML::ActivateTab)activate_tab;

- (String const&)onCreateChildTab:(Optional<URL::URL> const&)url
                      activateTab:(Web::HTML::ActivateTab)activate_tab
                        pageIndex:(u64)page_index;

- (void)onLoadStart:(URL::URL const&)url isRedirect:(BOOL)is_redirect;
- (void)onLoadFinish:(URL::URL const&)url;

- (void)onURLChange:(URL::URL const&)url;
- (void)onTitleChange:(Utf16String const&)title;
- (void)onFaviconChange:(Gfx::Bitmap const&)bitmap;
- (void)onAudioPlayStateChange:(Web::HTML::AudioPlayState)play_state;

- (void)onFindInPageResult:(size_t)current_match_index
           totalMatchCount:(Optional<size_t> const&)total_match_count;

@end

@interface LadybirdWebView : NSView <NSMenuDelegate>

- (instancetype)init:(id<LadybirdWebViewObserver>)observer;
- (instancetype)initAsChild:(id<LadybirdWebViewObserver>)observer
                     parent:(LadybirdWebView*)parent
                  pageIndex:(u64)page_index;

- (void)loadURL:(URL::URL const&)url;

- (WebView::ViewImplementation&)view;
- (String const&)handle;

- (void)setWindowPosition:(Gfx::IntPoint)position;
- (void)setWindowSize:(Gfx::IntSize)size;

- (void)handleResize;
- (void)handleDevicePixelRatioChange;
- (void)handleDisplayRefreshRateChange;
- (void)handleVisibility:(BOOL)is_visible;

- (void)findInPage:(NSString*)query
    caseSensitivity:(CaseSensitivity)case_sensitivity;
- (void)findInPageNextMatch;
- (void)findInPagePreviousMatch;

@end
