/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

// The WebContentAccessibilityView NSView overlay we implement for the macOS Qt port also conforms to this.
@protocol LadybirdAccessibilityView <NSObject>

- (id)accessibilityElementForNodeID:(int64_t)nodeID;
- (NSRect)accessibilityScreenRectForViewRect:(NSRect)viewRect;
- (NSRect)accessibilityViewRectForScreenPoint:(NSPoint)screenPoint;
- (void)performAccessibilityAction:(NSString*)action forNodeID:(int64_t)nodeID;
- (NSURL*)accessibilityPageURL;
- (BOOL)accessibilityViewIsFirstResponder;
- (NSWindow*)window;

@end
