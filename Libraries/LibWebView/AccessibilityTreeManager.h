/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <LibGfx/Point.h>
#include <LibWebView/AccessibilityNodeData.h>
#include <LibWebView/Export.h>

namespace WebView {

class WEBVIEW_API AccessibilityTreeManager {
public:
    void update_tree(Vector<AccessibilityNodeData> nodes);

    Function<void(String, String)> on_live_region_changed;

    AccessibilityNodeData const* node(i64 id) const;
    AccessibilityNodeData const* root() const;
    AccessibilityNodeData const* hit_test(Gfx::IntPoint point) const;
    void set_focused_node(i64 node_id);

    bool is_empty() const { return m_nodes.is_empty(); }

private:
    AccessibilityNodeData const* hit_test_recursive(i64 node_id, Gfx::IntPoint point) const;

    HashMap<i64, AccessibilityNodeData> m_nodes;
    i64 m_root_id { -1 };
};

}
