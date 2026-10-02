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

class LayoutTreeBuildBridge {
public:
    RustFFI::FfiLayoutTreeBuildOutcome build(DOM::Node&);

    static void detach_top_layer_element_layout_subtree(DOM::Element&);

private:
    // The node a tree builder callback names by its identity.
    static DOM::Node& node_for_style_node(void* builder_pointer, u32 style_node);

    RustFFI::FfiDomTreeBuilderCallbacks make_ffi_dom_tree_builder_callbacks();

    GC::Ptr<DOM::Document> m_document;
};

static void update_style_if_needed_for_layout_tree_bypass_path(DOM::Element&);

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
        RustFFI::layout_arena_set_owned_image_natural_size(m_layout_node->arena_handle(), Node::slot_id(m_layout_node.ptr()), published);
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

// A DOM node paired with the identity its layout rows carry, so Rust can find them itself.
static RustFFI::FfiIdentifiedDomNode identified_dom_node(DOM::Node const* node)
{
    return {
        .node = const_cast<DOM::Node*>(node),
        .style_node = Node::style_node_of(node).value(),
    };
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

// The node an identity names. The document is not in the style computer's node table, because a document holding a
// reference back to itself there would keep itself alive; every other identity resolves through the table.
static DOM::Node& dom_node_for_style_node(DOM::Document& document, u32 style_node)
{
    CSS::StyleNodeID identity { style_node };
    if (identity == document.style_node_id())
        return document;
    auto node = document.style_computer().node_for_style_node(identity);
    VERIFY(node);
    return *node;
}

DOM::Node& LayoutTreeBuildBridge::node_for_style_node(void* builder_pointer, u32 style_node)
{
    VERIFY(builder_pointer);
    return dom_node_for_style_node(*static_cast<LayoutTreeBuildBridge*>(builder_pointer)->m_document, style_node);
}

void LayoutTreeBuildBridge::detach_top_layer_element_layout_subtree(DOM::Element& element)
{
    RustFFI::rust_detach_top_layer_element_layout_subtree(element.document().layout_node_arena().handle(), element.style_node_id().value());
}

RustFFI::FfiDomTreeBuilderCallbacks LayoutTreeBuildBridge::make_ffi_dom_tree_builder_callbacks()
{
    return {
        .builder = this,
        .top_layer_element_count = [](void* builder_pointer) {
            VERIFY(builder_pointer);
            return static_cast<LayoutTreeBuildBridge*>(builder_pointer)->m_document->top_layer_elements().size(); },
        .copy_top_layer_elements = [](void* builder_pointer, RustFFI::FfiIdentifiedDomNode* output, size_t count) {
            VERIFY(builder_pointer);
            VERIFY(output || count == 0);
            auto const& elements = static_cast<LayoutTreeBuildBridge*>(builder_pointer)->m_document->top_layer_elements();
            VERIFY(count == elements.size());
            size_t index = 0;
            for (auto const& element : elements)
                output[index++] = identified_dom_node(element.ptr()); },
        .restyle_bypass_path_element = [](void* builder_pointer, u32 style_node) { update_style_if_needed_for_layout_tree_bypass_path(as<DOM::Element>(node_for_style_node(builder_pointer, style_node))); },
        .attach_style_resources = [](void* builder_pointer, Compositing::RustFFI::NodeSlotId slot, bool owns_content_replacement_image) {
            VERIFY(builder_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            auto* layout_node = static_cast<Node*>(RustFFI::layout_arena_node_shell_if_live(builder.m_document->layout_node_arena().handle(), slot));
            VERIFY(layout_node);
            if (owns_content_replacement_image)
                attach_content_replacement_image(as<Box>(*layout_node));
            as<NodeWithStyle>(*layout_node).attach_style_resources(); },
        .nested_list_marker_style = [](void* builder_pointer, u32 element_style_node) -> u64 {
            auto& element = as<DOM::Element>(node_for_style_node(builder_pointer, element_style_node));
            auto& style_computer = element.document().style_computer();
            // NB: Republishing the element's own ::marker style can retire its animation record while the pseudo-element
            //     still refers to it. Give the nested marker a record of its own, made from a copy of the existing style.
            auto marker_style = [&] {
                if (auto style = element.computed_style(CSS::PseudoElement::Marker))
                    return CSS::ComputedValues::Builder { *style }.build();
                // The style engine derives the ::marker style for this read alone; C++ computes it where the engine
                // leaves the read to it.
                if (auto style = style_computer.engine_transient_pseudo_element_style(element, CSS::PseudoElement::Marker))
                    return style.release_nonnull();
                return style_computer.materialize_style_record({ element, CSS::PseudoElement::Marker });
            }();
            return style_computer.intern_anonymous_layout_style(*marker_style).value(); },
        .attach_generated_image = [](void* builder_pointer, Compositing::RustFFI::NodeSlotId slot, u32 element_style_node, RustFFI::FfiPseudoElement ffi_pseudo, RustFFI::FfiGeneratedImage generated_image) {
            auto& element = as<DOM::Element>(node_for_style_node(builder_pointer, element_style_node));
            auto& arena = element.document().layout_node_arena();
            auto& image_box = as<Box>(*static_cast<Node*>(RustFFI::layout_arena_node_shell_if_live(arena.handle(), slot)));
            auto image = [&] -> NonnullRefPtr<CSS::AbstractImageStyleValue const> {
                if (generated_image.kind == RustFFI::FfiGeneratedImageKind::ListStyleImage) {
                    auto& marker = as<NodeWithStyle>(*static_cast<Node*>(RustFFI::layout_arena_node_shell_if_live(arena.handle(), generated_image.marker)));
                    return *marker.list_style_image();
                }
                auto const* payloads = DOM::AbstractElement { element, css_pseudo_element(ffi_pseudo) }.style_record_payloads();
                VERIFY(payloads);
                auto content = CSS::style_group_from_payloads<CSS::ComputedValues::ContentValues>(payloads)->computed_content_value();
                return content->as_content().content().values()[generated_image.content_index]->as_abstract_image();
            }();
            attach_owned_image_provider(image_box, const_cast<CSS::AbstractImageStyleValue&>(*image));
            image_box.attach_style_resources(); },
    };
}

// A bypass path (top-layer iteration, slot projection, SVG mask/clip-path or pattern reference)
// may reach an element whose `computed_values` is null. Route through `update_style_for_element`,
// which seeds the style computer's ancestor filter so descendant-combinator selectors continue to
// match during the lazy re-cascade.
static void update_style_if_needed_for_layout_tree_bypass_path(DOM::Element& element)
{
    if (!element.has_style())
        element.document().update_style_for_element({ element });
}

RustFFI::FfiLayoutTreeBuildOutcome LayoutTreeBuildBridge::build(DOM::Node& dom_node)
{
    m_document = &dom_node.document();
    auto callbacks = make_ffi_dom_tree_builder_callbacks();
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
        RustFFI::layout_arena_set_viewport_scroll_offset(arena, navigable->viewport_scroll_offset());
    return RustFFI::rust_build_layout_tree(&callbacks, arena, &dom_node, document.style_node_id().value(), document_style_record.value());
}

RustFFI::FfiLayoutTreeBuildOutcome build_layout_tree(DOM::Node& dom_node)
{
    LayoutTreeBuildBridge bridge;
    return bridge.build(dom_node);
}

void detach_top_layer_element_layout_subtree(DOM::Element& element)
{
    LayoutTreeBuildBridge::detach_top_layer_element_layout_subtree(element);
}

// https://drafts.csswg.org/css-tables-3/#fixup-algorithm

}
