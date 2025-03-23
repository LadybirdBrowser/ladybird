/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibIPC/Forward.h>
#include <LibMain/Main.h>
#include <LibWebView/Forward.h>

#import <Cocoa/Cocoa.h>

namespace Ladybird {
class WebViewBridge;
}

@interface Application : NSApplication

- (void)setupWebViewApplication:(Main::Arguments&)arguments;
- (ErrorOr<void>)launchServices;

@end
