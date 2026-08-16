/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Length.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Layout {

Box::Box(DOM::Document& document, GC::Ptr<DOM::Node> node, CSS::LayoutStyle style, RustFFI::NodeKind kind)
    : NodeWithStyle(document, node, move(style))
{
    set_node_kind(kind);
    if (RustFFI::layout_node_kind_is_replaced_box(kind))
        set_flag(RustFFI::NodeFlag::IsReplacedElement, true);
}

Box::~Box()
{
}

static ImageProvider const& image_provider_for_element(DOM::Element const& element)
{
    if (auto const* image = as_if<HTML::HTMLImageElement>(element))
        return *image;
    if (auto const* input = as_if<HTML::HTMLInputElement>(element))
        return *input;
    if (auto const* object = as_if<HTML::HTMLObjectElement>(element))
        return *object;

    VERIFY_NOT_REACHED();
}

ImageProvider const& Box::image_provider() const
{
    VERIFY(kind() == RustFFI::NodeKind::ImageBox);
    if (m_owned_image_provider)
        return *m_owned_image_provider;

    auto const* element = dom_node();
    VERIFY(element);
    return image_provider_for_element(as<DOM::Element>(*element));
}

void Box::set_owned_image_provider(NonnullOwnPtr<ImageProvider> image_provider)
{
    VERIFY(kind() == RustFFI::NodeKind::ImageBox);
    m_owned_image_provider = move(image_provider);
}

bool Box::is_partial_relayout_boundary() const
{
    // An absolutely or fixed positioned descendant whose containing block is outside this
    // box's subtree is laid out by a formatting context outside it, which makes subtree
    // isolation impossible for any kind of boundary.
    if (abspos_descendant_escapes())
        return false;

    // Committing a subtree splices the new paint subtree into the old paintable's paint-tree
    // position, so a boundary must still have one - either on the box or, for a box the tree
    // builder just rebuilt, held by the DOM node until the next commit replaces it there.
    if (!paintable_box() && !(dom_node() && dom_node()->unsafe_paintable()))
        return false;

    // An in-flow SVG viewport's used size is determined solely by its own attributes and outer
    // context, never by its children, so its size and position from the previous layout can be
    // reused - provided a commit has actually saved them; its content lays out in the viewport's
    // own user units, so a nested <svg> is just as reproducible from its own root as the
    // outermost one. An absolutely positioned SVG root's placement is not frozen, so it must
    // qualify through the saved-inputs replay path below instead.
    if (is_svg_svg_box() && !is_absolutely_positioned())
        return has_saved_committed_geometry();

    if (!is_absolutely_positioned())
        return false;
    if (is_anonymous())
        return false;
    if (dom_node() == document().document_element())
        return false;
    if (!has_saved_abspos_layout_inputs())
        return false;

    // Only a full layout pass resolves anchor() functions in the inset properties to plain
    // values; a replay from saved inputs cannot.
    if (insets_use_anchor_functions())
        return false;

    // NOTE: Content-dependent sizing (shrink-to-fit, intrinsic constraints, aspect-ratio) does
    //       not disqualify a boundary: replay re-solves the boundary's own size, and a resized
    //       boundary triggers ancestor scrollable overflow recomputation after commit.

    auto formatting_context_type = formatting_context_type_created_by_box(*this);
    if (!formatting_context_type.has_value())
        return false;
    switch (*formatting_context_type) {
    case RustFFI::FfiFormattingContextType::Block:
    case RustFFI::FfiFormattingContextType::Flex:
    case RustFFI::FfiFormattingContextType::Grid:
    case RustFFI::FfiFormattingContextType::Svg:
        return true;
    default:
        return false;
    }
}

static CSS::SizeWithAspectRatio default_preferred_size_for_text_control(HTML::HTMLInputElement const& input_element, Box const& box)
{
    // https://html.spec.whatwg.org/multipage/rendering.html#the-input-element-as-a-text-entry-widget
    // An input element whose type attribute is in one of the above states is an element with default preferred size,
    // and user agents are expected to apply the 'field-sizing' CSS property to the element. User agents are expected
    // to determine the inline size of its intrinsic size by the following steps:
    // [...] If the element has a size attribute, and parsing that attribute's value using the rules for parsing
    // non-negative integers doesn't generate an error, return the value obtained from applying the converting a
    // character width to pixels algorithm to the value of the attribute. Otherwise, return the value obtained from
    // applying the converting a character width to pixels algorithm to the number 20.
    //
    // FIXME: Implement the specified "converting a character width to pixels" algorithm. The size attribute should
    //        also only affect the text entry types (text, search, tel, url, email, password). The other types using
    //        this box are domain-specific widgets that should ignore it.
    auto inline_size = CSS::Length(input_element.size(), CSS::LengthUnit::Ch).to_px(box);

    // FIXME: HTML does not yet detail the primitive appearance of text inputs. Use one line for the default preferred
    //        block size, matching the native appearance described by HTML and the behavior of other engines.
    auto block_size = box.line_height();

    if (box.writing_mode() != CSS::WritingMode::HorizontalTb)
        return { block_size, inline_size, {} };

    return { inline_size, block_size, {} };
}

static CSS::SizeWithAspectRatio natural_size_of_object_content_document_svg_root(Box const& box)
{
    if (!is<HTML::HTMLObjectElement>(box.dom_node()))
        return {};

    if (auto const* content_document = as<HTML::HTMLObjectElement>(*box.dom_node()).content_document_without_origin_check()) {
        if (auto const* root = content_document->document_element();
            root && root->is_svg_svg_element()) {

            auto const& svg_root = as<SVG::SVGSVGElement>(*root);
            auto resolution_context = svg_root.layout_node()
                ? CSS::Length::ResolutionContext::for_layout_node(*svg_root.layout_node())
                : CSS::Length::ResolutionContext::for_document(*content_document);
            auto metrics = SVG::SVGSVGElement::negotiate_natural_metrics(svg_root, resolution_context);
            return { metrics.width, metrics.height, metrics.aspect_ratio };
        }
    }
    return {};
}

CSS::SizeWithAspectRatio Box::natural_size() const
{
    switch (kind()) {
    case RustFFI::NodeKind::ImageBox: {
        auto const& image_provider = this->image_provider();
        if (image_provider.is_image_available()) {
            return {
                .width = image_provider.intrinsic_width(),
                .height = image_provider.intrinsic_height(),
                .aspect_ratio = image_provider.intrinsic_aspect_ratio()
            };
        }
        return { 0, 0, {} };
    }
    case RustFFI::NodeKind::VideoBox: {
        auto natural_size = as<HTML::HTMLVideoElement>(*dom_node()).natural_element_size();
        if (!natural_size.has_value())
            return {};
        if (natural_size->is_empty())
            return { 0, 0, {} };
        return { natural_size->width(), natural_size->height(), natural_size->width() / natural_size->height() };
    }
    case RustFFI::NodeKind::SVGSVGBox: {
        auto metrics = SVG::SVGSVGElement::negotiate_natural_metrics(as<SVG::SVGSVGElement>(*dom_node()), CSS::Length::ResolutionContext::for_layout_node(*this));
        return { metrics.width, metrics.height, metrics.aspect_ratio };
    }
    case RustFFI::NodeKind::NavigableContainerViewport:
        return natural_size_of_object_content_document_svg_root(*this);
    default:
        return {};
    }
}

CSS::SizeWithAspectRatio Box::compute_auto_content_box_size() const
{
    switch (kind()) {
    case RustFFI::NodeKind::CheckBox:
        return { 13, 13, {} };
    case RustFFI::NodeKind::RadioButton:
        return { 12, 12, {} };
    case RustFFI::NodeKind::CanvasBox: {
        auto width = as<HTML::HTMLCanvasElement>(*dom_node()).width();
        auto height = as<HTML::HTMLCanvasElement>(*dom_node()).height();
        if (width == 0 || height == 0)
            return { width, height, {} };
        return { width, height, CSSPixelFraction(width, height) };
    }
    case RustFFI::NodeKind::RangeInputBox: {
        // AD-HOC: A slider has no in-flow content to size itself from, so provide a default content-box size for when
        //         its `width` or `height` is `auto`.
        // NB: We only support horizontal sliders, so the default size is not adjusted for the writing mode.
        auto width = CSS::Length(20, CSS::LengthUnit::Ch).to_px(*this);
        auto height = CSS::Length(16, CSS::LengthUnit::Px).to_px(*this);
        return { width, height, {} };
    }
    case RustFFI::NodeKind::TextAreaBox: {
        auto const& text_area_element = as<HTML::HTMLTextAreaElement>(*dom_node());
        auto inline_size = CSS::Length(text_area_element.cols(), CSS::LengthUnit::Ch).to_px(*this);
        auto block_size = CSS::Length(text_area_element.rows(), CSS::LengthUnit::Lh).to_px(*this);

        if (writing_mode() != CSS::WritingMode::HorizontalTb)
            return { block_size, inline_size, {} };

        return { inline_size, block_size, {} };
    }
    case RustFFI::NodeKind::TextInputBox:
        return default_preferred_size_for_text_control(as<HTML::HTMLInputElement>(*dom_node()), *this);
    default:
        return natural_size();
    }
}

CSS::SizeWithAspectRatio Box::auto_content_box_size() const
{
    // https://drafts.csswg.org/css-contain-2/#containment-size
    // Replaced elements must be treated as having a natural width and height of 0 and no natural aspect
    // ratio.
    if (has_size_containment())
        return { 0, 0, {} };

    return compute_auto_content_box_size();
}

RustFFI::FfiReplacedContentFacts Box::build_replaced_content_facts_for_arena() const
{
    RustFFI::FfiReplacedContentFacts facts {};
    auto auto_content_size = auto_content_box_size();
    facts.has_auto_content_width = auto_content_size.has_width();
    facts.auto_content_width = auto_content_size.width.value_or(0).raw_value();
    facts.has_auto_content_height = auto_content_size.has_height();
    facts.auto_content_height = auto_content_size.height.value_or(0).raw_value();
    if (auto_content_size.aspect_ratio.has_value()) {
        facts.auto_content_aspect_ratio_numerator = auto_content_size.aspect_ratio->numerator().raw_value();
        facts.auto_content_aspect_ratio_denominator = auto_content_size.aspect_ratio->denominator().raw_value();
    }
    if (appearance() == CSS::Appearance::None) {
        if (auto const* input = as_if<HTML::HTMLInputElement>(dom_node())) {
            switch (input->type_state()) {
            case HTML::HTMLInputElement::TypeAttributeState::Text:
            case HTML::HTMLInputElement::TypeAttributeState::Search:
            case HTML::HTMLInputElement::TypeAttributeState::URL:
            case HTML::HTMLInputElement::TypeAttributeState::Telephone:
            case HTML::HTMLInputElement::TypeAttributeState::Email:
            case HTML::HTMLInputElement::TypeAttributeState::Password:
            case HTML::HTMLInputElement::TypeAttributeState::Number: {
                auto default_preferred_size = default_preferred_size_for_text_control(*input, *this);
                facts.has_default_preferred_width = default_preferred_size.has_width();
                facts.default_preferred_width = default_preferred_size.width.value_or(0).raw_value();
                facts.has_default_preferred_height = default_preferred_size.has_height();
                facts.default_preferred_height = default_preferred_size.height.value_or(0).raw_value();
                break;
            }
            default:
                break;
            }
        }
    }
    return facts;
}

void Box::did_set_content_size()
{
    if (kind() != RustFFI::NodeKind::NavigableContainerViewport)
        return;

    if (auto content_navigable = as<HTML::NavigableContainer>(*dom_node()).content_navigable()) {
        auto content_size = paintable_box()->content_size();
        as<HTML::LocalNavigable>(*content_navigable).set_viewport_size(content_size);
        document().page().client().page_did_update_child_frame_viewport(content_navigable->id(), paintable_box()->absolute_rect());
    }
}

RefPtr<Painting::Paintable> Box::paintable_box()
{
    if (auto paintable = Node::paintable())
        return static_cast<Painting::Paintable&>(*paintable);
    return nullptr;
}

RefPtr<Painting::Paintable const> Box::paintable_box() const
{
    if (auto paintable = Node::paintable())
        return static_cast<Painting::Paintable const&>(*paintable);
    return nullptr;
}

Optional<CSSPixelFraction> Box::preferred_aspect_ratio() const
{
    auto computed_aspect_ratio = aspect_ratio();

    // https://www.w3.org/TR/css-contain-2/#containment-size
    if (!has_size_containment() && computed_aspect_ratio.use_natural_aspect_ratio_if_available) {
        if (auto auto_size = auto_content_box_size(); auto_size.has_aspect_ratio())
            return auto_size.aspect_ratio;
    }

    if (!computed_aspect_ratio.preferred_ratio.has_value())
        return {};

    auto ratio = computed_aspect_ratio.preferred_ratio.value();
    if (ratio.is_degenerate())
        return {};

    auto fraction = CSSPixelFraction(ratio.numerator(), ratio.denominator());
    // ratio.is_degenerate() operates on doubles while CSSPixelFraction uses CSSPixels, so we need to check again here.
    if (fraction == 0)
        return {};

    return fraction;
}

}
