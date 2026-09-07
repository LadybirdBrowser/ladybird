/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibWeb/HTML/SelectItem.h>

#import <Cocoa/Cocoa.h>

// The popup menu for a <select> element. Every -openWithEvent:forView:minimumWidth:items: ends in exactly one closed
// report — carrying the chosen item's id, or nothing when the menu went away without a choice — unless
// -closeWithoutReporting took the menu down.
@interface SelectDropdown : NSObject

- (instancetype)init;

// Pops the menu up for the view at the event's location, and returns once the menu has closed.
- (void)openWithEvent:(NSEvent*)event forView:(NSView*)view minimumWidth:(CGFloat)minimumWidth items:(Vector<Web::HTML::SelectItem> const&)items;
- (void)closeWithoutReporting;

- (void)setOnClosed:(Function<void(Optional<u32> const& selectedItemId)>)onClosed;

@property (nonatomic, readonly) NSMenu* menu;

@end
