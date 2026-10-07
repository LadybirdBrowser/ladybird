/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Menu.h>

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QMenu>
#include <QPointer>
#include <QWindow>
#include <qpa/qplatformmenu.h>
#include <qpa/qwindowsysteminterface.h>

#import <AppKit/AppKit.h>

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

static void hide_native_context_menu_shortcuts(QMenu& menu)
{
    static constexpr auto property = "LadybirdHideNativeContextMenuShortcuts";
    if (menu.property(property).toBool() || !menu.toNSMenu())
        return;
    menu.setProperty(property, true);

    // Qt ignores QAction::isShortcutVisibleInContextMenu() for native menus. Run after Qt's aboutToShow handling,
    // which can update the native items.
    QObject::connect(menu.platformMenu(), &QPlatformMenu::aboutToShow, &menu, [&menu] {
        for (auto* action : menu.actions()) {
            if (auto* submenu = action->menu())
                hide_native_context_menu_shortcuts(*submenu);
        }

        for (NSMenuItem* item in menu.toNSMenu().itemArray) {
            item.keyEquivalent = @"";
            item.keyEquivalentModifierMask = 0;
        }
    });
}

void execute_context_menu(QMenu& menu, QPoint const& global_position)
{
    auto* parent = menu.parentWidget();
    if (!parent) {
        menu.exec(global_position);
        return;
    }

    parent->window()->winId();

    // Page views have their own native window. Track the menu against the native widget under the popup position,
    // not always the top-level browser window.
    auto* source = QApplication::widgetAt(global_position);
    if (!source || source->window() != parent->window())
        source = parent;
    auto* window = source->windowHandle() ? source : source->nativeParentWidget();

    if (!menu.toNSMenu()) {
        menu.exec(global_position);
        return;
    }

    hide_native_context_menu_shortcuts(menu);

    // AppKit consumes the mouse release while tracking the menu. Use Qt's Cocoa popup implementation so its native
    // mouse-button state is reset afterwards.
    QPointer window_handle = window->windowHandle();
    menu.platformMenu()->showPopup(window_handle, QRect { window->mapFromGlobal(global_position), QSize {} }, nullptr);

    // Qt Widgets also keeps an implicit grab until it receives the last release. Send the releases swallowed by AppKit
    // through Qt's window-system interface to clear both that grab and QGuiApplication's cached mouse-button state.
    if (window_handle && NSEvent.pressedMouseButtons == 0) {
        auto buttons = QApplication::mouseButtons();
        auto position = QCursor::pos();

        for (unsigned bit = Qt::LeftButton; bit <= Qt::MaxMouseButton && window_handle; bit <<= 1) {
            auto button = static_cast<Qt::MouseButton>(bit);
            if (!buttons.testFlag(button))
                continue;
            buttons &= ~button;

            QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
                window_handle, window_handle->mapFromGlobal(position), position, buttons, button,
                QEvent::MouseButtonRelease, QApplication::queryKeyboardModifiers());
        }
    }
}

}
