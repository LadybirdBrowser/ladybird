/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

// The NSView that hosts the LadybirdAccessibilityElement tree: WebContentViewAccessibility.mm's overlay conforms to it.
@protocol LadybirdAccessibilityView <NSObject>

- (id)accessibilityElementForNodeID:(int64_t)nodeID;
- (NSRect)accessibilityScreenRectForViewRect:(NSRect)viewRect;
- (NSRect)accessibilityViewRectForScreenPoint:(NSPoint)screenPoint;
- (void)performAccessibilityAction:(NSString*)action forNodeID:(int64_t)nodeID;
- (NSURL*)accessibilityPageURL;
- (BOOL)accessibilityViewIsFirstResponder;
- (NSWindow*)window;

@end
