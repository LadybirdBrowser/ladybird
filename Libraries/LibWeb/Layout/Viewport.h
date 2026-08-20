/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/SearchableText.h>

namespace Web::Layout {

class Viewport final : public Box {
public:
    Viewport(DOM::Document&, BindToPreparedArenaSlot, Compositing::RustFFI::NodeSlotId, RustFFI::NodeKind);
    virtual ~Viewport() override;

    DOM::Document const& dom_node() const;
    SearchableText const& searchable_text() { return m_searchable_text_cache.get(); }
    void invalidate_searchable_text_cache() { m_searchable_text_cache.invalidate(); }

private:
    virtual bool is_viewport() const override { return true; }
    SearchableTextCache m_searchable_text_cache;
};

template<>
inline bool Node::fast_is<Viewport>() const { return is_viewport(); }

}
