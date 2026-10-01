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
    ~LayoutTreeBuildBridge();

    RustFFI::FfiLayoutTreeBuildOutcome build(DOM::Node&);

    static void detach_top_layer_element_layout_subtree(DOM::Element&);

private:
    struct PseudoElementFrameStorage;
    static TraversalDecision clear_stale_layout_node(DOM::Node&, u32 cleared_subtree_root);

    RustFFI::FfiDomTreeBuilderCallbacks make_ffi_dom_tree_builder_callbacks();
    RustFFI::FfiPseudoTreeBuilderCallbacks make_ffi_pseudo_tree_builder_callbacks();
    RustFFI::FfiTreeBuilderCallbacks make_ffi_tree_builder_callbacks();

    static Box& create_list_item_marker(Box& list_box, CSS::LayoutStyle marker_style);
    static RustFFI::FfiFirstLetterNodes create_first_letter_nodes(DOM::Element&, RustFFI::FfiFirstLetterTarget);

    void pin_style_record_for_build(CSS::StyleRecordID);

    GC::Ptr<DOM::Document> m_document;
    OwnPtr<PseudoElementFrameStorage> m_pseudo_element_frames;
    // Every style record a principal box is built from, held for the whole build. Letting go of a record the build has
    // stopped looking at buys nothing before the build ends, and holding them all in one place is what lets a visit
    // carry no C++ frame of its own.
    Vector<CSS::StyleRecordID> m_pinned_style_records;
};

void LayoutTreeBuilderAccess::clear_synthetic_pseudo_element_layout_nodes(DOM::Element& element)
{
    element.clear_synthetic_pseudo_element_layout_nodes({});
}

void LayoutTreeBuilderAccess::detach_layout_node(DOM::Node& node)
{
    if (auto* layout_node = node.unsafe_layout_node()) {
        layout_node->prepare_for_detach_from_layout_tree();
        RustFFI::layout_arena_unbind_row(layout_node->arena_handle(), Node::slot_id(layout_node));
    }
}

void LayoutTreeBuilderAccess::set_synthetic_pseudo_element_node(DOM::Element& element, CSS::PseudoElement pseudo_element, Layout::NodeWithStyle* layout_node)
{
    element.set_synthetic_pseudo_element_node({}, pseudo_element, layout_node);
}

static void update_style_if_needed_for_layout_tree_bypass_path(DOM::Element&);
static Compositing::RustFFI::NodeSlotId create_layout_node_for_text(DOM::Text&);

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

