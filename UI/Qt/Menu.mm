/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Menu.h>

#include <QMenu>

#import <Cocoa/Cocoa.h>

namespace Ladybird {

void enable_menu_icons([[maybe_unused]] QMenu& menu)
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
    if (@available(macOS 27, *)) {
        NSMenu* native_menu = menu.toNSMenu();

        for (NSMenuItem* item in [native_menu itemArray]) {
            [item setPreferredImageVisibility:NSMenuItemImageVisibilityVisible];
        }
    }
#endif
}

}
