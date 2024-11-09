/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>
#import <Interface/LadybirdWebViewWindow.h>

@class LadybirdWebView;

@interface TaskManager : LadybirdWebViewWindow

- (instancetype)init;

@end
