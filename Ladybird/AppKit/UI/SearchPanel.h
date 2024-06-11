/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>

#import <System/Cocoa.h>

@interface SearchPanel : NSStackView

- (void)find:(id)selector;
- (void)findNextMatch:(id)selector;
- (void)findPreviousMatch:(id)selector;
- (void)useSelectionForFind:(id)selector;
- (void)onFindInPageResult:(size_t)current_match_index
           totalMatchCount:(Optional<size_t> const&)total_match_count;

@end