static Box& create_content_image_box(DOM::Document& document, GC::Ptr<DOM::Element> element, CSS::LayoutStyle style, CSS::AbstractImageStyleValue& image)
{
    image.load_any_resources(document);
    auto image_provider = GeneratedContentImageProvider::create(document, image);
    auto& image_provider_ref = *image_provider;
    auto& image_box = allocate_layout_node<Box>(document, element, style, RustFFI::NodeKind::ImageBox);
    image_box.set_owned_image_provider(move(image_provider));
    image_provider_ref.set_layout_node(image_box);
    return image_box;
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

struct FirstLetterTextSlices {
    TextNode* first_letter_slice;
    TextNode* remainder_slice;
};

static FirstLetterTextSlices create_first_letter_text_slices(DOM::Document& document, TextNode& text_node, size_t letter_end)
{
    auto const full_length = text_node.text().length_in_code_units();

    // The first-letter and remainder boxes render slices of the same DOM text node; generated text
    // (from a content property) has no DOM node and gets plain generated slices of its text instead.
    if (auto* dom_text = text_node.dom_text()) {
        auto& mutable_dom_text = const_cast<DOM::Text&>(*dom_text);
        auto& remainder_slice = allocate_layout_node<TextNode>(document, mutable_dom_text, Node::AttachToDOMNode::Yes);
        auto& first_letter_slice = allocate_layout_node<TextNode>(document, mutable_dom_text, Node::AttachToDOMNode::No);
        return { &first_letter_slice, &remainder_slice };
    }

    auto text = text_node.text();
    return {
        &allocate_layout_node<GeneratedTextNode>(document, Utf16String::from_utf16(text.utf16_view().substring_view(0, letter_end))),
        &allocate_layout_node<GeneratedTextNode>(document, Utf16String::from_utf16(text.utf16_view().substring_view(letter_end, full_length - letter_end))),
    };
}

RustFFI::FfiFirstLetterNodes LayoutTreeBuildBridge::create_first_letter_nodes(DOM::Element& element, RustFFI::FfiFirstLetterTarget target)
{
    VERIFY(target.found);
    auto& text_node = as<TextNode>(*static_cast<Node*>(target.text_node));
    auto& document = element.document();

    auto [first_letter_slice, remainder_slice] = create_first_letter_text_slices(document, text_node, target.letter_end);

    auto const* first_letter_box_values = element.style_group<CSS::ComputedValues::BoxValues>(CSS::PseudoElement::FirstLetter);
    VERIFY(first_letter_box_values);
    auto display = first_letter_box_values->display_value();
    auto first_letter_wrapper = DOM::Element::create_layout_node_for_display_type(document, display, CSS::LayoutStyle { element.style_record_identity(CSS::PseudoElement::FirstLetter) }, nullptr);
    if (first_letter_wrapper) {
        first_letter_wrapper->attach_style_resources();
        first_letter_wrapper->set_generated_for(CSS::PseudoElement::FirstLetter, element);
        LayoutTreeBuilderAccess::set_synthetic_pseudo_element_node(element, CSS::PseudoElement::FirstLetter, first_letter_wrapper);
    }
    return {
        .wrapper = Node::slot_id(first_letter_wrapper),
        .first_letter_slice = Node::slot_id(first_letter_slice),
        .remainder_slice = Node::slot_id(remainder_slice),
    };
}

Box& LayoutTreeBuildBridge::create_list_item_marker(Box& list_box, CSS::LayoutStyle marker_style)
{
    auto& list_item_marker = allocate_layout_node<Box>(list_box.document(), nullptr, move(marker_style), RustFFI::NodeKind::ListItemMarkerBox);
    list_item_marker.set_list_marker_is_inside(list_box.list_style_position() == CSS::ListStylePosition::Inside);
    return list_item_marker;
}

// https://drafts.csswg.org/css-lists-3/#text-markers
// "<counter-style>: Specifies the element's marker string as the value of the list-item counter
// represented using the specified <counter-style>. Specifically, the marker string is the result of
// generating a counter representation of the list-item counter value using the specified
// <counter-style>, prefixed by the prefix of the <counter-style>, and followed by the suffix of the
// <counter-style>. If the specified <counter-style> does not exist, decimal is assumed.
// <string>: The element's marker string is the specified <string>."
static CSS::ContentData resolve_normal_marker_content(DOM::AbstractElement& element_reference, Box const& list_box, Box const& marker)
{
    CSS::ContentData content;
    content.type = CSS::ContentData::Type::List;

    if (auto const* list_style_image = marker.list_style_image()) {
        content.data.append(NonnullRefPtr { const_cast<CSS::AbstractImageStyleValue&>(*list_style_image) });
        return content;
    }

    if (CSS::marker_text_depends_on_list_item_counter_value(list_box.list_style_type()))
        element_reference.element().document().did_render_list_item_counter_value(element_reference.element());

    auto counter_value = CSS::counter_value_for_use(element_reference, CSS::list_item_counter_name());

    auto generate_from_counter_style = [&](RefPtr<CSS::CounterStyle const> const& counter_style) -> Utf16String {
        auto counter_representation = CSS::generate_a_counter_representation(counter_style, element_reference.style_scope(), counter_value);
        if (counter_style) {
            content.counter_style_dependencies.append(counter_style);
            return Utf16String::formatted("{}{}{}", counter_style->prefix(), counter_representation, counter_style->suffix());
        }
        return Utf16String::formatted("{}. ", counter_representation);
    };

    auto marker_string = list_box.list_style_type().visit(
        [](Empty const&) -> Utf16String { VERIFY_NOT_REACHED(); },
        [&](RefPtr<CSS::CounterStyle const> const& counter_style) -> Utf16String {
            return generate_from_counter_style(counter_style);
        },
        [](Utf16String const& string) -> Utf16String {
            return string;
        },
        [&](CSS::UnresolvedCounterStyleName const&) -> Utf16String {
            return generate_from_counter_style(nullptr);
        },
        [&](CSS::ListStyleSymbols const& symbols) -> Utf16String {
            return generate_from_counter_style(symbols.counter_style);
        });
    content.data.append(move(marker_string));
    return content;
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

static RustFFI::FfiComputedContentType ffi_computed_content_type(CSS::StyleValue const& content)
{
    if (content.is_keyword())
        return content.to_keyword() == CSS::Keyword::None ? RustFFI::FfiComputedContentType::None : RustFFI::FfiComputedContentType::Normal;
    VERIFY(content.is_content());
    return RustFFI::FfiComputedContentType::List;
}

struct PseudoElementFrame {
    AK_ALLOC_WITH_KMALLOC;

    CSS::StyleRecordID style_record_identity;
    GC::Ptr<CSS::StyleComputer> style_record_owner;
    CSS::Display display;
    RefPtr<CSS::AbstractImageStyleValue const> replacement_image;
    Box* originating_list_box { nullptr };
    NodeWithStyle* layout_node { nullptr };
    CSS::ContentData resolved_content;
    Layout::Node* content_item { nullptr };
};

struct LayoutTreeBuildBridge::PseudoElementFrameStorage {
    AK_ALLOC_WITH_KMALLOC;

    Vector<NonnullOwnPtr<PseudoElementFrame>> frames;
    size_t active_frame_count { 0 };
};

RustFFI::FfiPseudoTreeBuilderCallbacks LayoutTreeBuildBridge::make_ffi_pseudo_tree_builder_callbacks()
{
    return {
        .builder = this,
        .push_frame = [](void* builder_pointer) -> void* {
            VERIFY(builder_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            if (!builder.m_pseudo_element_frames)
                builder.m_pseudo_element_frames = make<PseudoElementFrameStorage>();
            auto& storage = *builder.m_pseudo_element_frames;
            if (storage.active_frame_count == storage.frames.size())
                storage.frames.append(make<PseudoElementFrame>());
            return storage.frames[storage.active_frame_count++].ptr(); },
        .pop_frame = [](void* builder_pointer, void* frame_pointer) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            auto& storage = *static_cast<LayoutTreeBuildBridge*>(builder_pointer)->m_pseudo_element_frames;
            VERIFY(storage.active_frame_count > 0);
            VERIFY(storage.frames[storage.active_frame_count - 1].ptr() == frame_pointer);
            auto& frame = *storage.frames[storage.active_frame_count - 1];
            if (!!frame.style_record_identity) {
                VERIFY(frame.style_record_owner);
                frame.style_record_owner->unpin_style_record(frame.style_record_identity);
                frame.style_record_identity = {};
                frame.style_record_owner = nullptr;
            }
            --storage.active_frame_count; },
        .initialize = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo) -> RustFFI::FfiPseudoElementFacts {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto pseudo_element = css_pseudo_element(ffi_pseudo);
            VERIFY(!frame.style_record_identity);
            VERIFY(!frame.style_record_owner);
            if (auto existing_pseudo = element.get_synthetic_pseudo_element(pseudo_element); existing_pseudo.has_value() && existing_pseudo->layout_node())
                existing_pseudo->set_layout_node(nullptr);
            frame.style_record_identity = element.style_record_identity(pseudo_element);
            if (!!frame.style_record_identity) {
                frame.style_record_owner = &element.document().style_computer();
                frame.style_record_owner->pin_style_record(frame.style_record_identity);
            }
            frame.replacement_image = nullptr;
            frame.originating_list_box = nullptr;
            frame.layout_node = nullptr;
            frame.content_item = nullptr;
            auto const* pseudo_payloads = element.style_record_payloads(pseudo_element);
            if (!pseudo_payloads) {
                return {
                    .has_style = false,
                    .pseudo_element = ffi_pseudo,
                    .content_type = RustFFI::FfiComputedContentType::None,
                    .display_is_none = false,
                    .display_is_contents = false,
                    .display_is_list_item = false,
                    .has_content_replacement = false,
                    .originating_list_box = Node::slot_id(nullptr),
                    .normal_marker_has_content = false,
                    .marker_position_is_inside = false,
                };
            }
            frame.display = CSS::style_group_from_payloads<CSS::ComputedValues::BoxValues>(pseudo_payloads)->display_value();
            auto const computed_content = CSS::style_group_from_payloads<CSS::ComputedValues::ContentValues>(pseudo_payloads)->computed_content_value();
            auto const computed_content_type = ffi_computed_content_type(computed_content);
            frame.replacement_image = content_replacement_image(computed_content);
            if (pseudo_element == CSS::PseudoElement::Marker)
                frame.originating_list_box = element.unsafe_layout_node()->is_list_item_box() ? static_cast<Box*>(element.unsafe_layout_node()) : nullptr;
            auto const normal_marker_has_content = frame.originating_list_box
                && (!frame.originating_list_box->list_style_type().has<Empty>() || frame.originating_list_box->list_style_image());
            return {
                .has_style = true,
                .pseudo_element = ffi_pseudo,
                .content_type = computed_content_type,
                .display_is_none = frame.display.is_none(),
                .display_is_contents = frame.display.is_contents(),
                .display_is_list_item = frame.display.is_list_item(),
                .has_content_replacement = frame.replacement_image != nullptr,
                .originating_list_box = Node::slot_id(frame.originating_list_box),
                .normal_marker_has_content = normal_marker_has_content,
                .marker_position_is_inside = frame.originating_list_box
                    && frame.originating_list_box->list_style_position() == CSS::ListStylePosition::Inside,
            }; },
        .create_layout_node = [](void* builder_pointer, void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement, RustFFI::FfiPseudoElementDecision decision) -> Compositing::RustFFI::NodeSlotId {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            VERIFY(frame.style_record_identity);
            CSS::LayoutStyle style { frame.style_record_identity };
            auto& document = element.document();
            switch (decision) {
            case RustFFI::FfiPseudoElementDecision::None:
                VERIFY_NOT_REACHED();
            case RustFFI::FfiPseudoElementDecision::ContentReplacement:
                VERIFY(frame.replacement_image);
                frame.layout_node = &create_content_image_box(document, nullptr, style, const_cast<CSS::AbstractImageStyleValue&>(*frame.replacement_image));
                break;
            case RustFFI::FfiPseudoElementDecision::Contents:
                frame.layout_node = &allocate_layout_node<NodeWithStyle>(document, nullptr, style, RustFFI::NodeKind::InlineNode);
                frame.layout_node->set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
                break;
            case RustFFI::FfiPseudoElementDecision::Box:
                if (frame.originating_list_box) {
                    frame.layout_node = &create_list_item_marker(*frame.originating_list_box, style);
                    break;
                }
                frame.layout_node = DOM::Element::create_layout_node_for_display_type(document, frame.display, style, nullptr);
                break;
            }
            return Node::slot_id(frame.layout_node); },

        .create_nested_list_marker = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement originating_pseudo) -> Compositing::RustFFI::NodeSlotId {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            VERIFY(frame.layout_node);
            auto marker_style = [&] {
                // NB: Republishing the element's own ::marker style can retire its animation record while the
                //     pseudo-element still refers to it. Give the nested marker a copy of the existing style.
                if (auto style = element.computed_style(CSS::PseudoElement::Marker))
                    return CSS::ComputedValues::Builder { *style }.build();
                return element.document().style_computer().materialize_style_record({ element, CSS::PseudoElement::Marker });
            }();
            auto& list_item_marker = create_list_item_marker(as<Box>(*frame.layout_node), move(marker_style));
            list_item_marker.attach_style_resources();
            // NB: The marker of a list-item ::before or ::after belongs to that pseudo-element, not to the element's own
            //     ::marker, so it is generated for the originating pseudo-element and never becomes the ::marker's box.
            list_item_marker.set_generated_for(css_pseudo_element(originating_pseudo), element);
            return Node::slot_id(&list_item_marker); },
        .create_nested_list_marker_content = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement originating_pseudo, void* marker_pointer) -> Compositing::RustFFI::NodeSlotId {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            VERIFY(marker_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto& list_box = as<Box>(*frame.layout_node);
            auto& list_item_marker = as<Box>(*static_cast<Node*>(marker_pointer));
            DOM::AbstractElement element_reference { element, css_pseudo_element(originating_pseudo) };
            auto content = resolve_normal_marker_content(element_reference, list_box, list_item_marker);
            auto& content_node = [&]() -> Node& {
                if (auto const* text = content.data.first().get_pointer<Utf16String>()) {
                    auto& text_node = allocate_layout_node<GeneratedTextNode>(list_box.document(), *text);
                    text_node.set_generated_for(css_pseudo_element(originating_pseudo), element);
                    return text_node;
                }
                auto& image = *content.data.first().get<NonnullRefPtr<CSS::AbstractImageStyleValue>>();
                auto& image_box = create_content_image_box(list_box.document(), nullptr, list_item_marker.copy_computed_values(), image);
                image_box.set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
                image_box.attach_style_resources();
                image_box.set_generated_for(css_pseudo_element(originating_pseudo), element);
                return image_box;
            }();
            list_item_marker.set_content(content);
            return Node::slot_id(&content_node); },
        .configure_layout_node = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo) {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto pseudo_element = css_pseudo_element(ffi_pseudo);
            VERIFY(frame.layout_node);
            frame.layout_node->set_generated_for(pseudo_element, element);
            LayoutTreeBuilderAccess::set_synthetic_pseudo_element_node(element, pseudo_element, frame.layout_node); },
        .resolve_content = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo, u32 initial_quote_nesting_level) -> RustFFI::FfiResolvedPseudoContentFacts {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            DOM::AbstractElement element_reference { *static_cast<DOM::Element*>(element_pointer), css_pseudo_element(ffi_pseudo) };
            auto const* payloads = element_reference.style_record_payloads();
            VERIFY(payloads);
            auto const* content_values = CSS::style_group_from_payloads<CSS::ComputedValues::ContentValues>(payloads);
            if (auto* marker = frame.layout_node->is_list_item_marker_box() ? static_cast<Box*>(frame.layout_node) : nullptr;
                marker && content_values->content_is_normal()) {
                VERIFY(frame.originating_list_box);
                frame.resolved_content = resolve_normal_marker_content(element_reference, *frame.originating_list_box, *marker);
                frame.layout_node->set_content(frame.resolved_content);
                return {
                    .final_quote_nesting_level = initial_quote_nesting_level,
                    .content_is_list = true,
                    .content_item_count = frame.resolved_content.data.size(),
                };
            }
            auto [content, final_quote_nesting_level] = CSS::ComputedValues::resolved_content(*content_values,
                *CSS::style_group_from_payloads<CSS::ComputedValues::InheritedListValues>(payloads),
                element_reference, initial_quote_nesting_level, CSS::NotifyListItemCounterRendered::Yes);
            frame.resolved_content = move(content);
            frame.layout_node->set_content(frame.resolved_content);
            return {
                .final_quote_nesting_level = final_quote_nesting_level,
                .content_is_list = frame.resolved_content.type == CSS::ContentData::Type::List,
                .content_item_count = frame.resolved_content.data.size(),
            }; },
        .create_content_item = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo, size_t index) -> Compositing::RustFFI::NodeSlotId {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            VERIFY(frame.layout_node);
            VERIFY(index < frame.resolved_content.data.size());
            auto& item = frame.resolved_content.data[index];
            if (auto const* string = item.get_pointer<Utf16String>()) {
                // An empty generated text node carries the inline fragment of an ordinary inline pseudo-element.
                // Other pseudo-element boxes exist independently of their contents, so avoid giving them a
                // zero-length child that would force layout to measure an otherwise empty box.
                if (string->is_empty() && !(frame.display.is_inline_outside() && frame.display.is_flow_inside()))
                    return Node::slot_id(nullptr);
                frame.content_item = &allocate_layout_node<GeneratedTextNode>(element.document(), *string);
            } else {
                auto& image = *item.get<NonnullRefPtr<CSS::AbstractImageStyleValue>>();
                auto& image_box = create_content_image_box(element.document(), nullptr, frame.layout_node->copy_computed_values(), image);
                // https://drafts.csswg.org/css-content-3/#content-property
                // For <image>, this is an inline anonymous replaced element.
                image_box.set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
                image_box.attach_style_resources();
                frame.content_item = &image_box;
            }
            frame.content_item->set_generated_for(css_pseudo_element(ffi_pseudo), element);
            return Node::slot_id(frame.content_item); },
    };
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

