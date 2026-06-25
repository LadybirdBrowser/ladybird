/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class ImageBox final : public ReplacedBox {
    LAYOUT_NODE(ImageBox, ReplacedBox);

public:
    ImageBox(DOM::Document&, GC::Ptr<DOM::Element>, CSS::ComputedProperties const&, ImageProvider const&);
    ImageBox(DOM::Document&, GC::Ptr<DOM::Element>, CSS::ComputedProperties const&, NonnullOwnPtr<ImageProvider>);
    virtual ~ImageBox() override;

    bool renders_as_alt_text() const;

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

    ImageProvider const& image_provider() const;
    ImageProvider& image_provider()
    {
        return const_cast<ImageProvider&>(const_cast<ImageBox const&>(*this).image_provider());
    }

    void dom_node_did_update_alt_text(Badge<ImageProvider>);

private:
    virtual CSS::SizeWithAspectRatio natural_size() const override;

    OwnPtr<ImageProvider> m_owned_image_provider;

    mutable Optional<CSSPixels> m_cached_alt_text_width;
};

}
