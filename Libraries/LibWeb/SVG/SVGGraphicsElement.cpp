/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <LibWeb/Bindings/SVGGraphicsElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/FragmentIdentifier.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

SVGGraphicsElement::SVGGraphicsElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGGraphicsElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_required_extensions);
    visitor.visit(m_system_language);
    visitor.visit(m_transform);
}

void SVGGraphicsElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // https://svgwg.org/svg2-draft/types.html#SynchronizingReflectedValues
    // Whenever a reflected content attribute's base value changes, then the reflecting object must be synchronized,
    // immediately after the value changed.
    if (namespace_.has_value())
        return;
    if (name == AttributeNames::requiredExtensions || name == AttributeNames::systemLanguage) {
        if (name == AttributeNames::requiredExtensions && m_required_extensions)
            m_required_extensions->synchronize();
        else if (name == AttributeNames::systemLanguage && m_system_language)
            m_system_language->synchronize();

        // NB: Whether the conditional processing attributes evaluate to true decides whether the element has a box.
        CSS::record_element_box_kind(*this);
        set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::StyleChange);
        return;
    }
    if (name == AttributeNames::transform && m_transform)
        m_transform->synchronize();
}

// https://svgwg.org/svg2-draft/struct.html#ConditionalProcessingRequiredExtensionsAttribute
static bool required_extensions_evaluate_to_true(Utf16View value)
{
    // The value is a list of URL references which identify the required extensions, with the individual values
    // separated by white space. Determines whether all of the named extensions are supported by the user agent. If all
    // of the given extensions are supported, then the attribute evaluates to true; otherwise, the current element and
    // its children are skipped and thus will not be rendered.
    // If a null string or empty string value is given to attribute ‘requiredExtensions’, the attribute evaluates to
    // "false".
    auto extensions = SVGStringList::parse(value, SVGStringList::Separator::Whitespace);
    if (extensions.is_empty())
        return false;

    // INTEROP: Other browsers treat the HTML and MathML namespaces as supported extensions, since their content can be
    //          embedded with foreignObject.
    return all_of(extensions, [](auto const& extension) {
        return extension == Namespace::HTML || extension == Namespace::MathML;
    });
}

// https://svgwg.org/svg2-draft/struct.html#ConditionalProcessingSystemLanguageAttribute
static bool system_language_evaluates_to_true(Utf16View value)
{
    // If a null string or empty string value is given to attribute ‘systemLanguage’, the attribute evaluates to
    // "false".
    if (value.is_empty())
        return false;

    // Evaluates to "true" if one of the language tags indicated by user preferences is a case-insensitive match of one
    // of the language tags given in the value of this parameter, or if one of the language tags indicated by user
    // preferences is a case-insensitive prefix of one of the language tags given in the value of this parameter such
    // that the first tag character following the prefix is "-".
    auto language_tags = SVGStringList::parse(value, SVGStringList::Separator::Comma);
    for (auto const& preferred_language : ResourceLoader::the().preferred_languages()) {
        auto preferred_tag = Utf16String::from_utf8(preferred_language);
        for (auto const& language_tag : language_tags) {
            if (language_tag.equals_ignoring_ascii_case(preferred_tag))
                return true;
            if (language_tag.length_in_code_units() > preferred_tag.length_in_code_units()
                && language_tag.code_unit_at(preferred_tag.length_in_code_units()) == '-'
                && language_tag.utf16_view().substring_view(0, preferred_tag.length_in_code_units()).equals_ignoring_ascii_case(preferred_tag))
                return true;
        }
    }

    // Evaluates to "false" otherwise.
    return false;
}

