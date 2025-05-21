/*
 * Copyright (c) 2025, Ramon van Sprundel <ramonvansprundel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/Painter.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGPatternElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>
#include <LibWeb/SVG/SVGUseElement.h>
#include <LibWeb/SVG/TagNames.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGPatternElement);

void SVGPatternElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGPatternElement);
    m_view_box_for_bindings = realm.create<SVGAnimatedRect>(realm);
}

void SVGPatternElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin<SupportsXLinkHref::Yes>::visit_edges(visitor);
    visitor.visit(m_view_box_for_bindings);
}

SVGPatternElement::SVGPatternElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGPatternElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // Reset cached paint style when attributes change
    if (name == SVG::AttributeNames::width || name == SVG::AttributeNames::height || name == SVG::AttributeNames::x || name == SVG::AttributeNames::y || name == SVG::AttributeNames::patternUnits || name == SVG::AttributeNames::patternContentUnits || name == SVG::AttributeNames::patternTransform || name == SVG::AttributeNames::viewBox || name == SVG::AttributeNames::preserveAspectRatio || name == SVG::AttributeNames::href || name == AttributeNames::xlink_href) {
        m_paint_style = nullptr;
    }

    if (name.equals_ignoring_ascii_case(SVG::AttributeNames::viewBox)) {
        m_view_box = try_parse_view_box(value.value_or(String {}));
        m_view_box_for_bindings->set_nulled(!m_view_box.has_value());
        if (m_view_box.has_value()) {
            m_view_box_for_bindings->set_base_val(Gfx::DoubleRect { m_view_box->min_x, m_view_box->min_y, m_view_box->width, m_view_box->height });
            m_view_box_for_bindings->set_anim_val(Gfx::DoubleRect { m_view_box->min_x, m_view_box->min_y, m_view_box->width, m_view_box->height });
        }
    }
}

PatternUnits SVGPatternElement::pattern_units() const
{
    return PatternUnits::ObjectBoundingBox;
}

PatternUnits SVGPatternElement::pattern_content_units() const
{
    return PatternUnits::UserSpaceOnUse;
}

RefPtr<Gfx::Bitmap> SVGPatternElement::create_pattern_bitmap(SVGPaintContext const&) const
{
    // TODO: Implement pattern rendering
    // 1. Create a bitmap with the appropriate size
    // 2. Render child SVG elements into the bitmap
    // 3. Apply transformations, viewBox, etc.
    return nullptr;
}

Optional<Painting::PaintStyle> SVGPatternElement::to_gfx_paint_style(SVGPaintContext const& context) const
{
    // TODO: Implement pattern rendering
    (void)context;
    return {};
}

GC::Ref<SVGAnimatedLength> SVGPatternElement::x()
{
    return SVGAnimatedLength::create(realm(),
        SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, 0),
        SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, 0));
}

GC::Ref<SVGAnimatedLength> SVGPatternElement::y()
{
    return SVGAnimatedLength::create(realm(),
        SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, 0),
        SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, 0));
}

GC::Ref<SVGAnimatedLength> SVGPatternElement::width()
{
    return SVGAnimatedLength::create(realm(),
        SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, 0),
        SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, 0));
}

GC::Ref<SVGAnimatedLength> SVGPatternElement::height()
{
    return SVGAnimatedLength::create(realm(),
        SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, 0),
        SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, 0));
}

GC::Ptr<SVGPatternElement const> SVGPatternElement::linked_pattern(HashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (seen_patterns.contains(this))
        return {};

    seen_patterns.set(this);

    // TODO: Implement href/xlink:href resolution
    return {};
}

}