static bool is_svg_resource_box(Node const& layout_node)
{
    return layout_node.is_svg_pattern_box() || layout_node.is_svg_mask_box() || layout_node.is_svg_clip_box();
}

TraversalDecision LayoutTreeBuildBridge::clear_stale_layout_node(DOM::Node& node, u32 cleared_subtree_root)
{
    node.set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
    node.set_child_needs_layout_tree_update(false);

    // NB: Called during layout tree construction.
    auto* layout_node = node.unsafe_layout_node();
    // SVGPatternBox, SVGMaskBox, and SVGClipBox are created on behalf of a referencing
    // element and attached to that element's layout subtree. Skip them so they survive
    // cleanup of their DOM ancestor, unless their layout attachment is inside the
    // subtree being cleared too.
    if (layout_node && is_svg_resource_box(*layout_node)) {
        RustFFI::FfiStaleNodeCallbacks callbacks {
            .layout_dom_node = [](void* layout_node_pointer) -> void* {
                VERIFY(layout_node_pointer);
                return static_cast<Layout::Node*>(layout_node_pointer)->dom_node(); },
            .dom_is_shadow_including_inclusive_descendant = [](void* node_pointer, void* root_pointer) {
                VERIFY(node_pointer);
                VERIFY(root_pointer);
                return static_cast<DOM::Node*>(node_pointer)->is_shadow_including_inclusive_descendant_of(*static_cast<DOM::Node*>(root_pointer)); },
        };
        // The cleared root is only ever looked at here, so the walk carries it as an identity and it is resolved once
        // an SVG resource box asks rather than once per cleared node.
        auto* cleared_subtree_root_node = cleared_subtree_root ? &dom_node_for_style_node(node.document(), cleared_subtree_root) : nullptr;
        if (RustFFI::rust_should_preserve_svg_resource_layout_node(
                &callbacks, layout_node->arena_handle(), Node::slot_id(layout_node), cleared_subtree_root_node))
            return TraversalDecision::SkipChildrenAndContinue;
    }

    if (layout_node)
        layout_node->clear_committed_box();
    LayoutTreeBuilderAccess::detach_layout_node(node);
    if (layout_node && layout_node->parent()) {
        // The parent may keep its subtree (a child lost its box in place); an emptied container
        // reads as having block-level children, like a freshly built one.
        auto* parent = layout_node->parent();
        destroy_layout_subtree(*layout_node);
        if (!parent->has_children())
            parent->set_children_are_inline(false);
    }

    if (is<DOM::Element>(node))
        LayoutTreeBuilderAccess::clear_synthetic_pseudo_element_layout_nodes(static_cast<DOM::Element&>(node));

    return TraversalDecision::Continue;
}

