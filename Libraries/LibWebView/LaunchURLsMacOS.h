/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Vector.h>

namespace WebView {

// Takes the URLs and files from the Apple events that macOS queued when it launched this process to open them. Those
// never appear in argv — LaunchServices delivers them in kAEGetURL and kAEOpenDocuments events after launch.
Vector<ByteString> take_urls_from_launch_apple_events();

}
