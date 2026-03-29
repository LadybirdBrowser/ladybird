/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

// Protocol for the view methods that LadybirdAccessibilityElement needs.
// Both the AppKit LadybirdWebView and the Qt macOS bridge conform to this.
@protocol LadybirdAccessibilityView <NSObject>

- (id)accessibilityElementForNodeID:(int64_t)nodeID;
- (NSRect)accessibilityScreenRectForViewRect:(NSRect)viewRect;
- (NSRect)accessibilityViewRectForScreenPoint:(NSPoint)screenPoint;
- (void)performAccessibilityAction:(NSString*)action forNodeID:(int64_t)nodeID;
- (NSWindow*)window;

@end
