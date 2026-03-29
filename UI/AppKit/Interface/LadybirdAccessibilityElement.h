/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>

namespace WebView {

struct AccessibilityNodeData;
class AccessibilityTreeManager;

}

#import <Interface/LadybirdAccessibilityViewProtocol.h>

@interface LadybirdAccessibilityElement : NSObject

- (instancetype)initWithNodeID:(int64_t)nodeID
                       manager:(WebView::AccessibilityTreeManager const*)manager
                          view:(id<LadybirdAccessibilityView>)view;

@property (nonatomic, readonly) int64_t nodeID;

@end
