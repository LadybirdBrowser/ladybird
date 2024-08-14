/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PropertyID.h>

namespace Web::CSS {

struct RequiredInvalidationAfterStyleChange {
    bool repaint : 1 { false };
    bool rebuild_stacking_context_tree : 1 { false };
    bool relayout : 1 { false };
    bool rebuild_layout_tree : 1 { false };

    void operator|=(RequiredInvalidationAfterStyleChange const& other)
    {
        repaint |= other.repaint;
        rebuild_stacking_context_tree |= other.rebuild_stacking_context_tree;
        relayout |= other.relayout;
        rebuild_layout_tree |= other.rebuild_layout_tree;
    }

    [[nodiscard]] bool is_none() const { return !repaint && !rebuild_stacking_context_tree && !relayout && !rebuild_layout_tree; }
    [[nodiscard]] bool is_full() const { return repaint && rebuild_stacking_context_tree && relayout && rebuild_layout_tree; }
    static RequiredInvalidationAfterStyleChange full() { return { true, true, true, true }; }
};

RequiredInvalidationAfterStyleChange compute_property_invalidation(CSS::PropertyID property_id, RefPtr<CSSStyleValue const> const& old_value, RefPtr<CSSStyleValue const> const& new_value);

}
