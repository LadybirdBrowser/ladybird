/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWebView/AccessibilityTreeManager.h>

class QWidget;

namespace Ladybird {

class WebContentView;

// Attaches LadybirdAccessibilityElement objects to the QWidget's underlying NSView — bypassing Qt's Cocoa accessibility
// bridge entirely.
void install_accessibility(WebContentView* view);
// take_initial_focus: a page's first tree makes the view the window's first responder and announces the load; the
// trees DOM mutations bring leave both alone. report_focus: the first tree an assistive technology that arrived
// mid-page gets has AppKit look the focus up again, if the view holds it.
void update_accessibility_tree(WebContentView* view, bool take_initial_focus, bool report_focus);
void post_accessibility_focus_changed(WebContentView* view, i64 node_id);
void post_accessibility_announcement(String const& text, String const& live_value);

}
