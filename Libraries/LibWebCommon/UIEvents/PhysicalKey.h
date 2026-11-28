/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebCommon/Export.h>
#include <LibWebCommon/UIEvents/KeyCode.h>

namespace Web::UIEvents {

// These name a key by its position, as the key a US keyboard has there. They cover the keys whose characters depend
// on the keyboard layout (letters, digits and punctuation), and return Key_Invalid for every other key.

// A PC scan code (set 1), as Windows reports it. Linux's evdev key codes are the same numbers for these keys.
WEBCOMMON_API KeyCode physical_key_from_scan_code(u32 scan_code);

// A macOS virtual key code, as NSEvent's keyCode reports it.
WEBCOMMON_API KeyCode physical_key_from_macos_virtual_key(u32 virtual_key);

}
