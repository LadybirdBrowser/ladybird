/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWebView/Autocomplete.h>

#import <Cocoa/Cocoa.h>

@protocol AutocompleteObserver <NSObject>

- (void)onSelectedSuggestion:(NSUInteger)suggestion_index;
- (void)onAutocompleteDidClose;

@end

@interface Autocomplete : NSObject

- (instancetype)init:(id<AutocompleteObserver>)observer
     withToolbarItem:(NSToolbarItem*)toolbar_item;

- (void)showWithSuggestions:(Vector<WebView::AutocompleteSuggestion>)suggestions
    selectedSuggestionIndex:(NSInteger)selected_suggestion_index;
- (void)setSelectedSuggestionIndex:(NSInteger)selected_suggestion_index;
- (void)clearSelection;
- (BOOL)close;
- (BOOL)isVisible;

@end
