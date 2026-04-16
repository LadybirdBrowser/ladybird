/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Autocomplete.h>

#import <Cocoa/Cocoa.h>

static constexpr auto MAXIMUM_VISIBLE_AUTOCOMPLETE_SUGGESTIONS = 8uz;

@protocol AutocompleteObserver <NSObject>

- (void)onSelectedSuggestion:(String)suggestion;
- (void)onHighlightedSuggestion:(String)suggestion;
- (void)onAutocompleteDidClose;

@end

@interface Autocomplete : NSObject

- (instancetype)init:(id<AutocompleteObserver>)observer
     withToolbarItem:(NSToolbarItem*)toolbar_item;

- (void)showWithSuggestions:(Vector<WebView::AutocompleteSuggestion>)suggestions
                selectedRow:(NSInteger)selected_row;
- (void)clearSelection;
- (BOOL)close;
- (BOOL)isVisible;

- (Optional<String>)selectedSuggestion;

- (BOOL)selectNextSuggestion;
- (BOOL)selectPreviousSuggestion;

@end
