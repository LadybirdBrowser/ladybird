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

class WebContentView;

// Attaches LadybirdAccessibilityElement objects to the QWidget's underlying NSView, bypassing Qt's accessibility bridge
// entirely. This uses the same NSAccessibility wrapper as the AppKit UI.
void install_accessibility(WebContentView* view);
void update_accessibility_tree(WebContentView* view);
void post_accessibility_focus_changed(WebContentView* view, i64 node_id);
void post_accessibility_announcement(String const& text, String const& live_value);

}
