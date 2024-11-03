/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>
#import <Ladybird/AppKit/UI/LadybirdWebViewWindow.h>

@class LadybirdWebView;

@interface TaskManager : LadybirdWebViewWindow

- (instancetype)init;

@end
