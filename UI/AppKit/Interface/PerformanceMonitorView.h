/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>
#include <LibWebView/TabPerformanceStats.h>

@interface PerformanceMonitorView : NSView
- (void)updateStats:(WebView::TabPerformanceStats const&)stats;
@end