void LayoutTreeBuildBridge::detach_top_layer_element_layout_subtree(DOM::Element& element)
{
    RustFFI::FfiTopLayerDetachCallbacks callbacks {
        .context = &element.document(),
        .prepare_subtree_for_detach = [](void* layout_node_pointer) {
            VERIFY(layout_node_pointer);
            static_cast<Layout::Node*>(layout_node_pointer)->prepare_subtree_for_detach_from_layout_tree(); },
        .clear_stale_layout_node = [](void* document_pointer, u32 style_node, u32 cleared_subtree_root) -> bool {
            VERIFY(document_pointer);
            auto& document = *static_cast<DOM::Document*>(document_pointer);
            auto decision = clear_stale_layout_node(dom_node_for_style_node(document, style_node), cleared_subtree_root);
            return decision == TraversalDecision::SkipChildrenAndContinue; },
    };
    RustFFI::rust_detach_top_layer_element_layout_subtree(
        &callbacks, element.document().layout_node_arena().handle(), element.style_node_id().value());
}

LayoutTreeBuildBridge::~LayoutTreeBuildBridge()
{
    if (m_pinned_style_records.is_empty())
        return;
    auto& style_computer = m_document->style_computer();
    for (auto style_record_identity : m_pinned_style_records)
        style_computer.unpin_style_record(style_record_identity);
}

