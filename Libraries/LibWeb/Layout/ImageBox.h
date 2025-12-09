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
    GC_CELL(ImageBox, ReplacedBox);
    GC_DECLARE_ALLOCATOR(ImageBox);

public:
    ImageBox(DOM::Document&, GC::Ptr<DOM::Element>, GC::Ref<CSS::ComputedProperties>, ImageProvider const&);
    virtual ~ImageBox() override;

    bool renders_as_alt_text() const;

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

    auto const& image_provider() const { return m_image_provider; }
    auto& image_provider() { return m_image_provider; }

    void dom_node_did_update_alt_text(Badge<ImageProvider>);

private:
    virtual void visit_edges(Visitor&) override;
    virtual CSS::SizeWithAspectRatio natural_size() const override;

    ImageProvider const& m_image_provider;

    mutable Optional<CSSPixels> m_cached_alt_text_width;
};

}
