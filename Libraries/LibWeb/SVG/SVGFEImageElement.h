/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

class SVGFEImageElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFEImageElement>
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_PLATFORM_OBJECT(SVGFEImageElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFEImageElement);

public:
    virtual ~SVGFEImageElement() override = default;

    RefPtr<Gfx::ImmutableBitmap> current_image_bitmap(Gfx::IntSize = {}) const;
    Optional<Gfx::IntRect> content_rect() const;

private:
    SVGFEImageElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    void process_href(Optional<String> const& href);

    Optional<URL::URL> m_href;

    GC::Ptr<HTML::SharedResourceRequest> m_resource_request;
};

};