void LayoutTreeBuildBridge::pin_style_record_for_build(CSS::StyleRecordID style_record_identity)
{
    m_document->style_computer().pin_style_record(style_record_identity);
    m_pinned_style_records.append(style_record_identity);
}

RustFFI::FfiDomTreeBuilderCallbacks LayoutTreeBuildBridge::make_ffi_dom_tree_builder_callbacks()
{
    return {
        .builder = this,
        .clear_stale_layout_node = [](void* builder_pointer, u32 style_node, u32 cleared_subtree_root) -> bool {
            VERIFY(builder_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            auto decision = clear_stale_layout_node(dom_node_for_style_node(*builder.m_document, style_node), cleared_subtree_root);
            return decision == TraversalDecision::SkipChildrenAndContinue; },
        .principal_descendant_facts = [](void*, void* node_pointer, void* layout_node_pointer) -> RustFFI::FfiPrincipalDescendantFacts {
            VERIFY(node_pointer);
            VERIFY(layout_node_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            auto* graphics_element = as_if<SVG::SVGGraphicsElement>(node);
            GC::Ptr<SVG::SVGMaskElement const> mask;
            GC::Ptr<SVG::SVGClipPathElement const> clip_path;
            GC::Ptr<SVG::SVGPatternElement const> fill_pattern;
            GC::Ptr<SVG::SVGPatternElement const> stroke_pattern;
            if (graphics_element) {
                auto const& layout_node = as<NodeWithStyle>(*static_cast<Node*>(layout_node_pointer));
                mask = graphics_element->mask(layout_node);
                clip_path = graphics_element->clip_path(layout_node);
                fill_pattern = graphics_element->fill_pattern(layout_node);
                stroke_pattern = graphics_element->stroke_pattern(layout_node);
            }
            return {
                .svg_graphics_element = graphics_element ? Node::style_node_of(graphics_element).value() : 0,
                .svg_mask = identified_dom_node(mask.ptr()),
                .svg_clip_path = identified_dom_node(clip_path.ptr()),
                .svg_fill_pattern = identified_dom_node(fill_pattern.ptr()),
                .svg_stroke_pattern = identified_dom_node(stroke_pattern.ptr()),
            }; },
        .create_first_letter_nodes = [](void*, void* element_pointer, RustFFI::FfiFirstLetterTarget target) -> RustFFI::FfiFirstLetterNodes {
            VERIFY(element_pointer);
            return create_first_letter_nodes(*static_cast<DOM::Element*>(element_pointer), target); },
        .top_layer_element_count = [](void* document_pointer) {
            VERIFY(document_pointer);
            return static_cast<DOM::Document*>(document_pointer)->top_layer_elements().size(); },
        .copy_top_layer_elements = [](void* document_pointer, RustFFI::FfiIdentifiedDomNode* output, size_t count) {
            VERIFY(document_pointer);
            VERIFY(output || count == 0);
            auto const& elements = static_cast<DOM::Document*>(document_pointer)->top_layer_elements();
            VERIFY(count == elements.size());
            size_t index = 0;
            for (auto const& element : elements)
                output[index++] = identified_dom_node(element.ptr()); },
        .svg_pattern_content_element = [](void* pattern_pointer) -> RustFFI::FfiIdentifiedDomNode {
            VERIFY(pattern_pointer);
            return identified_dom_node(static_cast<SVG::SVGPatternElement*>(pattern_pointer)->pattern_content_element().ptr()); },
        .principal_dom_node = [](void* builder_pointer, u32 style_node) -> void* {
            VERIFY(builder_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            return &dom_node_for_style_node(*builder.m_document, style_node); },
        .prepare_principal_element = [](void* builder_pointer, void* element_pointer, bool should_create_layout_node) {
            VERIFY(builder_pointer);
            VERIFY(element_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            element.update_inside_blocking_wheel_event_handler_state();
            if (should_create_layout_node) {
                LayoutTreeBuilderAccess::clear_synthetic_pseudo_element_layout_nodes(element);
                update_style_if_needed_for_layout_tree_bypass_path(element);
            }
            if (!should_create_layout_node && element.needs_pseudo_element_layout_tree_update()) {
                for (auto pseudo_element : { CSS::PseudoElement::Before, CSS::PseudoElement::After }) {
                    if (auto* pseudo_node = element.pseudo_element_unsafe_layout_node(pseudo_element)) {
                        pseudo_node->for_each_in_inclusive_subtree([](Layout::Node& node) {
                            node.clear_committed_box();
                            return TraversalDecision::Continue;
                        });
                        pseudo_node->prepare_subtree_for_detach_from_layout_tree();
                        VERIFY(destroy_layout_subtree(*pseudo_node));
                        LayoutTreeBuilderAccess::set_synthetic_pseudo_element_node(element, pseudo_element, nullptr);
                    }
                }
                if (auto* layout_node = element.unsafe_layout_node(); !layout_node->has_children())
                    layout_node->set_children_are_inline(false);
            }
            auto style_record_identity = element.style_record_identity();
            VERIFY(style_record_identity);
            static_cast<LayoutTreeBuildBridge*>(builder_pointer)->pin_style_record_for_build(style_record_identity); },
        .create_principal_element_layout = [](void* builder_pointer, void* element_pointer, RustFFI::FfiElementLayoutKind kind) -> Compositing::RustFFI::NodeSlotId {
            VERIFY(builder_pointer);
            VERIFY(element_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto style_record_identity = element.style_record_identity();
            VERIFY(style_record_identity);
            CSS::LayoutStyle style { style_record_identity };
            Layout::Node* layout_node = nullptr;
            switch (kind) {
            case RustFFI::FfiElementLayoutKind::ContentReplacement: {
                auto const* content_values = element.style_group<CSS::ComputedValues::ContentValues>();
                VERIFY(content_values);
                auto computed_content = content_values->computed_content_value();
                auto replacement_image = content_replacement_image(computed_content);
                VERIFY(replacement_image);
                layout_node = &create_content_image_box(element.document(), element, style, const_cast<CSS::AbstractImageStyleValue&>(*replacement_image));
                break;
            }
            case RustFFI::FfiElementLayoutKind::SvgMask:
                layout_node = &allocate_layout_node<Layout::Box>(element.document(), element, style, RustFFI::NodeKind::SVGMaskBox);
                break;
            case RustFFI::FfiElementLayoutKind::SvgClipPath:
                layout_node = &allocate_layout_node<Layout::Box>(element.document(), element, style, RustFFI::NodeKind::SVGClipBox);
                break;
            case RustFFI::FfiElementLayoutKind::SvgPattern:
                layout_node = &allocate_layout_node<Layout::Box>(element.document(), element, style, RustFFI::NodeKind::SVGPatternBox);
                break;
            case RustFFI::FfiElementLayoutKind::Normal:
                layout_node = element.create_layout_node(style);
                break;
            }
            return Node::slot_id(layout_node); },
        .create_principal_document_layout = [](void*, void* document_pointer) -> Compositing::RustFFI::NodeSlotId {
            VERIFY(document_pointer);
            auto& document = *static_cast<DOM::Document*>(document_pointer);
            return Node::slot_id(&allocate_layout_node<Layout::Viewport>(document, document.style_computer().create_document_style())); },
        .create_principal_text_layout = [](void* text_pointer) -> Compositing::RustFFI::NodeSlotId {
            VERIFY(text_pointer);
            return create_layout_node_for_text(*static_cast<DOM::Text*>(text_pointer)); },
        .attach_style_resources = [](void* builder_pointer, Compositing::RustFFI::NodeSlotId slot) {
            VERIFY(builder_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            auto* layout_node = static_cast<Node*>(RustFFI::layout_arena_node_shell_if_live(builder.m_document->layout_node_arena().handle(), slot));
            VERIFY(layout_node);
            as<NodeWithStyle>(*layout_node).attach_style_resources(); },
        .layout = make_ffi_tree_builder_callbacks(),
        .pseudo = make_ffi_pseudo_tree_builder_callbacks(),
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

static Compositing::RustFFI::NodeSlotId create_layout_node_for_text(DOM::Text& text_node)
{
    text_node.update_inside_blocking_wheel_event_handler_state();
    return Node::slot_id(&allocate_layout_node<Layout::TextNode>(text_node.document(), text_node));
}

RustFFI::FfiLayoutTreeBuildOutcome LayoutTreeBuildBridge::build(DOM::Node& dom_node)
{
    m_document = &dom_node.document();
    auto callbacks = make_ffi_dom_tree_builder_callbacks();
    auto& document = dom_node.document();
    return RustFFI::rust_build_layout_tree(&callbacks, document.layout_node_arena().handle(), &dom_node, document.style_node_id().value());
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

RustFFI::FfiTreeBuilderCallbacks LayoutTreeBuildBridge::make_ffi_tree_builder_callbacks()
{
    return {
        .context = this,

        .prepare_subtree_for_detach = [](void*, void* layout_node_pointer) {
            VERIFY(layout_node_pointer);
            static_cast<Node*>(layout_node_pointer)->prepare_subtree_for_detach_from_layout_tree(); },
    };
}

// https://drafts.csswg.org/css-tables-3/#fixup-algorithm

}
