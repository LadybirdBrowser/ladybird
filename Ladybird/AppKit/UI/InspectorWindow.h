/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Neil Viloria <neilcviloria@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#import <Cocoa/Cocoa.h>
#import <UI/Inspector.h>

@class LadybirdWebView;
@class Tab;

@interface InspectorWindow : NSWindow

- (instancetype)init:(Tab*)tab;

@end
