/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class ImageBox final : public ReplacedBox {
    JS_CELL(ImageBox, ReplacedBox);
    JS_DECLARE_ALLOCATOR(ImageBox);

public:
    ImageBox(DOM::Document&, DOM::Element&, NonnullRefPtr<CSS::StyleProperties>, ImageProvider const&);
    virtual ~ImageBox() override;

    virtual void prepare_for_replaced_layout() override;

    const DOM::Element& dom_node() const { return static_cast<const DOM::Element&>(ReplacedBox::dom_node()); }

    bool renders_as_alt_text() const;

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;

    auto const& image_provider() const { return m_image_provider; }
    auto& image_provider() { return m_image_provider; }

    void dom_node_did_update_alt_text(Badge<ImageProvider>);

private:
    virtual void visit_edges(Visitor&) override;

    ImageProvider const& m_image_provider;

    Optional<CSSPixels> m_cached_alt_text_width;
};

}
