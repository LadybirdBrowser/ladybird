/*
 * Copyright (c) 2025, Ramon van Sprundel <ramonvansprundel@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LibWeb/SVG/SVGAnimatedEnumeration.h"
#include "LibWeb/SVG/SVGAnimatedTransformList.h"
#include <LibGfx/Bitmap.h>
#include <LibGfx/Painter.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGPatternElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
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
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGPatternElement);
    Base::initialize(realm);
    m_view_box_for_bindings = realm.create<SVGAnimatedRect>(realm);
}

void SVGPatternElement::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin<SupportsXLinkHref::Yes>::visit_edges(visitor);
    visitor.visit(m_view_box_for_bindings);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_width);
    visitor.visit(m_height);
    visitor.visit(m_pattern_transform);
}

SVGPatternElement::SVGPatternElement(DOM::Document& document,
    DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGPatternElement::attribute_changed(
    FlyString const& name, Optional<String> const& old_value,
    Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::width || name == SVG::AttributeNames::height || name == SVG::AttributeNames::x || name == SVG::AttributeNames::y || name == SVG::AttributeNames::patternUnits || name == SVG::AttributeNames::patternContentUnits || name == SVG::AttributeNames::patternTransform || name == SVG::AttributeNames::viewBox || name == SVG::AttributeNames::preserveAspectRatio || name == SVG::AttributeNames::href || name == AttributeNames::xlink_href) {
        m_paint_style = nullptr;
    }

    if (name == SVG::AttributeNames::x) {
        auto x_value = 0.0f;
        if (value.has_value()) {
            auto maybe_x = value->bytes_as_string_view().to_number<float>();
            if (maybe_x.has_value())
                x_value = maybe_x.value();
        }

        if (!m_x) {
            m_x = SVGAnimatedLength::create(
                realm(),
                SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, x_value),
                SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                    x_value));
        } else {
            (void)m_x->base_val()->set_value(x_value);
            (void)m_x->anim_val()->set_value(x_value);
        }
    } else if (name == SVG::AttributeNames::y) {
        auto y_value = 0.0f;
        if (value.has_value()) {
            auto maybe_y = value->bytes_as_string_view().to_number<float>();
            if (maybe_y.has_value())
                y_value = maybe_y.value();
        }

        if (!m_y) {
            m_y = SVGAnimatedLength::create(
                realm(),
                SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, y_value),
                SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                    y_value));
        } else {
            (void)m_y->base_val()->set_value(y_value);
            (void)m_y->anim_val()->set_value(y_value);
        }
    } else if (name == SVG::AttributeNames::width) {
        auto width_value = 0.0f;
        if (value.has_value()) {
            auto maybe_width = value->bytes_as_string_view().to_number<float>();
            if (maybe_width.has_value())
                width_value = maybe_width.value();
        }

        if (!m_width) {
            m_width = SVGAnimatedLength::create(
                realm(),
                SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                    width_value),
                SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                    width_value));
        } else {
            (void)m_width->base_val()->set_value(width_value);
            (void)m_width->anim_val()->set_value(width_value);
        }
    } else if (name == SVG::AttributeNames::height) {
        auto height_value = 0.0f;
        if (value.has_value()) {
            auto maybe_height = value->bytes_as_string_view().to_number<float>();
            if (maybe_height.has_value())
                height_value = maybe_height.value();
        }

        if (!m_height) {
            m_height = SVGAnimatedLength::create(
                realm(),
                SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                    height_value),
                SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                    height_value));
        } else {
            (void)m_height->base_val()->set_value(height_value);
            (void)m_height->anim_val()->set_value(height_value);
        }
    } else if (name.equals_ignoring_ascii_case(SVG::AttributeNames::viewBox)) {
        m_view_box = try_parse_view_box(value.value_or(String {}));
        m_view_box_for_bindings->set_nulled(!m_view_box.has_value());
        if (m_view_box.has_value()) {
            m_view_box_for_bindings->set_base_val(
                Gfx::DoubleRect { m_view_box->min_x, m_view_box->min_y,
                    m_view_box->width, m_view_box->height });
            m_view_box_for_bindings->set_anim_val(
                Gfx::DoubleRect { m_view_box->min_x, m_view_box->min_y,
                    m_view_box->width, m_view_box->height });
        }
    } else if (name == SVG::AttributeNames::patternTransform) {
        if (!m_pattern_transform) {
            m_pattern_transform = SVGAnimatedTransformList::create(
                realm(), SVGTransformList::create(realm()),
                SVGTransformList::create(realm()));
        }
    }
}

GC::Ref<SVGAnimatedEnumeration> SVGPatternElement::pattern_units() const
{
    if (!m_pattern_units) {
        m_pattern_units = SVGAnimatedEnumeration::create(
            realm(), to_underlying(PatternUnits::ObjectBoundingBox));
    }
    return *m_pattern_units;
}

GC::Ref<SVGAnimatedEnumeration> SVGPatternElement::pattern_content_units() const
{
    if (!m_pattern_content_units) {
        m_pattern_content_units = SVGAnimatedEnumeration::create(
            realm(), to_underlying(PatternUnits::UserSpaceOnUse));
    }
    return *m_pattern_content_units;
}

GC::Ref<SVGAnimatedTransformList> SVGPatternElement::pattern_transform() const
{
    if (!m_pattern_transform) {
        m_pattern_transform = SVGAnimatedTransformList::create(realm(),
            SVGTransformList::create(realm()),
            SVGTransformList::create(realm()));
    }
    return *m_pattern_transform;
}

RefPtr<Gfx::Bitmap>
SVGPatternElement::create_pattern_bitmap(SVGPaintContext const&) const
{
    // TODO: Implement pattern rendering
    // 1. Create a bitmap with the appropriate size
    // 2. Render child SVG elements into the bitmap
    // 3. Apply transformations, viewBox, etc.
    return nullptr;
}

Optional<Painting::PaintStyle>
SVGPatternElement::to_gfx_paint_style(SVGPaintContext const& context) const
{
    // TODO: Implement pattern rendering
    (void)context;
    return {};
}

GC::Ref<SVGAnimatedLength> SVGPatternElement::x() const
{
    if (!m_x) {
        auto x_value = 0.0f;
        if (auto x_attr = attribute(AttributeNames::x); x_attr.has_value()) {
            auto maybe_x = x_attr->bytes_as_string_view().to_number<float>();
            if (maybe_x.has_value())
                x_value = maybe_x.value();
        }

        m_x = SVGAnimatedLength::create(
            realm(),
            SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, x_value),
            SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, x_value));
    }
    return *m_x;
}

GC::Ref<SVGAnimatedLength> SVGPatternElement::y() const
{
    if (!m_y) {
        auto y_value = 0.0f;
        if (auto y_attr = attribute(AttributeNames::y); y_attr.has_value()) {
            auto maybe_y = y_attr->bytes_as_string_view().to_number<float>();
            if (maybe_y.has_value())
                y_value = maybe_y.value();
        }

        m_y = SVGAnimatedLength::create(
            realm(),
            SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, y_value),
            SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, y_value));
    }
    return *m_y;
}

GC::Ref<SVGAnimatedLength> SVGPatternElement::width() const
{
    if (!m_width) {
        auto width_value = 0.0f;
        if (auto width_attr = attribute(AttributeNames::width);
            width_attr.has_value()) {
            auto maybe_width = width_attr->bytes_as_string_view().to_number<float>();
            if (maybe_width.has_value())
                width_value = maybe_width.value();
        }

        m_width = SVGAnimatedLength::create(
            realm(),
            SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                width_value),
            SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                width_value));
    }
    return *m_width;
}

GC::Ref<SVGAnimatedLength> SVGPatternElement::height() const
{
    if (!m_height) {
        auto height_value = 0.0f;
        if (auto height_attr = attribute(AttributeNames::height);
            height_attr.has_value()) {
            auto maybe_height = height_attr->bytes_as_string_view().to_number<float>();
            if (maybe_height.has_value())
                height_value = maybe_height.value();
        }

        m_height = SVGAnimatedLength::create(
            realm(),
            SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                height_value),
            SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER,
                height_value));
    }
    return *m_height;
}

GC::Ptr<SVGPatternElement const> SVGPatternElement::linked_pattern(
    HashTable<SVGPatternElement const*>& seen_patterns) const
{
    if (seen_patterns.contains(this))
        return {};

    seen_patterns.set(this);

    // TODO: Implement href/xlink:href resolution
    return {};
}

}
