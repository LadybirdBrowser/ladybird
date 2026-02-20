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

@protocol AutocompleteObserver <NSObject>

- (void)onSelectedSuggestion:(String)suggestion;

@end

@interface Autocomplete : NSPopover

- (instancetype)init:(id<AutocompleteObserver>)observer
     withToolbarItem:(NSToolbarItem*)toolbar_item;

- (void)showWithSuggestions:(Vector<WebView::AutocompleteSuggestion>)suggestions;
- (BOOL)close;

- (Optional<String>)selectedSuggestion;

- (BOOL)selectNextSuggestion;
- (BOOL)selectPreviousSuggestion;

@end