bool SVGGraphicsElement::conditional_processing_attributes_evaluate_to_true() const
{
    // If the attribute is not present, then it implicitly evaluates to "true".
    if (auto required_extensions = attribute(AttributeNames::requiredExtensions); required_extensions.has_value() && !required_extensions_evaluate_to_true(*required_extensions))
        return false;
    if (auto system_language = attribute(AttributeNames::systemLanguage); system_language.has_value() && !system_language_evaluates_to_true(*system_language))
        return false;
    return true;
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGTests__requiredExtensions
GC::Ref<SVGStringList> SVGGraphicsElement::required_extensions_for_bindings()
{
    // The requiredExtensions IDL attribute reflects the ‘requiredExtensions’ content attribute.
    // NB: The attribute's value is a set of space-separated tokens.
    if (!m_required_extensions)
        m_required_extensions = SVGStringList::create(*this, AttributeNames::requiredExtensions, SVGStringList::Separator::Whitespace);
    return *m_required_extensions;
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGTests__systemLanguage
GC::Ref<SVGStringList> SVGGraphicsElement::system_language_for_bindings()
{
    // The systemLanguage IDL attribute reflects the ‘systemLanguage’ content attribute.
    // NB: The attribute's value is a set of comma-separated tokens.
    if (!m_system_language)
        m_system_language = SVGStringList::create(*this, AttributeNames::systemLanguage, SVGStringList::Separator::Comma);
    return *m_system_language;
}

GC::Ptr<DOM::Element> SVGGraphicsElement::resolve_url_to_element(CSS::URL const& url) const
{
    // FIXME: Complete and use the entire URL, not just the fragment.
    if (auto fragment_offset = Utf16View { url.url() }.find_code_unit_offset('#'); fragment_offset.has_value()) {
        auto fragment_string = url.url().substring_view(fragment_offset.value() + 1);
        return resolve_fragment_identifier_to_element(decode_fragment_identifier(fragment_string));
    }

    return {};
}

GC::Ptr<DOM::Element> SVGGraphicsElement::resolve_fragment_identifier_to_element(Utf16String const& fragment) const
{
    if (auto element = document().get_element_by_id(fragment))
        return element;

    auto containing_shadow = containing_shadow_root();
    if (containing_shadow) {
        if (auto element = containing_shadow->get_element_by_id(fragment))
            return element;
    }

    return {};
}

Gfx::AffineTransform transform_from_transform_list(ReadonlySpan<Transform> transform_list)
{
    Gfx::AffineTransform affine_transform;
    for (auto& transform : transform_list) {
        transform.operation.visit(
            [&](Transform::Translate const& translate) {
                affine_transform.multiply(Gfx::AffineTransform {}.translate({ translate.x, translate.y }));
            },
            [&](Transform::Scale const& scale) {
                affine_transform.multiply(Gfx::AffineTransform {}.scale({ scale.x, scale.y }));
            },
            [&](Transform::Rotate const& rotate) {
                Gfx::AffineTransform translate_transform;
                affine_transform.multiply(
                    Gfx::AffineTransform {}
                        .translate({ rotate.x, rotate.y })
                        .rotate_radians(AK::to_radians(rotate.a))
                        .translate({ -rotate.x, -rotate.y }));
            },
            [&](Transform::SkewX const& skew_x) {
                affine_transform.multiply(Gfx::AffineTransform {}.skew_radians(AK::to_radians(skew_x.a), 0));
            },
            [&](Transform::SkewY const& skew_y) {
                affine_transform.multiply(Gfx::AffineTransform {}.skew_radians(0, AK::to_radians(skew_y.a)));
            },
            [&](Transform::Matrix const& matrix) {
                affine_transform.multiply(Gfx::AffineTransform {
                    matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f });
            });
    }
    return affine_transform;
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGGraphicsElement__getBBox
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMRect>> SVGGraphicsElement::get_b_box(Bindings::SVGBoundingBoxOptions const&)
{
    // The getBBox method is used to compute the bounding box of the current element.  When the getBBox(options) method
    // is called, the bounding box algorithm is invoked for the current element, with fill, stroke, markers and clipped
    // members of the options dictionary argument used to control which parts of the element are included in the
    // bounding box, using the element's user coordinate system as the coordinate system to return the bounding box in.
    // A newly created DOMRect object that defines the computed bounding box is returned.
    // If the element's geometric attributes are missing or have invalid values (e.g. negative width or height), such
    // that the element is not rendered, then a DOMRect with x, y, width and height all set to 0 is returned.

    // FIXME: It should be possible to compute this without layout updates. The bounding box is within the
    //        SVG coordinate space (before any viewbox or other transformations), so it should be possible to
    //        calculate this from SVG geometry without a full layout tree (at least for simple cases).
    //        See: https://svgwg.org/svg2-draft/coords.html#BoundingBoxes
    Layout::ForcedReadScope read { document() };
    document().update_layout_if_needed_for_node(*this, DOM::UpdateLayoutReason::SVGGraphicsElementGetBBox);
    auto const* self_layout_node = layout_node(read);
    if (!self_layout_node)
        return Geometry::DOMRect::create();
    auto owner_svg_element = this->owner_svg_element();

    // The outermost svg element has no ancestor svg element to measure against; per the bounding box
    // algorithm its bounding box is the union of its children's bounding boxes in its own user
    // coordinate system, with each child's own transforms kept.
    if (!owner_svg_element) {
        if (!is<SVGSVGElement>(*this))
            return Geometry::DOMRect::create();
        if (!Painting::has_committed_box(*self_layout_node))
            return Geometry::DOMRect::create();
        Gfx::FloatRect united_rect;
        for (auto const* child = self_layout_node->first_child_ptr(); child; child = child->next_sibling_ptr()) {
            switch (child->kind()) {
            case Layout::RustFFI::NodeKind::SVGMaskBox:
            case Layout::RustFFI::NodeKind::SVGClipBox:
            case Layout::RustFFI::NodeKind::SVGPatternBox:
                continue;
            default:
                break;
            }
            if (!Painting::has_committed_box(*child))
                continue;
            auto child_rect = Painting::svg_element_transform(*child).map(Painting::absolute_rect(*child).to_type<float>());
            united_rect.unite(child_rect);
        }
        if (united_rect.is_empty())
            return Geometry::DOMRect::create();
        return Geometry::DOMRect::create(united_rect);
    }

    auto const* owner_layout_node = owner_svg_element->layout_node(read);
    if (!owner_layout_node || !Painting::has_committed_box(*owner_layout_node) || !Painting::has_committed_box(*self_layout_node)) {
        // Throw only for non-rendered *graphics* elements where geometry isn't computable
        // (e.g. elements inside <marker>, <pattern>, etc.).
        if (is<SVGSVGElement>(*this))
            return Geometry::DOMRect::create();
        return WebIDL::InvalidStateError::create(
            HTML::relevant_realm(*this),
            "Element is not rendered and geometry is not computable"_utf16);
    }

    // https://svgwg.org/svg2-draft/coords.html#BoundingBoxes
    // Let fill-shape be the equivalent path of element if it is a shape, or a shape that includes each of the glyph
    // cells corresponding to the text within the elements otherwise.
    // NB: Layout sizes a text content element's box to exactly that: The glyph cells of all the text within it.
    if (self_layout_node->kind() == Layout::RustFFI::NodeKind::SVGTextBox || self_layout_node->kind() == Layout::RustFFI::NodeKind::SVGTextPathBox) {
        auto rect = Painting::absolute_rect(*self_layout_node).to_type<float>();
        if (rect.is_empty())
            return Geometry::DOMRect::create();
        return Geometry::DOMRect::create(rect);
    }

    // A path-like element's bounding box covers its geometry alone; the committed content rect is
    // inflated by the visible stroke width, so take the unstroked path bounds directly.
    if (auto committed_path_bounding_box = Painting::committed_svg_path_bounding_box(*self_layout_node); committed_path_bounding_box.has_value())
        return Geometry::DOMRect::create(*committed_path_bounding_box);

    auto rect = Painting::absolute_rect(*self_layout_node).to_type<float>();
    // An element with a non-positive geometry dimension is not rendered and
    // therefore contributes an empty bounding box, regardless of its
    // positioning rectangle's origin.
    if (rect.width() <= 0 || rect.height() <= 0)
        return Geometry::DOMRect::create();
    return Geometry::DOMRect::create(rect);
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGGraphicsElement__transform
GC::Ref<SVGAnimatedTransformList> SVGGraphicsElement::transform()
{
    // The transform IDL attribute reflects the computed value of the transform property and its corresponding
    // ‘transform’ presentation attribute.
    // FIXME: Reflect the computed value of the transform property rather than only the presentation attribute.
    if (!m_transform)
        m_transform = SVGAnimatedTransformList::create(*this, AttributeNames::transform);
    return *m_transform;
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGGraphicsElement__getScreenCTM
GC::Ptr<Geometry::DOMMatrix> SVGGraphicsElement::get_screen_ctm()
{
    // 1. If the current element is not in the document, then return null.
    if (!is_connected())
        return {};

    Layout::ForcedReadScope read { document() };
    document().update_layout_if_needed_for_node(*this, DOM::UpdateLayoutReason::SVGGraphicsElementGetScreenCTM);

    // 2. If the current element is a non-rendered element, and the UA is not able to resolve the style of the element,
    //    then return null.
    //
    // NB: We currently require committed box data connected to the document's visual-context tree to compute this matrix.
    //     This also excludes geometry in resource-only subtrees such as masks, clip paths, and patterns.
    auto const* layout_node = this->layout_node(read);
    if (!layout_node || !Painting::has_committed_box(*layout_node) || !document().has_committed_viewport_box())
        return {};

    // 3. Let ctm be a matrix that transforms the coordinate space of the current element (including its transform
    //    property) to the coordinate space of the document's viewport.
    //
    // NB: An SVG viewport's current user coordinate system is the space produced by its viewBox transform, which is the
    //     coordinate system recorded for its descendants. Other graphics elements use their own accumulated context so
    //     their transform property is included exactly once.
    auto visual_context_tree = document().visual_context_tree();
    if (!Painting::has_accumulated_visual_context(*layout_node))
        return {};

    auto visual_context = Painting::svg_viewport_transform(*layout_node).has_value()
        ? Painting::accumulated_visual_context_for_descendants(*layout_node)
        : Painting::accumulated_visual_context(*layout_node);
    auto ctm = visual_context_tree.accumulated_matrix(
        visual_context.spatial,
        document().scroll_state_snapshot(),
        Compositing::AccumulatedVisualContextTree::IncludeVisualViewportTransform::No);

    // NB: Accumulated visual-context matrices operate in device-pixel space. Conjugate the matrix by the device scale
    //     to expose CSS-pixel coordinates through the DOM API, then project any 3D transform onto the SVG plane.
    auto device_scale = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto css_to_device_scale = Gfx::scale_matrix(Vector3 { device_scale, device_scale, device_scale });
    auto device_to_css_scale = Gfx::scale_matrix(Vector3 { 1 / device_scale, 1 / device_scale, 1 / device_scale });
    ctm = device_to_css_scale * ctm * css_to_device_scale;
    ctm = Gfx::extract_2d_affine_transform(Gfx::flattened(ctm)).to_matrix();

    // 4. Return a newly created, detached DOMMatrix object that represents the same matrix as ctm.
    return Geometry::DOMMatrix::create(ctm);
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGGraphicsElement__getCTM
GC::Ptr<Geometry::DOMMatrix> SVGGraphicsElement::get_ctm()
{
    // 1. If the current element is not in the document, then return null.
    if (!is_connected())
        return {};

    Layout::ForcedReadScope read { document() };
    document().update_layout_if_needed_for_node(*this, DOM::UpdateLayoutReason::SVGGraphicsElementGetCTM);

    // 2. If the current element is a non-rendered element, and the UA is not able to resolve the style of the element,
    //    then return null.
    // NB: We require committed box data for the current element and each of its ancestors up to the element ctm
    //     transforms to, which also excludes geometry in resource-only subtrees such as masks, clip paths, and patterns.
    auto const* layout_node = this->layout_node(read);
    if (!layout_node || !Painting::has_committed_box(*layout_node))
        return {};

    // 3. Let ctm be a matrix determined based on what the current element is:
    // -> the current element is the outermost svg element
    //    ctm is a matrix that transforms the coordinate space of the ‘svg’ (including its transform property) to the
    //    coordinate space of the document's viewport. The matrix includes the transforms produced by the ‘viewBox’ and
    //    ‘preserveAspectRatio’ attributes, the transform property, and any transform due to currentScale and
    //    currentTranslate properties on the SVGSVGElement.
    // AD-HOC: Like Blink and WebKit, transform to the coordinate space of the svg element's SVG viewport instead, which
    //         leaves only the transform produced by the viewBox and preserveAspectRatio attributes. Gecko returns null.
    // -> any other element
    //    ctm is a matrix that transforms the coordinate space of the current element (including its transform property)
    //    to the coordinate space of its closest ancestor viewport-establishing element (also including its transform
    //    property).
    // NB: Like Blink and WebKit, the coordinate space of an svg element includes the transforms produced by its position
    //     and its viewBox attribute.
    auto outermost_svg_element = this->outermost_svg_element();
    GC::Ptr<DOM::Node const> target = this;
    if (outermost_svg_element.ptr() != this)
        target = viewport_element();
    if (!target)
        return {};

    Gfx::AffineTransform ctm;
    for (auto const* node = layout_node;; node = node->parent()) {
        if (!node || !Painting::has_committed_box(*node))
            return {};

        Gfx::AffineTransform local_transform;
        auto viewport_transform = Painting::svg_viewport_transform(*node);
        if (node->dom_node() == outermost_svg_element.ptr()) {
            local_transform = viewport_transform.value_or({});
        } else {
            local_transform = Painting::svg_element_transform(*node);
            if (viewport_transform.has_value()) {
                local_transform.translate(Painting::absolute_rect(*node).location().to_type<float>());
                local_transform.multiply(*viewport_transform);
            }
        }
        ctm = local_transform.multiply(ctm);

        if (node->dom_node() == target.ptr())
            break;
    }

    // 4. Return a newly created, detached DOMMatrix object that represents the same matrix as ctm.
    return Geometry::DOMMatrix::create(ctm.to_matrix());
}

}
