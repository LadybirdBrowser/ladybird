/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWebView/AccessibilityTreeManager.h>

class QWidget;

namespace Ladybird {

// On macOS, swizzles Qt's QMacAccessibilityElement at runtime to add
// AXWebArea role, landmark subroles, role descriptions, and search
// predicate support that VoiceOver requires for web content navigation.
void install_native_accessibility(QWidget* widget, WebView::AccessibilityTreeManager* manager);
void notify_accessibility_tree_loaded(QWidget* widget, WebView::AccessibilityTreeManager* manager);
void post_accessibility_focus_changed(QWidget* widget, i64 node_id);
void post_accessibility_announcement(String const& text, String const& live_value);

}
