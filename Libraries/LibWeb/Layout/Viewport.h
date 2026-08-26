/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/SearchableText.h>

namespace Web::Layout {

class Viewport final : public BlockContainer {
public:
    explicit Viewport(DOM::Document&, CSS::LayoutStyle);
    virtual ~Viewport() override;

    SearchableText const& searchable_text() { return m_searchable_text_cache.get(); }
    void invalidate_searchable_text_cache() { m_searchable_text_cache.invalidate(); }

    DOM::Document const& dom_node() const;

private:
    virtual bool is_viewport() const override { return true; }

    SearchableTextCache m_searchable_text_cache;
};

template<>
inline bool Node::fast_is<Viewport>() const { return is_viewport(); }

}
