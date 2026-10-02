/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Aziz B. Yesilyurt <abyesilyurt@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Utf16String.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>

namespace Web::Layout {

class GeneratedContentImageProvider final
    : public ImageProvider {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~GeneratedContentImageProvider() override = default;

    virtual void layout_node_was_detached() const override
    {
        m_image_client = nullptr;
        m_layout_node = nullptr;
    }

    static NonnullOwnPtr<GeneratedContentImageProvider> create(DOM::Document& document, NonnullRefPtr<CSS::AbstractImageStyleValue> image)
    {
        return adopt_own(*new GeneratedContentImageProvider(document, move(image)));
    }

    void set_layout_node(Layout::Node& layout_node)
    {
        m_layout_node = layout_node;
        publish_natural_size();
    }

    virtual GC::Ptr<HTML::DecodedImageData> decoded_image_data() const override
    {
        if (!m_image_client)
            return nullptr;
        return m_image_client->decoded_image_data();
    }

    virtual Optional<CSSPixels> intrinsic_width() const override { return natural_size().width; }
    virtual Optional<CSSPixels> intrinsic_height() const override { return natural_size().height; }
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override { return natural_size().aspect_ratio; }
    virtual Layout::Node const* image_provider_layout_node() const override { return m_layout_node.ptr(); }

private:
    class ImageClient final : public CSS::ImageStyleValue::Client {
    public:
        AK_ALLOC_WITH_KMALLOC;

        ImageClient(GeneratedContentImageProvider const& owner, DOM::Document& document, CSS::ImageStyleValue const& image)
            : CSS::ImageStyleValue::Client(document, image)
            , m_owner(owner)
        {
        }

        virtual ~ImageClient() override
        {
            image_style_value_finalize();
        }

        virtual void image_style_value_did_update(CSS::ImageStyleValue&) override
        {
            if (!m_owner.m_layout_node)
                return;
            m_owner.publish_natural_size();
            m_owner.image_provider_contents_changed();
            m_owner.m_layout_node->set_needs_layout_update(DOM::SetNeedsLayoutReason::GeneratedContentImageFinishedLoading);
        }

    private:
        GeneratedContentImageProvider const& m_owner;
    };

    GeneratedContentImageProvider(DOM::Document& document, NonnullRefPtr<CSS::AbstractImageStyleValue> image)
        : m_image(move(image))
    {
        if (auto const* image = m_image->selected_image_style_value())
            m_image_client = make<ImageClient>(*this, document, *image);
    }

    // The box's replaced content facts are derived from the natural size of the image it shows, which the provider
    // publishes to the box's row as it is handed over and as its image loads: zero while the image is not available.
    void publish_natural_size() const
    {
        auto natural_size = is_image_available() ? this->natural_size() : CSS::SizeWithAspectRatio { 0, 0, {} };
        RustFFI::FfiNaturalSize published {};
        published.width = natural_size.width;
        published.height = natural_size.height;
        if (natural_size.aspect_ratio.has_value()) {
            published.has_aspect_ratio = true;
            published.aspect_ratio_numerator = natural_size.aspect_ratio->numerator();
            published.aspect_ratio_denominator = natural_size.aspect_ratio->denominator();
        }
        RustFFI::render_state_set_owned_image_natural_size(m_layout_node->document_host(), Node::slot_id(m_layout_node.ptr()), published);
    }

    CSS::SizeWithAspectRatio natural_size() const
    {
        auto decoded_image_data = this->decoded_image_data();
        if (!decoded_image_data)
            return {};
        return m_image->natural_size(*decoded_image_data);
    }

    mutable WeakPtr<Layout::Node> m_layout_node;
    NonnullRefPtr<CSS::AbstractImageStyleValue> m_image;
    mutable OwnPtr<ImageClient> m_image_client;
};

// Gives an image box the provider of the image it shows, which the box owns.
static void attach_owned_image_provider(Box& image_box, CSS::AbstractImageStyleValue& image)
{
    auto& document = image_box.document();
    image.load_any_resources(document);
    auto image_provider = GeneratedContentImageProvider::create(document, image);
    auto& image_provider_ref = *image_provider;
    image_box.set_owned_image_provider(move(image_provider));
    image_provider_ref.set_layout_node(image_box);
}

static RefPtr<CSS::AbstractImageStyleValue const> content_replacement_image(CSS::StyleValue const& content)
{
    if (!content.is_content())
        return nullptr;
    auto const& items = content.as_content().content().values();
    if (items.size() != 1 || !items.first()->is_abstract_image())
        return nullptr;
    return &items.first()->as_abstract_image();
}

// The image box the build stamped for an element or pseudo-element whose content is a single image owns the provider
// of that image.
static void attach_content_replacement_image(Box& image_box)
{
    auto replacement_image = content_replacement_image(image_box.style_group<CSS::ComputedValues::ContentValues>().computed_content_value());
    VERIFY(replacement_image);
    attach_owned_image_provider(image_box, const_cast<CSS::AbstractImageStyleValue&>(*replacement_image));
}

static CSS::PseudoElement css_pseudo_element(RustFFI::FfiPseudoElement pseudo_element)
{
    switch (pseudo_element) {
    case RustFFI::FfiPseudoElement::Before:
        return CSS::PseudoElement::Before;
    case RustFFI::FfiPseudoElement::After:
        return CSS::PseudoElement::After;
    case RustFFI::FfiPseudoElement::Marker:
        return CSS::PseudoElement::Marker;
    case RustFFI::FfiPseudoElement::Backdrop:
        return CSS::PseudoElement::Backdrop;
    case RustFFI::FfiPseudoElement::Other:
    case RustFFI::FfiPseudoElement::None:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

bool attach_owed_style_resources(DOM::Document& document, Compositing::RustFFI::NodeSlotId slot, bool owns_content_replacement_image)
{
    auto* layout_node = static_cast<Node*>(RustFFI::layout_arena_node_shell_if_live(document.layout_node_arena().handle(), slot));
    VERIFY(layout_node);
    // A box that replaces its element's contents with a single image owns the provider that answers for it. The image
    // is named by the same style record the box was stamped from, and it loads before the resources the rest of that
    // style asks for.
    bool image_was_available = false;
    if (owns_content_replacement_image) {
        auto& image_box = as<Box>(*layout_node);
        attach_content_replacement_image(image_box);
        image_was_available = image_box.image_provider().is_image_available();
        if (image_was_available)
            image_box.set_needs_layout_update(DOM::SetNeedsLayoutReason::GeneratedContentImageFinishedLoading);
    }
    as<NodeWithStyle>(*layout_node).attach_style_resources();
    return image_was_available;
}

bool attach_owed_generated_image(DOM::Document& document, Compositing::RustFFI::NodeSlotId slot, u32 element_style_node, RustFFI::FfiPseudoElement ffi_pseudo, RustFFI::FfiGeneratedImage generated_image)
{
    // A generator that went away since the build made its pseudo-element's boxes names no image any more, and the boxes
    // go away with it.
    auto element = document.style_computer().element_for_style_node(CSS::StyleNodeID { element_style_node });
    if (!element)
        return false;
    auto& arena = document.layout_node_arena();
    auto& image_box = as<Box>(*static_cast<Node*>(RustFFI::layout_arena_node_shell_if_live(arena.handle(), slot)));
    auto image = [&] -> NonnullRefPtr<CSS::AbstractImageStyleValue const> {
        if (generated_image.kind == RustFFI::FfiGeneratedImageKind::ListStyleImage) {
            auto& marker = as<NodeWithStyle>(*static_cast<Node*>(RustFFI::layout_arena_node_shell_if_live(arena.handle(), generated_image.marker)));
            return *marker.list_style_image();
        }
        auto const* payloads = DOM::AbstractElement { *element, css_pseudo_element(ffi_pseudo) }.style_record_payloads();
        VERIFY(payloads);
        auto content = CSS::style_group_from_payloads<CSS::ComputedValues::ContentValues>(payloads)->computed_content_value();
        return content->as_content().content().values()[generated_image.content_index]->as_abstract_image();
    }();
    attach_owned_image_provider(image_box, const_cast<CSS::AbstractImageStyleValue&>(*image));
    bool image_was_available = image_box.image_provider().is_image_available();
    if (image_was_available)
        image_box.set_needs_layout_update(DOM::SetNeedsLayoutReason::GeneratedContentImageFinishedLoading);
    image_box.attach_style_resources();
    return image_was_available;
}

RustFFI::FfiLayoutTreeBuildOutcome build_layout_tree(DOM::Node& dom_node)
{
    auto& document = dom_node.document();
    auto* arena = document.layout_node_arena().handle();
    // The viewport's style is the document's, which the style computer makes rather than publishes, so a build that may
    // build the viewport is handed it before it starts.
    CSS::StyleRecordID document_style_record;
    if (RustFFI::layout_arena_tree_build_may_create_viewport(arena, document.style_node_id().value())) {
        auto& style_computer = document.style_computer();
        document_style_record = style_computer.intern_anonymous_layout_style(*style_computer.create_document_style());
    }
    // The viewport's row holds what the navigable has scrolled the viewport to, which the navigable publishes as it
    // scrolls. A new document has not heard from it yet.
    if (auto navigable = document.navigable())
        RustFFI::render_state_set_viewport_scroll_offset(document.layout_node_arena().host(), navigable->viewport_scroll_offset());
    return RustFFI::rust_build_layout_tree(arena, document.style_node_id().value(), document_style_record.value());
}

void detach_top_layer_element_layout_subtree(DOM::Element& element)
{
    RustFFI::render_state_detach_top_layer_element(element.document().layout_node_arena().host(), element.style_node_id().value());
}

// https://drafts.csswg.org/css-tables-3/#fixup-algorithm

}
