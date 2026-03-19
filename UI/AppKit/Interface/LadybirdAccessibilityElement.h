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

@class LadybirdWebView;

@interface LadybirdAccessibilityElement : NSObject

- (instancetype)initWithNodeID:(int64_t)nodeID
                       manager:(WebView::AccessibilityTreeManager const*)manager
                       webView:(LadybirdWebView*)webView;

@property (nonatomic, readonly) int64_t nodeID;

@end
