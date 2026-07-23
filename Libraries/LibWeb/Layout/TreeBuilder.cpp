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
#include <AK/Utf16String.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/Layout/AbsposLayoutInputs.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/FieldSetBox.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/ListItemBox.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/SVGClipBox.h>
#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Layout/SVGPatternBox.h>
#include <LibWeb/Layout/TableGrid.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TreeBuilder.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/SVG/SVGSwitchElement.h>

namespace Web::Layout {

TreeBuilder::TreeBuilder()
    : m_rust_state(RustFFI::rust_tree_builder_state_create())
{
    VERIFY(m_rust_state);
}

TreeBuilder::~TreeBuilder()
{
    RustFFI::rust_tree_builder_state_destroy(m_rust_state);
}

void TreeBuilder::push_parent(Layout::NodeWithStyle& parent)
{
    RustFFI::rust_tree_builder_push_parent(m_rust_state, &parent);
}

void TreeBuilder::pop_parent()
{
    RustFFI::rust_tree_builder_pop_parent(m_rust_state);
}

Layout::NodeWithStyle& TreeBuilder::current_parent() const
{
    return *static_cast<Layout::NodeWithStyle*>(RustFFI::rust_tree_builder_current_parent(m_rust_state));
}

size_t TreeBuilder::ancestor_count() const
{
    return RustFFI::rust_tree_builder_ancestor_count(m_rust_state);
}

Layout::NodeWithStyle& TreeBuilder::ancestor_at(size_t index) const
{
    return *static_cast<Layout::NodeWithStyle*>(RustFFI::rust_tree_builder_ancestor_at(m_rust_state, index));
}

u32 TreeBuilder::quote_nesting_level() const
{
    return RustFFI::rust_tree_builder_quote_nesting_level(m_rust_state);
}

void TreeBuilder::set_quote_nesting_level(u32 quote_nesting_level)
{
    RustFFI::rust_tree_builder_set_quote_nesting_level(m_rust_state, quote_nesting_level);
}

static RustFFI::FfiTreeBuilderCallbacks make_ffi_tree_builder_callbacks(TreeBuilder*);
static RustFFI::FfiPrincipalDisplayFacts ffi_principal_display_facts(CSS::Display);
static void update_style_if_needed_for_layout_tree_bypass_path(DOM::Element&);
static RefPtr<Layout::Node> create_layout_node_for_text(DOM::Text&);

void TreeBuilder::note_tree_restructuring_at(Layout::Node const& node)
{
    if (m_current_rebuild_root && !m_current_rebuild_root->is_inclusive_ancestor_of(node))
        m_layout_tree_update_escaped_rebuild_roots = true;
}

void TreeBuilder::insert_node_into_inline_or_block_ancestor(Layout::Node& node, CSS::Display display, AppendOrPrepend mode)
{
    auto callbacks = make_ffi_tree_builder_callbacks(this);
    RustFFI::rust_insert_node_into_inline_or_block_ancestor(
        &callbacks,
        &current_parent(),
        &node,
        display.is_inline_outside(),
        mode == AppendOrPrepend::Append ? RustFFI::FfiInsertionMode::Append : RustFFI::FfiInsertionMode::Prepend);
}

class GeneratedContentImageProvider final
    : public ImageProvider {
public:
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
        if (auto document = m_document.ptr()) {
            if (auto const* image = selected_image_style_value())
                return image->image_data(*document);
        }
        return nullptr;
    }

    virtual Optional<CSSPixels> intrinsic_width() const override
    {
        if (auto document = m_document.ptr())
            return m_image->natural_width(*document);
        return {};
    }

    virtual Optional<CSSPixels> intrinsic_height() const override
    {
        if (auto document = m_document.ptr())
            return m_image->natural_height(*document);
        return {};
    }

    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override
    {
        if (auto document = m_document.ptr())
            return m_image->natural_aspect_ratio(*document);
        return {};
    }

private:
    class ImageClient final : public CSS::ImageStyleValue::Client {
    public:
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
            m_owner.m_layout_node->set_needs_layout_update(DOM::SetNeedsLayoutReason::GeneratedContentImageFinishedLoading);
        }

    private:
        GeneratedContentImageProvider const& m_owner;
    };

    GeneratedContentImageProvider(DOM::Document& document, NonnullRefPtr<CSS::AbstractImageStyleValue> image)
        : m_document(document)
        , m_image(move(image))
    {
        if (auto const* image = selected_image_style_value())
            m_image_client = make<ImageClient>(*this, document, *image);
    }

    CSS::ImageStyleValue const* selected_image_style_value() const
    {
        if (m_image->is_image())
            return &m_image->as_image();

        if (m_image->is_image_set()) {
            if (auto const* selected_image = m_image->as_image_set().selected_image(); selected_image && selected_image->is_image())
                return &selected_image->as_image();
        }

        return nullptr;
    }

    GC::Weak<DOM::Document> m_document;
    mutable WeakPtr<Layout::Node> m_layout_node;
    NonnullRefPtr<CSS::AbstractImageStyleValue> m_image;
    mutable OwnPtr<ImageClient> m_image_client;
};

static NonnullRefPtr<ImageBox> create_content_image_box(DOM::Document& document, GC::Ptr<DOM::Element> element, NonnullRefPtr<CSS::ComputedValues const> style, CSS::AbstractImageStyleValue& image)
{
    image.load_any_resources(document);
    auto image_provider = GeneratedContentImageProvider::create(document, image);
    auto& image_provider_ref = *image_provider;
    auto image_box = make_ref_counted<ImageBox>(document, element, style, move(image_provider));
    image_provider_ref.set_layout_node(*image_box);
    return image_box;
}

static CSS::AbstractImageStyleValue const* content_replacement_image(CSS::ComputedContentData const& content)
{
    if (content.type != CSS::ComputedContentData::Type::List
        || content.items.size() != 1
        || !content.items.first().has<NonnullRefPtr<CSS::AbstractImageStyleValue const>>()) {
        return nullptr;
    }

    return content.items.first().get<NonnullRefPtr<CSS::AbstractImageStyleValue const>>().ptr();
}

struct FirstLetterTextSlices {
    NonnullRefPtr<TextNode> first_letter_slice;
    NonnullRefPtr<TextNode> remainder_slice;
};

static FirstLetterTextSlices create_first_letter_text_slices(DOM::Document& document, TextNode& text_node, size_t letter_end)
{
    auto const full_length = text_node.text().length_in_code_units();

    // The first-letter and remainder boxes render slices of the same DOM text node; generated text
    // (from a content property) has no DOM node and gets plain generated slices of its text instead.
    if (auto* dom_text = text_node.dom_text()) {
        auto& mutable_dom_text = const_cast<DOM::Text&>(*dom_text);
        auto remainder_slice = make_ref_counted<TextSliceNode>(document, mutable_dom_text, Node::AttachToDOMNode::Yes, letter_end, full_length - letter_end);
        auto first_letter_slice = make_ref_counted<TextSliceNode>(document, mutable_dom_text, Node::AttachToDOMNode::No, 0, letter_end);
        remainder_slice->set_first_letter_slice(*first_letter_slice);
        return { move(first_letter_slice), move(remainder_slice) };
    }

    auto text = text_node.text();
    return {
        make_ref_counted<GeneratedTextNode>(document, Utf16String::from_utf16(text.utf16_view().substring_view(0, letter_end))),
        make_ref_counted<GeneratedTextNode>(document, Utf16String::from_utf16(text.utf16_view().substring_view(letter_end, full_length - letter_end))),
    };
}

void TreeBuilder::create_first_letter_wrapper_if_needed(DOM::Element& element, BlockContainer& block_container)
{
    if (!element.computed_values(CSS::PseudoElement::FirstLetter))
        return;

    auto callbacks = make_ffi_tree_builder_callbacks(nullptr);
    auto target = RustFFI::rust_find_first_letter_in_block(&callbacks, &block_container);
    if (!target.found)
        return;

    auto& text_node = as<TextNode>(*static_cast<Node*>(target.text_node));
    auto& document = element.document();

    auto [first_letter_slice, remainder_slice] = create_first_letter_text_slices(document, text_node, target.letter_end);

    auto first_letter_values = element.computed_values(CSS::PseudoElement::FirstLetter);
    VERIFY(first_letter_values);
    auto display = first_letter_values->display();
    auto first_letter_wrapper = DOM::Element::create_layout_node_for_display_type(document, display, first_letter_values.release_nonnull(), nullptr);
    if (!first_letter_wrapper)
        return;
    first_letter_wrapper->attach_style_resources();
    first_letter_wrapper->set_generated_for(CSS::PseudoElement::FirstLetter, element);
    first_letter_wrapper->set_children_are_inline(true);
    first_letter_wrapper->append_child(*first_letter_slice);
    element.set_synthetic_pseudo_element_node({}, CSS::PseudoElement::FirstLetter, first_letter_wrapper);

    auto* parent = text_node.parent();
    VERIFY(parent);
    parent->insert_before(*first_letter_wrapper, text_node);
    parent->insert_before(*remainder_slice, text_node);
    parent->remove_child(text_node);
}

NonnullRefPtr<ListItemMarkerBox> TreeBuilder::create_and_attach_list_item_marker(ListItemBox& list_box, DOM::Element& element, NonnullRefPtr<CSS::ComputedValues const> marker_style)
{
    auto list_item_marker = make_ref_counted<ListItemMarkerBox>(
        list_box.document(),
        list_box.computed_values().list_style_type(),
        list_box.computed_values().list_style_position(),
        element,
        move(marker_style));
    list_item_marker->attach_style_resources();
    list_box.set_marker(list_item_marker);
    element.set_synthetic_pseudo_element_node({}, CSS::PseudoElement::Marker, list_item_marker);
    list_box.prepend_child(*list_item_marker);
    return list_item_marker;
}

static RustFFI::FfiPseudoElement ffi_pseudo_element(CSS::PseudoElement pseudo_element)
{
    switch (pseudo_element) {
    case CSS::PseudoElement::Before:
        return RustFFI::FfiPseudoElement::Before;
    case CSS::PseudoElement::After:
        return RustFFI::FfiPseudoElement::After;
    case CSS::PseudoElement::Marker:
        return RustFFI::FfiPseudoElement::Marker;
    case CSS::PseudoElement::Backdrop:
        return RustFFI::FfiPseudoElement::Backdrop;
    default:
        return RustFFI::FfiPseudoElement::Other;
    }
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
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

static RustFFI::FfiComputedContentType ffi_computed_content_type(CSS::ComputedContentData::Type content_type)
{
    switch (content_type) {
    case CSS::ComputedContentData::Type::Normal:
        return RustFFI::FfiComputedContentType::Normal;
    case CSS::ComputedContentData::Type::None:
        return RustFFI::FfiComputedContentType::None;
    case CSS::ComputedContentData::Type::List:
        return RustFFI::FfiComputedContentType::List;
    }
    VERIFY_NOT_REACHED();
}

struct PseudoElementFrame {
    RefPtr<CSS::ComputedValues const> computed_values;
    CSS::Display display;
    CSS::AbstractImageStyleValue const* replacement_image { nullptr };
    ListItemBox* originating_list_box { nullptr };
    RefPtr<NodeWithStyle> layout_node;
    CSS::ContentData resolved_content;
    RefPtr<Layout::Node> content_item;
};

RefPtr<NodeWithStyle> TreeBuilder::create_pseudo_element_if_needed(DOM::Element& element, CSS::PseudoElement pseudo_element, Optional<AppendOrPrepend> insertion_mode)
{
    PseudoElementFrame frame;
    auto callbacks = make_ffi_pseudo_tree_builder_callbacks();
    auto* layout_node = RustFFI::rust_create_pseudo_element(
        &callbacks,
        &frame,
        &element,
        ffi_pseudo_element(pseudo_element),
        insertion_mode.has_value(),
        insertion_mode.value_or(AppendOrPrepend::Append) == AppendOrPrepend::Append
            ? RustFFI::FfiInsertionMode::Append
            : RustFFI::FfiInsertionMode::Prepend);
    return static_cast<NodeWithStyle*>(layout_node);
}

RustFFI::FfiPseudoTreeBuilderCallbacks TreeBuilder::make_ffi_pseudo_tree_builder_callbacks()
{
    return {
        .builder = this,
        .initialize = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo) -> RustFFI::FfiPseudoElementFacts {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto pseudo_element = css_pseudo_element(ffi_pseudo);
            if (auto existing_pseudo = element.get_synthetic_pseudo_element(pseudo_element); existing_pseudo.has_value() && existing_pseudo->layout_node())
                existing_pseudo->set_layout_node(nullptr);
            frame.computed_values = element.computed_values(pseudo_element);
            frame.replacement_image = nullptr;
            frame.originating_list_box = nullptr;
            frame.layout_node = nullptr;
            frame.content_item = nullptr;
            if (!frame.computed_values) {
                return {
                    .has_style = false,
                    .pseudo_element = ffi_pseudo,
                    .content_type = RustFFI::FfiComputedContentType::None,
                    .display_is_none = false,
                    .display_is_contents = false,
                    .display_is_list_item = false,
                    .has_content_replacement = false,
                    .originating_layout_node_is_list_item = false,
                    .normal_marker_has_content = false,
                };
            }
            frame.display = frame.computed_values->display();
            auto const computed_content_type = frame.computed_values->computed_content().type;
            frame.replacement_image = content_replacement_image(frame.computed_values->computed_content());
            if (pseudo_element == CSS::PseudoElement::Marker && computed_content_type == CSS::ComputedContentData::Type::Normal)
                frame.originating_list_box = as_if<ListItemBox>(*element.unsafe_layout_node());
            auto const normal_marker_has_content = frame.originating_list_box
                && (!frame.originating_list_box->computed_values().list_style_type().has<Empty>() || frame.originating_list_box->list_style_image());
            return {
                .has_style = true,
                .pseudo_element = ffi_pseudo,
                .content_type = ffi_computed_content_type(computed_content_type),
                .display_is_none = frame.display.is_none(),
                .display_is_contents = frame.display.is_contents(),
                .display_is_list_item = frame.display.is_list_item(),
                .has_content_replacement = frame.replacement_image != nullptr,
                .originating_layout_node_is_list_item = frame.originating_list_box != nullptr,
                .normal_marker_has_content = normal_marker_has_content,
            }; },
        .create_layout_node = [](void* builder_pointer, void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement, RustFFI::FfiPseudoElementDecision decision) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            VERIFY(frame.computed_values);
            auto& document = element.document();
            switch (decision) {
            case RustFFI::FfiPseudoElementDecision::None:
                VERIFY_NOT_REACHED();
            case RustFFI::FfiPseudoElementDecision::NormalMarker:
                VERIFY(frame.originating_list_box);
                frame.layout_node = create_and_attach_list_item_marker(*frame.originating_list_box, element, NonnullRefPtr { *frame.computed_values });
                break;
            case RustFFI::FfiPseudoElementDecision::ContentReplacement:
                VERIFY(frame.replacement_image);
                frame.layout_node = create_content_image_box(document, nullptr, NonnullRefPtr { *frame.computed_values }, const_cast<CSS::AbstractImageStyleValue&>(*frame.replacement_image));
                break;
            case RustFFI::FfiPseudoElementDecision::Contents:
                frame.layout_node = make_ref_counted<InlineNode>(document, nullptr, NonnullRefPtr { *frame.computed_values });
                frame.layout_node->set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
                break;
            case RustFFI::FfiPseudoElementDecision::Box:
                frame.layout_node = DOM::Element::create_layout_node_for_display_type(document, frame.display, NonnullRefPtr { *frame.computed_values }, nullptr);
                break;
            } },
        .layout_node = [](void* frame_pointer) -> void* {
            VERIFY(frame_pointer);
            return static_cast<PseudoElementFrame*>(frame_pointer)->layout_node.ptr(); },
        .attach_style_resources = [](void* frame_pointer) {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            frame.layout_node->attach_style_resources(); },
        .layout_facts = [](void* frame_pointer) -> RustFFI::FfiPrincipalLayoutFacts {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            return {
                .is_replaced_element = frame.layout_node->is_replaced_element(),
                .display = ffi_principal_display_facts(frame.display),
            }; },
        .apply_replaced_display_adjustment = [](void* frame_pointer, RustFFI::FfiReplacedElementDisplayAdjustment adjustment) {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            if (adjustment == RustFFI::FfiReplacedElementDisplayAdjustment::Block)
                frame.display = CSS::Display::from_short(CSS::Display::Short::Block);
            else if (adjustment == RustFFI::FfiReplacedElementDisplayAdjustment::Inline)
                frame.display = CSS::Display::from_short(CSS::Display::Short::Inline);
            else
                VERIFY_NOT_REACHED();
            frame.layout_node->set_display(frame.display); },
        .layout_node_is_list_item = [](void* frame_pointer) {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            return is<ListItemBox>(*frame.layout_node); },
        .create_nested_list_marker = [](void* builder_pointer, void* frame_pointer, void* element_pointer) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            VERIFY(frame.layout_node);
            auto marker_style = element.document().style_computer().compute_style({ element, CSS::PseudoElement::Marker });
            (void)builder.create_and_attach_list_item_marker(as<ListItemBox>(*frame.layout_node), element, move(marker_style)); },
        .configure_layout_node = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo, u32 initial_quote_nesting_level) {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto pseudo_element = css_pseudo_element(ffi_pseudo);
            VERIFY(frame.layout_node);
            frame.layout_node->set_generated_for(pseudo_element, element);
            frame.layout_node->set_initial_quote_nesting_level(initial_quote_nesting_level);
            element.set_synthetic_pseudo_element_node({}, pseudo_element, frame.layout_node); },
        .insert_layout_node = [](void* builder_pointer, void* frame_pointer, RustFFI::FfiInsertionMode insertion_mode) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            static_cast<TreeBuilder*>(builder_pointer)->insert_node_into_inline_or_block_ancestor(
                *frame.layout_node,
                frame.layout_node->display(),
                insertion_mode == RustFFI::FfiInsertionMode::Append ? AppendOrPrepend::Append : AppendOrPrepend::Prepend); },
        .resolve_counters = [](void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo) {
            VERIFY(element_pointer);
            DOM::AbstractElement element_reference { *static_cast<DOM::Element*>(element_pointer), css_pseudo_element(ffi_pseudo) };
            CSS::resolve_counters(element_reference); },
        .resolve_content = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo, u32 initial_quote_nesting_level) -> RustFFI::FfiResolvedPseudoContentFacts {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            VERIFY(frame.computed_values);
            VERIFY(frame.layout_node);
            DOM::AbstractElement element_reference { *static_cast<DOM::Element*>(element_pointer), css_pseudo_element(ffi_pseudo) };
            auto [content, final_quote_nesting_level] = frame.computed_values->resolved_content(element_reference, initial_quote_nesting_level);
            frame.resolved_content = move(content);
            frame.layout_node->set_content(frame.resolved_content);
            return {
                .final_quote_nesting_level = final_quote_nesting_level,
                .content_is_list = frame.resolved_content.type == CSS::ContentData::Type::List,
                .content_item_count = frame.resolved_content.data.size(),
            }; },
        .create_content_item = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo, size_t index) -> void* {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            VERIFY(frame.layout_node);
            VERIFY(index < frame.resolved_content.data.size());
            auto& item = frame.resolved_content.data[index];
            if (auto const* string = item.get_pointer<Utf16String>()) {
                frame.content_item = make_ref_counted<GeneratedTextNode>(element.document(), *string);
            } else {
                auto& image = *item.get<NonnullRefPtr<CSS::AbstractImageStyleValue>>();
                auto image_box = create_content_image_box(element.document(), nullptr, NonnullRefPtr { frame.layout_node->computed_values() }, image);
                // https://drafts.csswg.org/css-content-3/#content-property
                // For <image>, this is an inline anonymous replaced element.
                image_box->set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
                image_box->attach_style_resources();
                frame.content_item = move(image_box);
            }
            frame.content_item->set_generated_for(css_pseudo_element(ffi_pseudo), element);
            return frame.content_item.ptr(); },
        .insert_content_item = [](void* builder_pointer, void* content_item_pointer) {
            VERIFY(builder_pointer);
            VERIFY(content_item_pointer);
            auto& content_item = *static_cast<Layout::Node*>(content_item_pointer);
            auto display = content_item.is_text_node()
                ? CSS::Display::from_short(CSS::Display::Short::Inline)
                : as<NodeWithStyle>(content_item).display();
            static_cast<TreeBuilder*>(builder_pointer)->insert_node_into_inline_or_block_ancestor(content_item, display, AppendOrPrepend::Append); },
        .push_layout_parent = [](void* builder_pointer, void* layout_node_pointer) {
            VERIFY(builder_pointer);
            VERIFY(layout_node_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->push_parent(as<NodeWithStyle>(*static_cast<Layout::Node*>(layout_node_pointer))); },
        .pop_layout_parent = [](void* builder_pointer) {
            VERIFY(builder_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->pop_parent(); },
        .quote_nesting_level = [](void* builder_pointer) {
            VERIFY(builder_pointer);
            return static_cast<TreeBuilder*>(builder_pointer)->quote_nesting_level(); },
        .set_quote_nesting_level = [](void* builder_pointer, u32 quote_nesting_level) {
            VERIFY(builder_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->set_quote_nesting_level(quote_nesting_level); },
    };
}

RefPtr<NodeWithStyle> TreeBuilder::create_content_replacement_if_needed(DOM::Element& element, NonnullRefPtr<CSS::ComputedValues const> computed_values) const
{
    auto const* replacement_image = content_replacement_image(computed_values->computed_content());
    if (!replacement_image)
        return {};

    return create_content_image_box(element.document(), element, move(computed_values), const_cast<CSS::AbstractImageStyleValue&>(*replacement_image));
}

static bool is_svg_resource_box(Node const& layout_node)
{
    return is<SVGPatternBox>(layout_node) || is<SVGMaskBox>(layout_node) || is<SVGClipBox>(layout_node);
}

static bool layout_node_is_attached_to_dom_subtree(Node const& layout_node, DOM::Node const& subtree_root)
{
    for (auto* ancestor = layout_node.parent(); ancestor; ancestor = ancestor->parent()) {
        auto* dom_node = ancestor->dom_node();
        if (dom_node && dom_node->is_shadow_including_inclusive_descendant_of(subtree_root))
            return true;
    }
    return false;
}

// The replacement box represents the same element in the same tree position, so layout state
// saved by the previous layout pass carries over to it: the saved abspos layout inputs, and the
// flat fragment and inline-box-piece lists held by the containing block of a node that
// participated in inline layout, which a subtree relayout that skips the containing block never
// rebuilds.
static void transfer_saved_layout_state_to_replacement_box(Layout::Node& old_layout_node, Layout::Node& new_layout_node)
{
    if (auto const* old_box = as_if<Box>(old_layout_node)) {
        if (auto* new_box = as_if<Box>(new_layout_node)) {
            if (old_box->saved_abspos_layout_inputs())
                new_box->set_saved_abspos_layout_inputs(*old_box->saved_abspos_layout_inputs());
        }
    }
    if (auto* containing_block = old_layout_node.containing_block()) {
        if (auto* paintable_with_lines = as_if<Painting::PaintableWithLines>(containing_block->paintable().ptr())) {
            for (auto& fragment : paintable_with_lines->fragments()) {
                if (fragment.has_layout_node() && &fragment.layout_node() == &old_layout_node)
                    fragment.set_layout_node(new_layout_node);
            }
            for (auto& piece : paintable_with_lines->inline_box_pieces()) {
                if (piece.node.ptr() == &old_layout_node)
                    piece.node = &new_layout_node;
            }
        }
    }
}

static DOM::Element* display_contents_style_parent_for_text_node(DOM::Text& text_node)
{
    auto* parent = text_node.flat_tree_parent();
    auto* parent_element = as_if<DOM::Element>(parent);
    if (!parent_element || !parent_element->computed_values())
        return nullptr;
    if (!parent_element->computed_values()->display().is_contents())
        return nullptr;
    return parent_element;
}

static bool display_contents_text_needs_style_wrapper(DOM::Text& text_node, DOM::Element const& style_parent)
{
    if (!text_node.data().is_ascii_whitespace())
        return true;

    return !first_is_one_of(style_parent.computed_values()->white_space_collapse(), CSS::WhiteSpaceCollapse::Collapse);
}

TraversalDecision TreeBuilder::clear_stale_layout_and_paint_node(DOM::Node& node, DOM::Node const* cleared_subtree_root)
{
    node.set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
    node.set_child_needs_layout_tree_update(false);

    // NB: Called during layout tree construction.
    RefPtr<Layout::Node> layout_node = node.unsafe_layout_node();
    // SVGPatternBox, SVGMaskBox, and SVGClipBox are created on behalf of a referencing
    // element and attached to that element's layout subtree. Skip them so they survive
    // cleanup of their DOM ancestor, unless their layout attachment is inside the
    // subtree being cleared too.
    if (layout_node && is_svg_resource_box(*layout_node)
        && (!cleared_subtree_root || !layout_node_is_attached_to_dom_subtree(*layout_node, *cleared_subtree_root))) {
        return TraversalDecision::SkipChildrenAndContinue;
    }

    if (layout_node && layout_node->parent())
        layout_node->remove();

    node.detach_layout_node({});
    node.clear_paintable();

    if (is<DOM::Element>(node))
        static_cast<DOM::Element&>(node).clear_synthetic_pseudo_element_layout_nodes(Badge<TreeBuilder> {});

    return TraversalDecision::Continue;
}

void TreeBuilder::detach_top_layer_element_layout_subtree(DOM::Element& element)
{
    // NB: Called at DOM mutation processing time, outside layout tree construction.
    if (auto element_layout_node = RefPtr { element.unsafe_layout_node() }) {
        // Take along any anonymous wrapper table fixup created around the box; an emptied
        // table wrapper left behind as a viewport child asserts during layout.
        RefPtr<Layout::Node> layout_node_to_detach = element_layout_node;
        if (auto* top_layer_placement = element_layout_node->topmost_layout_node_of_top_layer_placement())
            layout_node_to_detach = top_layer_placement;
        layout_node_to_detach->prepare_subtree_for_detach_from_layout_tree();
        if (layout_node_to_detach->parent())
            layout_node_to_detach->remove();
    }
    element.for_each_shadow_including_inclusive_descendant([&](auto& node) {
        return clear_stale_layout_and_paint_node(node, &element);
    });
    if (auto* slot_element = as_if<HTML::HTMLSlotElement>(element))
        clear_stale_layout_nodes_for_assigned_slottables(*slot_element);
}

static bool element_has_an_unrendered_flat_tree_ancestor(DOM::Element const& element)
{
    for (auto const* ancestor = element.flat_tree_parent(); ancestor; ancestor = ancestor->flat_tree_parent()) {
        auto const* ancestor_element = as_if<DOM::Element>(*ancestor);
        if (!ancestor_element)
            continue;
        // Null style means the style update pass skipped a display:none subtree.
        auto ancestor_style = ancestor_element->computed_values();
        if (!ancestor_style || ancestor_style->display().is_none())
            return true;
    }
    return false;
}

struct PrincipalNodeFrame {
    RefPtr<Layout::Node> old_layout_node;
    RefPtr<Layout::Node> layout_node;
    RefPtr<CSS::ComputedValues const> computed_values;
    CSS::Display display;
};

static RustFFI::FfiPrincipalDisplayFacts ffi_principal_display_facts(CSS::Display display)
{
    return {
        .display_is_none = display.is_none(),
        .display_is_contents = display.is_contents(),
        .display_is_table_inside = display.is_table_inside(),
        .display_is_block_outside = display.is_block_outside(),
        .display_is_internal_table = display.is_internal_table(),
        .display_is_table_caption = display.is_table_caption(),
    };
}

RustFFI::FfiDomTreeBuilderCallbacks TreeBuilder::make_ffi_dom_tree_builder_callbacks()
{
    return {
        .builder = this,
        .first_child = [](void* parent_pointer) -> void* {
            VERIFY(parent_pointer);
            return static_cast<DOM::ParentNode*>(parent_pointer)->first_child();
        },
        .next_sibling = [](void* node_pointer) -> void* {
            VERIFY(node_pointer);
            return static_cast<DOM::Node*>(node_pointer)->next_sibling();
        },
        .update_layout_tree = [](void* builder_pointer, void* node_pointer, void* context_pointer, bool must_create_subtree) {
            VERIFY(builder_pointer);
            VERIFY(node_pointer);
            VERIFY(context_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->update_layout_tree(
                *static_cast<DOM::Node*>(node_pointer),
                *static_cast<Context*>(context_pointer),
                must_create_subtree ? MustCreateSubtree::Yes : MustCreateSubtree::No); },
        .clear_update_flags = [](void* node_pointer) {
            VERIFY(node_pointer);
            auto& node = *static_cast<DOM::ParentNode*>(node_pointer);
            node.set_child_needs_layout_tree_update(false);
            node.set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None); },
        .needs_layout_tree_update = [](void* node_pointer) {
            VERIFY(node_pointer);
            return static_cast<DOM::Node*>(node_pointer)->needs_layout_tree_update(); },
        .assigned_node_count = [](void* slot_element_pointer) {
            VERIFY(slot_element_pointer);
            return static_cast<HTML::HTMLSlotElement*>(slot_element_pointer)->assigned_nodes_internal().size(); },
        .assigned_node_at = [](void* slot_element_pointer, size_t index) -> void* {
            VERIFY(slot_element_pointer);
            auto assigned_nodes = static_cast<HTML::HTMLSlotElement*>(slot_element_pointer)->assigned_nodes_internal();
            VERIFY(index < assigned_nodes.size());
            DOM::Node* node = nullptr;
            assigned_nodes[index].visit([&](auto& assigned_node) { node = assigned_node.ptr(); });
            return node;
        },
        .is_svg_element = [](void* node_pointer) {
            VERIFY(node_pointer);
            return is<SVG::SVGElement>(*static_cast<DOM::Node*>(node_pointer)); },
        .clear_stale_layout_and_paint_node = [](void* builder_pointer, void* node_pointer) {
            VERIFY(builder_pointer);
            VERIFY(node_pointer);
            (void)static_cast<TreeBuilder*>(builder_pointer)->clear_stale_layout_and_paint_node(*static_cast<DOM::Node*>(node_pointer)); },
        .display_contents_facts = [](void*, void* element_pointer, void* context_pointer) -> RustFFI::FfiDisplayContentsFacts {
            VERIFY(element_pointer);
            VERIFY(context_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto& context = *static_cast<Context*>(context_pointer);
            auto* slot_element = as_if<HTML::HTMLSlotElement>(element);
            auto shadow_root = element.shadow_root();
            return {
                .rendered_in_top_layer = element.rendered_in_top_layer(),
                .context_layout_top_layer = context.layout_top_layer,
                .content_visibility_hidden = element.computed_values()->content_visibility() == CSS::ContentVisibility::Hidden,
                .should_layout_dom_children = slot_element ? slot_element->assigned_nodes_internal().is_empty() && element.has_children() : element.has_children(),
                .child_needs_layout_tree_update = element.child_needs_layout_tree_update(),
                .dom_children_parent = static_cast<DOM::ParentNode*>(&element),
                .shadow_root = shadow_root ? static_cast<DOM::ParentNode*>(shadow_root.ptr()) : nullptr,
                .slot_element = slot_element,
            };
        },
        .set_context_layout_top_layer = [](void* context_pointer, bool layout_top_layer) {
            VERIFY(context_pointer);
            static_cast<Context*>(context_pointer)->layout_top_layer = layout_top_layer; },
        .clear_synthetic_pseudo_element_layout_nodes = [](void*, void* element_pointer) {
            VERIFY(element_pointer);
            static_cast<DOM::Element*>(element_pointer)->clear_synthetic_pseudo_element_layout_nodes(Badge<TreeBuilder> {}); },
        .clear_stale_inclusive_descendants = [](void* builder_pointer, void* element_pointer) {
            VERIFY(builder_pointer);
            VERIFY(element_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            static_cast<DOM::Element*>(element_pointer)->for_each_shadow_including_inclusive_descendant([&](auto& node) {
                return builder.clear_stale_layout_and_paint_node(node);
            }); },
        .resolve_counters = [](void* element_pointer) {
            VERIFY(element_pointer);
            DOM::AbstractElement element_reference { *static_cast<DOM::Element*>(element_pointer) };
            CSS::resolve_counters(element_reference); },
        .create_pseudo_element = [](void* builder_pointer, void* element_pointer, RustFFI::FfiPseudoElement pseudo_element, RustFFI::FfiInsertionMode insertion_mode) {
            VERIFY(builder_pointer);
            VERIFY(element_pointer);
            auto css_pseudo_element = [&] {
                switch (pseudo_element) {
                case RustFFI::FfiPseudoElement::Before:
                    return CSS::PseudoElement::Before;
                case RustFFI::FfiPseudoElement::After:
                    return CSS::PseudoElement::After;
                case RustFFI::FfiPseudoElement::Marker:
                    return CSS::PseudoElement::Marker;
                default:
                    VERIFY_NOT_REACHED();
                }
            }();
            auto mode = insertion_mode == RustFFI::FfiInsertionMode::Append ? AppendOrPrepend::Append : AppendOrPrepend::Prepend;
            (void)static_cast<TreeBuilder*>(builder_pointer)->create_pseudo_element_if_needed(*static_cast<DOM::Element*>(element_pointer), css_pseudo_element, mode); },
        .clear_stale_assigned_slottables = [](void* slot_element_pointer) {
            VERIFY(slot_element_pointer);
            clear_stale_layout_nodes_for_assigned_slottables(*static_cast<HTML::HTMLSlotElement*>(slot_element_pointer)); },
        .principal_descendant_facts = [](void*, void* node_pointer, void* layout_node_pointer, void* context_pointer) -> RustFFI::FfiPrincipalDescendantFacts {
            VERIFY(node_pointer);
            VERIFY(layout_node_pointer);
            VERIFY(context_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            auto& layout_node = *static_cast<Layout::Node*>(layout_node_pointer);
            auto& context = *static_cast<Context*>(context_pointer);
            auto* element = as_if<DOM::Element>(node);
            auto* slot_element = as_if<HTML::HTMLSlotElement>(node);
            auto* parent_node = as_if<DOM::ParentNode>(node);
            auto shadow_root = element ? element->shadow_root() : nullptr;
            auto* graphics_element = as_if<SVG::SVGGraphicsElement>(node);
            auto mask = graphics_element ? graphics_element->mask() : nullptr;
            auto clip_path = graphics_element ? graphics_element->clip_path() : nullptr;
            auto fill_pattern = graphics_element ? graphics_element->fill_pattern() : nullptr;
            auto stroke_pattern = graphics_element ? graphics_element->stroke_pattern() : nullptr;
            return {
                .is_element = element != nullptr,
                .context_layout_top_layer = context.layout_top_layer,
                .content_visibility_hidden = element && element->computed_values()->content_visibility() == CSS::ContentVisibility::Hidden,
                .should_layout_dom_children = slot_element ? slot_element->assigned_nodes_internal().is_empty() && node.has_children() : node.has_children(),
                .child_needs_layout_tree_update = node.child_needs_layout_tree_update(),
                .layout_node_can_have_children = layout_node.can_have_children(),
                .layout_node_is_replaced_box_with_children = layout_node.is_replaced_box_with_children(),
                .layout_node_is_list_item_box = layout_node.is_list_item_box(),
                .layout_node_is_block_container = is<BlockContainer>(layout_node),
                .is_svg_switch_element = is<SVG::SVGSwitchElement>(node),
                .is_document = node.is_document(),
                .has_style_containment = is<NodeWithStyle>(layout_node) && static_cast<NodeWithStyle&>(layout_node).has_style_containment(),
                .dom_children_parent = parent_node,
                .shadow_root = shadow_root ? static_cast<DOM::ParentNode*>(shadow_root.ptr()) : nullptr,
                .slot_element = slot_element,
                .svg_graphics_element = graphics_element,
                .svg_mask = const_cast<SVG::SVGMaskElement*>(mask.ptr()),
                .svg_clip_path = const_cast<SVG::SVGClipPathElement*>(clip_path.ptr()),
                .svg_fill_pattern = const_cast<SVG::SVGPatternElement*>(fill_pattern.ptr()),
                .svg_stroke_pattern = const_cast<SVG::SVGPatternElement*>(stroke_pattern.ptr()),
                .context_layout_svg_mask_or_clip_path = context.layout_svg_mask_or_clip_path,
                .context_layout_svg_pattern = context.layout_svg_pattern,
            }; },
        .create_first_letter_wrapper = [](void*, void* element_pointer, void* layout_node_pointer) {
            VERIFY(element_pointer);
            VERIFY(layout_node_pointer);
            create_first_letter_wrapper_if_needed(
                *static_cast<DOM::Element*>(element_pointer),
                as<BlockContainer>(*static_cast<Layout::Node*>(layout_node_pointer))); },
        .wrap_fieldset_layout = [](void* builder_pointer, void* layout_node_pointer) {
            VERIFY(builder_pointer);
            VERIFY(layout_node_pointer);
            auto callbacks = make_ffi_tree_builder_callbacks(static_cast<TreeBuilder*>(builder_pointer));
            RustFFI::rust_wrap_fieldset_contents_if_needed(&callbacks, layout_node_pointer); },
        .wrap_button_layout = [](void* builder_pointer, void* node_pointer, void* layout_node_pointer) {
            VERIFY(builder_pointer);
            VERIFY(node_pointer);
            VERIFY(layout_node_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->wrap_in_button_layout_tree_if_needed(
                *static_cast<DOM::Node*>(node_pointer),
                *static_cast<Layout::Node*>(layout_node_pointer)); },
        .clear_stale_descendants = [](void* builder_pointer, void* node_pointer) {
            VERIFY(builder_pointer);
            VERIFY(node_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            node.for_each_shadow_including_descendant([&](auto& descendant) {
                return builder.clear_stale_layout_and_paint_node(descendant, &node);
            }); },
        .push_layout_parent = [](void* builder_pointer, void* layout_node_pointer) {
            VERIFY(builder_pointer);
            VERIFY(layout_node_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->push_parent(as<NodeWithStyle>(*static_cast<Layout::Node*>(layout_node_pointer))); },
        .pop_layout_parent = [](void* builder_pointer) {
            VERIFY(builder_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->pop_parent(); },
        .ensure_replaced_children_wrapper = [](void* builder_pointer, void* layout_node_pointer) -> void* {
            VERIFY(builder_pointer);
            VERIFY(layout_node_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            auto& layout_node = *static_cast<Layout::Node*>(layout_node_pointer);
            if (!layout_node.first_child() || !layout_node.first_child()->is_anonymous()) {
                auto wrapper = as<NodeWithStyle>(layout_node).create_anonymous_wrapper();
                builder.current_parent().append_child(wrapper);
            }
            return layout_node.first_child().ptr(); },
        .top_layer_element_count = [](void* document_pointer) {
            VERIFY(document_pointer);
            return static_cast<DOM::Document*>(document_pointer)->top_layer_elements().size(); },
        .copy_top_layer_elements = [](void* document_pointer, void** output, size_t count) {
            VERIFY(document_pointer);
            VERIFY(output || count == 0);
            auto const& elements = static_cast<DOM::Document*>(document_pointer)->top_layer_elements();
            VERIFY(count == elements.size());
            size_t index = 0;
            for (auto const& element : elements)
                output[index++] = element.ptr(); },
        .rendered_in_top_layer = [](void* element_pointer) {
            VERIFY(element_pointer);
            return static_cast<DOM::Element*>(element_pointer)->rendered_in_top_layer(); },
        .has_unrendered_flat_tree_ancestor = [](void* element_pointer) {
            VERIFY(element_pointer);
            return element_has_an_unrendered_flat_tree_ancestor(*static_cast<DOM::Element*>(element_pointer)); },
        .clear_stale_top_layer_subtree = [](void* builder_pointer, void* element_pointer) {
            VERIFY(builder_pointer);
            VERIFY(element_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            element.for_each_shadow_including_inclusive_descendant([&](auto& node) {
                return builder.clear_stale_layout_and_paint_node(node, &element);
            }); },
        .quote_nesting_level = [](void* builder_pointer) {
            VERIFY(builder_pointer);
            return static_cast<TreeBuilder*>(builder_pointer)->quote_nesting_level(); },
        .set_quote_nesting_level = [](void* builder_pointer, u32 quote_nesting_level) {
            VERIFY(builder_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->set_quote_nesting_level(quote_nesting_level); },
        .clear_dom_update_flags = [](void* node_pointer) {
            VERIFY(node_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            node.set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
            node.set_child_needs_layout_tree_update(false); },
        .svg_pattern_content_element = [](void* pattern_pointer) -> void* {
            VERIFY(pattern_pointer);
            return const_cast<SVG::SVGPatternElement*>(static_cast<SVG::SVGPatternElement*>(pattern_pointer)->pattern_content_element().ptr()); },
        .register_svg_resource_reference = [](void* resource_pointer, void* graphics_element_pointer) {
            VERIFY(resource_pointer);
            VERIFY(graphics_element_pointer);
            static_cast<SVG::SVGElement*>(resource_pointer)->register_resource_box_referencing_element(
                {}, *static_cast<SVG::SVGGraphicsElement*>(graphics_element_pointer)); },
        .ancestor_count = [](void* builder_pointer) {
            VERIFY(builder_pointer);
            return static_cast<TreeBuilder*>(builder_pointer)->ancestor_count(); },
        .ancestor_at = [](void* builder_pointer, size_t index) -> void* {
            VERIFY(builder_pointer);
            return &static_cast<TreeBuilder*>(builder_pointer)->ancestor_at(index); },
        .layout_node_dom_node = [](void* layout_node_pointer) -> void* {
            VERIFY(layout_node_pointer);
            return static_cast<Layout::Node*>(layout_node_pointer)->dom_node(); },
        .set_context_layout_svg_mask_or_clip_path = [](void* context_pointer, bool layout_svg_mask_or_clip_path) {
            VERIFY(context_pointer);
            static_cast<Context*>(context_pointer)->layout_svg_mask_or_clip_path = layout_svg_mask_or_clip_path; },
        .set_context_layout_svg_pattern = [](void* context_pointer, bool layout_svg_pattern) {
            VERIFY(context_pointer);
            static_cast<Context*>(context_pointer)->layout_svg_pattern = layout_svg_pattern; },
        .principal_node_entry_facts = [](void*, void* node_pointer, void* context_pointer, bool must_create_subtree) -> RustFFI::FfiPrincipalNodeEntryFacts {
            VERIFY(node_pointer);
            VERIFY(context_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            auto& context = *static_cast<Context*>(context_pointer);
            // NB: Called during layout tree construction.
            auto* existing_layout_node = node.unsafe_layout_node();
            auto* element = as_if<DOM::Element>(node);
            return {
                .must_create_subtree = must_create_subtree,
                .needs_layout_tree_update = node.needs_layout_tree_update(),
                .document_needs_full_layout_tree_update = node.document().needs_full_layout_tree_update(),
                .is_document = node.is_document(),
                .has_layout_node = existing_layout_node != nullptr,
                .is_element = element != nullptr,
                .is_text = is<DOM::Text>(node),
                .rendered_in_top_layer = element && element->rendered_in_top_layer(),
                .context_layout_top_layer = context.layout_top_layer,
                .layout_node_is_attached = existing_layout_node && existing_layout_node->parent(),
                .is_svg_container = node.is_svg_container(),
                .requires_svg_container = node.requires_svg_container(),
                .context_has_svg_root = context.has_svg_root,
            }; },
        .request_top_layer_zone_rebuild = [](void* node_pointer) {
            VERIFY(node_pointer);
            static_cast<DOM::Node*>(node_pointer)->document().set_top_layer_needs_layout_zone_rebuild(); },
        .push_style_ancestor = [](void* element_pointer) {
            VERIFY(element_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            element.document().style_computer().push_ancestor(element); },
        .pop_style_ancestor = [](void* element_pointer) {
            VERIFY(element_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            element.document().style_computer().pop_ancestor(element); },
        .initialize_principal_frame = [](void* frame_pointer, void* node_pointer) {
            VERIFY(frame_pointer);
            VERIFY(node_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            // NB: Called during layout tree construction.
            frame.old_layout_node = node.unsafe_layout_node();
            frame.layout_node = nullptr;
            frame.computed_values = nullptr; },
        .prepare_principal_element = [](void* builder_pointer, void* frame_pointer, void* element_pointer, bool should_create_layout_node) -> RustFFI::FfiPrincipalDisplayFacts {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            if (should_create_layout_node) {
                // ::backdrop is a sibling of the element, not a child, so unlike other pseudo-elements, it is not
                // automatically discarded when the element's layout is recomputed.
                if (auto old_backdrop_node = element.pseudo_element_unsafe_layout_node(CSS::PseudoElement::Backdrop)) {
                    builder.m_layout_tree_update_escaped_rebuild_roots = true;
                    old_backdrop_node->remove();
                }
                element.clear_synthetic_pseudo_element_layout_nodes(Badge<TreeBuilder> {});
                update_style_if_needed_for_layout_tree_bypass_path(element);
            }
            frame.computed_values = element.computed_values();
            frame.display = frame.computed_values->display();
            return ffi_principal_display_facts(frame.display); },
        .create_principal_element_layout = [](void* builder_pointer, void* frame_pointer, void* element_pointer, void* context_pointer) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            VERIFY(context_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            frame.layout_node = builder.create_layout_node_for_element(
                *static_cast<DOM::Element*>(element_pointer),
                *static_cast<Context*>(context_pointer)); },
        .create_principal_document_layout = [](void* frame_pointer, void* document_pointer) {
            VERIFY(frame_pointer);
            VERIFY(document_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            auto& document = *static_cast<DOM::Document*>(document_pointer);
            frame.computed_values = document.style_computer().create_document_style();
            frame.display = frame.computed_values->display();
            frame.layout_node = make_ref_counted<Layout::Viewport>(document, frame.computed_values.release_nonnull()); },
        .create_principal_text_layout = [](void* frame_pointer, void* text_pointer) {
            VERIFY(frame_pointer);
            VERIFY(text_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            frame.layout_node = create_layout_node_for_text(*static_cast<DOM::Text*>(text_pointer));
            frame.display = CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow); },
        .reuse_principal_layout = [](void* frame_pointer, void* node_pointer) {
            VERIFY(frame_pointer);
            VERIFY(node_pointer);
            // NB: Called during layout tree construction.
            static_cast<PrincipalNodeFrame*>(frame_pointer)->layout_node = static_cast<DOM::Node*>(node_pointer)->unsafe_layout_node(); },
        .principal_layout_node = [](void* frame_pointer) -> void* {
            VERIFY(frame_pointer);
            return static_cast<PrincipalNodeFrame*>(frame_pointer)->layout_node.ptr(); },
        .attach_principal_style_resources = [](void* frame_pointer) {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            as<NodeWithStyle>(*frame.layout_node).attach_style_resources(); },
        .principal_layout_facts = [](void* frame_pointer) -> RustFFI::FfiPrincipalLayoutFacts {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            return {
                .is_replaced_element = frame.layout_node->is_replaced_element(),
                .display = ffi_principal_display_facts(frame.display),
            }; },
        .apply_replaced_display_adjustment = [](void* frame_pointer, RustFFI::FfiReplacedElementDisplayAdjustment adjustment) {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            if (adjustment == RustFFI::FfiReplacedElementDisplayAdjustment::Block)
                frame.display = CSS::Display::from_short(CSS::Display::Short::Block);
            else if (adjustment == RustFFI::FfiReplacedElementDisplayAdjustment::Inline)
                frame.display = CSS::Display::from_short(CSS::Display::Short::Inline);
            else
                VERIFY_NOT_REACHED();
            as<NodeWithStyle>(*frame.layout_node).set_display(frame.display); },
        .principal_placement_facts = [](void* builder_pointer, void* frame_pointer, void* node_pointer, void* context_pointer, bool must_create_subtree, bool should_create_layout_node) -> RustFFI::FfiPrincipalBoxPlacementFacts {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            VERIFY(node_pointer);
            VERIFY(context_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            auto& context = *static_cast<Context*>(context_pointer);
            VERIFY(frame.layout_node);
            auto* element = as_if<DOM::Element>(node);
            return {
                .must_create_subtree = must_create_subtree,
                .should_create_layout_node = should_create_layout_node,
                .has_old_layout_node = frame.old_layout_node != nullptr,
                .old_layout_node_is_attached = frame.old_layout_node && frame.old_layout_node->parent(),
                .old_and_new_layout_nodes_are_same = frame.old_layout_node == frame.layout_node,
                .has_current_rebuild_root = builder.m_current_rebuild_root != nullptr,
                .is_document = node.is_document(),
                .is_element = element != nullptr,
                .rendered_in_top_layer = element && element->rendered_in_top_layer(),
                .context_layout_top_layer = context.layout_top_layer,
                .layout_node_is_svg_box = frame.layout_node->is_svg_box(),
            }; },
        .start_principal_rebuild_root = [](void* builder_pointer, void* frame_pointer) -> void* {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            auto* prior_rebuild_root = builder.m_current_rebuild_root;
            builder.m_current_rebuild_root = frame.layout_node.ptr();
            builder.m_rebuilt_subtree_roots.append(frame.layout_node.ptr());
            return prior_rebuild_root; },
        .restore_principal_rebuild_root = [](void* builder_pointer, void* rebuild_root_pointer) {
            VERIFY(builder_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->m_current_rebuild_root = static_cast<Layout::Node*>(rebuild_root_pointer); },
        .mark_update_escaped_rebuild_roots = [](void* builder_pointer) {
            VERIFY(builder_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->m_layout_tree_update_escaped_rebuild_roots = true; },
        .create_principal_backdrop = [](void* builder_pointer, void* frame_pointer, void* element_pointer, bool may_replace_existing_layout_node) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            static_cast<TreeBuilder*>(builder_pointer)->create_backdrop_for_top_layer_element_if_needed(
                *static_cast<DOM::Element*>(element_pointer),
                frame.old_layout_node,
                may_replace_existing_layout_node); },
        .place_principal_layout = [](void* builder_pointer, void* frame_pointer, RustFFI::FfiPrincipalBoxPlacement placement) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            switch (placement) {
            case RustFFI::FfiPrincipalBoxPlacement::None:
                break;
            case RustFFI::FfiPrincipalBoxPlacement::DocumentRoot:
                builder.m_layout_root = frame.layout_node;
                break;
            case RustFFI::FfiPrincipalBoxPlacement::ReplaceExisting:
                VERIFY(frame.old_layout_node);
                transfer_saved_layout_state_to_replacement_box(*frame.old_layout_node, *frame.layout_node);
                frame.old_layout_node->prepare_subtree_for_detach_from_layout_tree();
                frame.old_layout_node->parent()->replace_child(*frame.layout_node, *frame.old_layout_node);
                break;
            case RustFFI::FfiPrincipalBoxPlacement::AppendSvg:
                builder.current_parent().append_child(*frame.layout_node);
                break;
            case RustFFI::FfiPrincipalBoxPlacement::NormalInsertion:
                builder.insert_node_into_inline_or_block_ancestor(*frame.layout_node, frame.display, AppendOrPrepend::Append);
                break;
            } },
        .clear_stale_inclusive_subtree = [](void* builder_pointer, void* node_pointer) {
            VERIFY(builder_pointer);
            VERIFY(node_pointer);
            auto& builder = *static_cast<TreeBuilder*>(builder_pointer);
            static_cast<DOM::Node*>(node_pointer)->for_each_shadow_including_inclusive_descendant([&](auto& node) {
                return builder.clear_stale_layout_and_paint_node(node);
            }); },
        .set_context_has_svg_root = [](void* context_pointer, bool has_svg_root) {
            VERIFY(context_pointer);
            static_cast<Context*>(context_pointer)->has_svg_root = has_svg_root; },
        .reset_style_ancestor_filter = [](void* document_pointer) {
            VERIFY(document_pointer);
            static_cast<DOM::Document*>(document_pointer)->style_computer().reset_ancestor_filter(); },
        .document_layout_node = [](void* document_pointer) -> void* {
            VERIFY(document_pointer);
            // NB: Called during layout tree construction.
            return static_cast<DOM::Document*>(document_pointer)->unsafe_layout_node(); },
        .fixup_tables = [](void* builder_pointer, void* root_pointer) {
            VERIFY(builder_pointer);
            VERIFY(root_pointer);
            auto callbacks = make_ffi_tree_builder_callbacks(static_cast<TreeBuilder*>(builder_pointer));
            RustFFI::rust_fixup_tables(&callbacks, root_pointer); },
        .layout_root = [](void* builder_pointer) -> void* {
            VERIFY(builder_pointer);
            return static_cast<TreeBuilder*>(builder_pointer)->m_layout_root.ptr(); },
    };
}

void TreeBuilder::clear_stale_layout_nodes_for_assigned_slottables(HTML::HTMLSlotElement& slot_element)
{
    // Assigned slottables are flat tree children of a slot, not DOM descendants, so subtree
    // cleanup of the slot does not reach them.
    for (auto const& slottable : slot_element.assigned_nodes_internal()) {
        slottable.visit([&](DOM::Node& slottable_root) {
            slottable_root.for_each_shadow_including_inclusive_descendant([&](auto& node) {
                return clear_stale_layout_and_paint_node(node, &slottable_root);
            });
        });
    }
}

// Elements inside a `display:none` subtree are skipped by `Document::update_style_recursively`,
// so a bypass path (top-layer iteration, slot projection, SVG mask/clip-path or pattern
// reference) may reach an element whose `needs_style_update` flag is still set or whose
// `computed_values` is null. Route through `update_style_for_element`, which seeds the style
// computer's ancestor filter so descendant-combinator selectors continue to match during the
// lazy re-cascade.
static void update_style_if_needed_for_layout_tree_bypass_path(DOM::Element& element)
{
    if (element.needs_style_update() || !element.computed_values()) {
        element.document().update_style_for_element({ element });
        element.set_needs_style_update(false);
    }
}

RefPtr<Layout::Node> TreeBuilder::create_layout_node_for_element(DOM::Element& element, Context& context) const
{
    auto& document = element.document();
    NonnullRefPtr<CSS::ComputedValues const> computed_values = *element.computed_values();

    if (auto content_replacement = create_content_replacement_if_needed(element, computed_values))
        return content_replacement;

    if (context.layout_svg_mask_or_clip_path) {
        RefPtr<Layout::Node> layout_node;
        if (is<SVG::SVGMaskElement>(element))
            layout_node = make_ref_counted<Layout::SVGMaskBox>(document, static_cast<SVG::SVGMaskElement&>(element), move(computed_values));
        else if (is<SVG::SVGClipPathElement>(element))
            layout_node = make_ref_counted<Layout::SVGClipBox>(document, static_cast<SVG::SVGClipPathElement&>(element), move(computed_values));
        else
            VERIFY_NOT_REACHED();
        // Only layout direct uses of SVG masks/clipPaths.
        context.layout_svg_mask_or_clip_path = false;
        return layout_node;
    }

    if (context.layout_svg_pattern) {
        context.layout_svg_pattern = false;
        return make_ref_counted<Layout::SVGPatternBox>(document, as<SVG::SVGPatternElement>(element), move(computed_values));
    }

    return element.create_layout_node(move(computed_values));
}

static RefPtr<Layout::Node> create_layout_node_for_text(DOM::Text& text_node)
{
    auto& document = text_node.document();
    RefPtr<Layout::Node> layout_node = make_ref_counted<Layout::TextNode>(document, text_node);
    if (auto* style_parent = display_contents_style_parent_for_text_node(text_node); style_parent && display_contents_text_needs_style_wrapper(text_node, *style_parent)) {
        auto wrapper = make_ref_counted<Layout::InlineNode>(document, nullptr, style_parent->computed_values().release_nonnull());
        wrapper->attach_style_resources();
        wrapper->set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
        wrapper->set_children_are_inline(true);
        wrapper->append_child(*layout_node);
        return wrapper;
    }
    return layout_node;
}

// Each element rendered in the top layer has a ::backdrop pseudo-element, for which it is the
// originating element. When the element's box replaces an existing one in place, the ::backdrop
// box must be inserted before the old box so it ends up behind the element; otherwise it is
// appended before the element's own box is.
void TreeBuilder::create_backdrop_for_top_layer_element_if_needed(DOM::Element& element, Layout::Node* old_layout_node, bool may_replace_existing_layout_node)
{
    if (may_replace_existing_layout_node) {
        if (auto backdrop_node = create_pseudo_element_if_needed(element, CSS::PseudoElement::Backdrop, {})) {
            // The ::backdrop box is a fresh sibling of the rebuild root, outside it.
            note_tree_restructuring_at(*old_layout_node->parent());
            old_layout_node->parent()->insert_before(*backdrop_node, old_layout_node);
        }
    } else {
        (void)create_pseudo_element_if_needed(element, CSS::PseudoElement::Backdrop, AppendOrPrepend::Append);
    }
}

void TreeBuilder::update_layout_tree(DOM::Node& dom_node, TreeBuilder::Context& context, MustCreateSubtree must_create_subtree)
{
    PrincipalNodeFrame frame;
    auto callbacks = make_ffi_dom_tree_builder_callbacks();
    RustFFI::rust_update_layout_tree(
        &callbacks,
        &frame,
        &dom_node,
        &context,
        must_create_subtree == MustCreateSubtree::Yes);
}

// A full-height flex column that centers the button contents vertically.
static NonnullRefPtr<NodeWithStyle> create_button_flex_wrapper(NodeWithStyle& parent)
{
    auto flex_wrapper = parent.create_anonymous_wrapper();
    flex_wrapper->modify_computed_values([](auto& values) {
        values.set_display(CSS::Display { CSS::DisplayOutside::Block, CSS::DisplayInside::Flex });
        values.set_justify_content(CSS::JustifyContent::Center);
        values.set_flex_direction(CSS::FlexDirection::Column);
        values.set_height(CSS::Size::make_percentage(CSS::Percentage(100)));
    });
    return flex_wrapper;
}

// Let percentage-sized descendants shrink to fixed-height buttons instead of the flex
// item's automatic minimum size.
static NonnullRefPtr<NodeWithStyle> create_button_content_box_wrapper(NodeWithStyle& parent)
{
    auto content_box_wrapper = parent.create_anonymous_wrapper();
    content_box_wrapper->modify_computed_values([](auto& values) {
        values.set_min_height(CSS::Size::make_px(CSSPixels(0)));
    });
    return content_box_wrapper;
}

void TreeBuilder::wrap_in_button_layout_tree_if_needed(DOM::Node& dom_node, Layout::Node& layout_node)
{
    auto const* html_element = as_if<HTML::HTMLElement>(dom_node);
    auto callbacks = make_ffi_tree_builder_callbacks(this);
    RustFFI::rust_wrap_button_contents_if_needed(&callbacks, &layout_node, html_element && html_element->uses_button_layout());
}

RefPtr<Layout::Node> TreeBuilder::build(DOM::Node& dom_node)
{
    Context context;
    PrincipalNodeFrame frame;
    auto callbacks = make_ffi_dom_tree_builder_callbacks();
    return static_cast<Layout::Node*>(RustFFI::rust_build_layout_tree(&callbacks, &frame, &dom_node, &context));
}

static RustFFI::FfiTableDisplay ffi_table_display(CSS::Display display)
{
    if (display.is_table_inside())
        return RustFFI::FfiTableDisplay::TableRoot;
    if (display.is_table_row_group())
        return RustFFI::FfiTableDisplay::TableRowGroup;
    if (display.is_table_header_group())
        return RustFFI::FfiTableDisplay::TableHeaderGroup;
    if (display.is_table_footer_group())
        return RustFFI::FfiTableDisplay::TableFooterGroup;
    if (display.is_table_column_group())
        return RustFFI::FfiTableDisplay::TableColumnGroup;
    if (display.is_table_column())
        return RustFFI::FfiTableDisplay::TableColumn;
    if (display.is_table_row())
        return RustFFI::FfiTableDisplay::TableRow;
    if (display.is_table_cell())
        return RustFFI::FfiTableDisplay::TableCell;
    if (display.is_table_caption())
        return RustFFI::FfiTableDisplay::TableCaption;
    return RustFFI::FfiTableDisplay::Other;
}

static RustFFI::FfiLayoutNodeFacts ffi_layout_node_facts(void*, void* node_pointer)
{
    VERIFY(node_pointer);
    auto& node = *static_cast<Node*>(node_pointer);
    auto const* node_with_style = as_if<NodeWithStyle>(node);
    auto const* text_node = as_if<TextNode>(node);
    return {
        .current_display = node_with_style ? ffi_table_display(node_with_style->display()) : RustFFI::FfiTableDisplay::Other,
        .display_before_box_type_transformation = node_with_style ? ffi_table_display(node_with_style->display_before_box_type_transformation()) : RustFFI::FfiTableDisplay::Other,
        .is_box = is<Box>(node),
        .has_style = node.has_style(),
        .is_anonymous = node.is_anonymous(),
        .is_block_container = node.is_block_container(),
        .children_are_inline = node.children_are_inline(),
        .is_out_of_flow = node.is_out_of_flow(),
        .is_text = text_node != nullptr,
        .text_is_ascii_whitespace = text_node && text_node->text_for_rendering().is_ascii_whitespace(),
        .is_inline_outside = node_with_style && node_with_style->display().is_inline_outside(),
        .is_table_wrapper = node.is_table_wrapper(),
        .has_been_wrapped_in_table_wrapper = node.has_been_wrapped_in_table_wrapper(),
        .has_replaced_element_table_display_adjustment = node_with_style && node_with_style->has_replaced_element_table_display_adjustment(),
        .is_inline = node.is_inline(),
        .is_in_flow = node.is_in_flow(),
        .is_inline_node = is<InlineNode>(node),
        .is_field_set_box = is<FieldSetBox>(node),
        .is_svg_foreign_object_box = node.is_svg_foreign_object_box(),
        .is_svg_box = node.is_svg_box(),
        .is_svg_svg_box = node.is_svg_svg_box(),
        .is_generated_for_pseudo_element = node.is_generated_for_pseudo_element(),
        .display_is_flow_inside = node_with_style && node_with_style->display().is_flow_inside(),
        .display_is_flex_inside = node_with_style && node_with_style->display().is_flex_inside(),
        .display_is_grid_inside = node_with_style && node_with_style->display().is_grid_inside(),
        .is_list_item_marker_box = is<ListItemMarkerBox>(node),
        .is_generated_for_marker = node.generated_for_pseudo_element() == CSS::PseudoElement::Marker,
        .is_fragmented_inline = node.is_fragmented_inline(),
        .has_first_letter_style = [&] {
            auto* dom_element = as_if<DOM::Element>(node.dom_node());
            return dom_element && dom_element->computed_values(CSS::PseudoElement::FirstLetter);
        }(),
        .has_rendered_legend = is<FieldSetBox>(node) && static_cast<FieldSetBox&>(node).rendered_legend(),
    };
}

struct FfiFirstLetterTextContext {
    Utf16View text;
    NonnullOwnPtr<Unicode::Segmenter> grapheme_segmenter;
};

static size_t ffi_first_letter_code_unit_length(void* context_pointer)
{
    VERIFY(context_pointer);
    return static_cast<FfiFirstLetterTextContext*>(context_pointer)->text.length_in_code_units();
}

static u32 ffi_first_letter_code_point_at(void* context_pointer, size_t index)
{
    VERIFY(context_pointer);
    auto& context = *static_cast<FfiFirstLetterTextContext*>(context_pointer);
    VERIFY(index < context.text.length_in_code_units());
    return context.text.code_point_at(index);
}

static size_t ffi_first_letter_next_grapheme_boundary(void* context_pointer, size_t index)
{
    VERIFY(context_pointer);
    auto& context = *static_cast<FfiFirstLetterTextContext*>(context_pointer);
    VERIFY(index <= context.text.length_in_code_units());
    return context.grapheme_segmenter->next_boundary(index).value_or(context.text.length_in_code_units());
}

static RustFFI::FfiFirstLetterCodePointFacts ffi_first_letter_code_point_facts(void*, u32 code_point)
{
    static auto const ps = Unicode::general_category_from_string("Ps"sv).value();
    static auto const pd = Unicode::general_category_from_string("Pd"sv).value();
    return {
        .is_space_separator = Unicode::code_point_has_space_separator_general_category(code_point),
        .is_punctuation = Unicode::code_point_has_punctuation_general_category(code_point),
        .is_letter = Unicode::code_point_has_letter_general_category(code_point),
        .is_number = Unicode::code_point_has_number_general_category(code_point),
        .is_symbol = Unicode::code_point_has_symbol_general_category(code_point),
        .is_open_punctuation = Unicode::code_point_has_general_category(code_point, ps),
        .is_dash_punctuation = Unicode::code_point_has_general_category(code_point, pd),
    };
}

static RustFFI::FfiFirstLetterTarget ffi_find_first_letter_in_text(void*, void* node_pointer)
{
    VERIFY(node_pointer);
    auto& text_node = as<TextNode>(*static_cast<Node*>(node_pointer));
    auto text = text_node.text().utf16_view();
    auto grapheme_segmenter = text_node.document().grapheme_segmenter().clone();
    grapheme_segmenter->set_segmented_text(text);
    FfiFirstLetterTextContext context { text, move(grapheme_segmenter) };
    RustFFI::FfiFirstLetterTextCallbacks callbacks {
        .context = &context,
        .code_unit_length = ffi_first_letter_code_unit_length,
        .code_point_at = ffi_first_letter_code_point_at,
        .next_grapheme_boundary = ffi_first_letter_next_grapheme_boundary,
        .code_point_facts = ffi_first_letter_code_point_facts,
    };

    auto const white_space_collapse = text_node.parent()->computed_values().white_space_collapse();
    auto const preserves_segment_breaks = first_is_one_of(white_space_collapse,
        CSS::WhiteSpaceCollapse::Preserve, CSS::WhiteSpaceCollapse::PreserveBreaks, CSS::WhiteSpaceCollapse::BreakSpaces);
    auto target = RustFFI::rust_find_first_letter_in_text(&callbacks, preserves_segment_breaks);
    if (target.found)
        target.text_node = &text_node;
    return target;
}

static void* ffi_layout_node_parent(void*, void* node_pointer)
{
    VERIFY(node_pointer);
    return static_cast<Node*>(node_pointer)->parent();
}

static void* ffi_layout_node_first_child(void*, void* node_pointer)
{
    VERIFY(node_pointer);
    return static_cast<Node*>(node_pointer)->first_child().ptr();
}

static void* ffi_layout_node_next_sibling(void*, void* node_pointer)
{
    VERIFY(node_pointer);
    return static_cast<Node*>(node_pointer)->next_sibling().ptr();
}

static void* ffi_layout_node_previous_sibling(void*, void* node_pointer)
{
    VERIFY(node_pointer);
    return static_cast<Node*>(node_pointer)->previous_sibling().ptr();
}

static void* ffi_layout_node_last_child(void*, void* node_pointer)
{
    VERIFY(node_pointer);
    return static_cast<Node*>(node_pointer)->last_child().ptr();
}

static Vector<NonnullRefPtr<Node>> retain_ffi_layout_nodes(void* const* node_pointers, size_t node_count)
{
    Vector<NonnullRefPtr<Node>> nodes;
    nodes.ensure_capacity(node_count);
    for (size_t index = 0; index < node_count; ++index) {
        VERIFY(node_pointers[index]);
        nodes.unchecked_append(*static_cast<Node*>(node_pointers[index]));
    }
    return nodes;
}

static void ffi_remove_layout_nodes(void*, void* const* node_pointers, size_t node_count)
{
    auto nodes = retain_ffi_layout_nodes(node_pointers, node_count);
    for (auto& node : nodes) {
        VERIFY(node->parent());
        node->parent()->remove_child(*node);
    }
}

static void ffi_wrap_in_anonymous_table_box(void*, void* const* node_pointers, size_t node_count, void* nearest_sibling_pointer, RustFFI::FfiAnonymousTableBoxKind kind)
{
    VERIFY(node_count > 0);
    auto sequence = retain_ffi_layout_nodes(node_pointers, node_count);
    auto& parent = *sequence.first()->parent();
    auto builder = CSS::ComputedValues::Builder::create_inheriting_from(parent.computed_values());
    switch (kind) {
    case RustFFI::FfiAnonymousTableBoxKind::TableRow:
        builder->set_display(CSS::Display { CSS::DisplayInternal::TableRow });
        break;
    case RustFFI::FfiAnonymousTableBoxKind::TableCell:
        builder->set_display(CSS::Display { CSS::DisplayInternal::TableCell });
        break;
    case RustFFI::FfiAnonymousTableBoxKind::Table:
        builder->set_display(CSS::Display::from_short(CSS::Display::Short::Table));
        break;
    case RustFFI::FfiAnonymousTableBoxKind::InlineTable:
        builder->set_display(CSS::Display::from_short(CSS::Display::Short::InlineTable));
        break;
    }

    auto wrapper = [&]() -> NonnullRefPtr<NodeWithStyle> {
        if (kind == RustFFI::FfiAnonymousTableBoxKind::TableCell)
            return make_ref_counted<BlockContainer>(parent.document(), nullptr, move(builder).build());
        return make_ref_counted<Box>(parent.document(), nullptr, move(builder).build());
    }();
    for (auto& child : sequence) {
        parent.remove_child(*child);
        wrapper->append_child(*child);
    }
    wrapper->set_children_are_inline(parent.children_are_inline());
    if (nearest_sibling_pointer)
        parent.insert_before(*wrapper, *static_cast<Node*>(nearest_sibling_pointer));
    else
        parent.append_child(*wrapper);
}

static NonnullRefPtr<CSS::ComputedValues const> table_wrapper_computed_values(Box& table_box)
{
    auto builder = CSS::ComputedValues::Builder::create_inheriting_from(table_box.computed_values());
    table_box.transfer_table_box_computed_values_to_wrapper_computed_values(builder);
    return move(builder).build();
}

static void ffi_update_existing_table_wrapper(void*, void* table_root_pointer, void* wrapper_pointer)
{
    VERIFY(table_root_pointer);
    VERIFY(wrapper_pointer);
    auto& table_box = as<Box>(*static_cast<Node*>(table_root_pointer));
    auto& wrapper = as<TableWrapper>(*static_cast<Node*>(wrapper_pointer));
    wrapper.set_computed_values(table_wrapper_computed_values(table_box));
}

static void ffi_wrap_table_root(void*, void* table_root_pointer, void* nearest_sibling_pointer)
{
    VERIFY(table_root_pointer);
    NonnullRefPtr table_box = as<Box>(*static_cast<Node*>(table_root_pointer));
    auto parent = table_box->parent();
    VERIFY(parent);
    auto wrapper = make_ref_counted<TableWrapper>(parent->document(), nullptr, table_wrapper_computed_values(*table_box));
    parent->remove_child(*table_box);
    wrapper->append_child(*table_box);
    if (nearest_sibling_pointer)
        parent->insert_before(*wrapper, *static_cast<Node*>(nearest_sibling_pointer));
    else
        parent->append_child(*wrapper);
    table_box->set_has_been_wrapped_in_table_wrapper(true);
}

static void* ffi_create_table_grid(void*, void* table_root_pointer)
{
    VERIFY(table_root_pointer);
    auto& table_box = as<Box>(*static_cast<Node*>(table_root_pointer));
    return new TableGrid(TableGrid::calculate_row_column_grid(table_box));
}

static void ffi_destroy_table_grid(void*, void* table_grid_pointer)
{
    delete static_cast<TableGrid*>(table_grid_pointer);
}

static size_t ffi_table_grid_column_count(void*, void* table_grid_pointer)
{
    VERIFY(table_grid_pointer);
    return static_cast<TableGrid*>(table_grid_pointer)->column_count();
}

static bool ffi_table_grid_is_occupied(void*, void* table_grid_pointer, size_t column_index, size_t row_index)
{
    VERIFY(table_grid_pointer);
    return static_cast<TableGrid*>(table_grid_pointer)->occupancy_grid().contains({ column_index, row_index });
}

static void ffi_append_missing_table_cell(void*, void* row_pointer)
{
    VERIFY(row_pointer);
    auto& row_box = as<Box>(*static_cast<Node*>(row_pointer));
    auto builder = CSS::ComputedValues::Builder::create_inheriting_from(row_box.computed_values());
    builder->set_display(CSS::Display { CSS::DisplayInternal::TableCell });
    // Ensure that the cell (with zero content height) will have the same height as the row by setting vertical-align to middle.
    builder->set_vertical_align(CSS::VerticalAlign::Middle);
    row_box.append_child(make_ref_counted<BlockContainer>(row_box.document(), nullptr, move(builder).build()));
}

static void* ffi_create_and_append_anonymous_wrapper(void*, void* parent_pointer)
{
    VERIFY(parent_pointer);
    auto& parent = as<NodeWithStyle>(*static_cast<Node*>(parent_pointer));
    auto wrapper = parent.create_anonymous_wrapper();
    parent.append_child(*wrapper);
    return wrapper.ptr();
}

static void ffi_wrap_children_in_anonymous(void*, void* parent_pointer, void* const* child_pointers, size_t child_count)
{
    VERIFY(parent_pointer);
    auto& parent = as<NodeWithStyle>(*static_cast<Node*>(parent_pointer));
    auto children = retain_ffi_layout_nodes(child_pointers, child_count);
    auto wrapper = parent.create_anonymous_wrapper();
    wrapper->set_children_are_inline(true);
    for (auto& child : children) {
        parent.remove_child(*child);
        wrapper->append_child(*child);
    }
    parent.set_children_are_inline(false);
    parent.append_child(*wrapper);
}

static void ffi_insert_child(void*, void* parent_pointer, void* child_pointer, RustFFI::FfiInsertionMode mode)
{
    VERIFY(parent_pointer);
    VERIFY(child_pointer);
    auto& parent = *static_cast<Node*>(parent_pointer);
    NonnullRefPtr child = *static_cast<Node*>(child_pointer);
    if (mode == RustFFI::FfiInsertionMode::Prepend)
        parent.prepend_child(*child);
    else
        parent.append_child(*child);
}

static void ffi_set_children_are_inline(void*, void* node_pointer, bool children_are_inline)
{
    VERIFY(node_pointer);
    static_cast<Node*>(node_pointer)->set_children_are_inline(children_are_inline);
}

static void ffi_note_tree_restructuring(void* context, void* node_pointer)
{
    VERIFY(context);
    VERIFY(node_pointer);
    static_cast<TreeBuilder*>(context)->note_tree_restructuring_at(*static_cast<Node*>(node_pointer));
}

static void ffi_wrap_button_contents(void*, void* layout_node_pointer)
{
    VERIFY(layout_node_pointer);
    auto& parent = as<NodeWithStyle>(*static_cast<Node*>(layout_node_pointer));

    // If the box does not overflow in the vertical axis, then it is centered vertically.
    // FIXME: Only apply alignment when box overflows
    auto flex_wrapper = create_button_flex_wrapper(parent);

    auto content_box_wrapper = create_button_content_box_wrapper(parent);
    content_box_wrapper->set_children_are_inline(parent.children_are_inline());

    Vector<NonnullRefPtr<Node>> sequence;
    for (auto child = parent.first_child(); child; child = child->next_sibling())
        sequence.append(*child);

    for (auto& node : sequence) {
        parent.remove_child(*node);
        content_box_wrapper->append_child(*node);
    }

    flex_wrapper->append_child(*content_box_wrapper);
    parent.append_child(*flex_wrapper);
    parent.set_children_are_inline(false);
}

static void ffi_wrap_fieldset_contents(void*, void* layout_node_pointer)
{
    VERIFY(layout_node_pointer);
    auto& fieldset_box = as<FieldSetBox>(*static_cast<Node*>(layout_node_pointer));
    auto legend = fieldset_box.rendered_legend();
    VERIFY(legend);
    auto wrapper = fieldset_box.create_anonymous_wrapper();
    wrapper->set_display(CSS::Display::from_short(CSS::Display::Short::FlowRoot));

    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    // The following properties are expected to inherit from the fieldset element:
    //     align-content, align-items, border-radius, column-count, column-fill, column-gap, column-rule,
    //     column-width, flex-direction, flex-wrap, grid (grid-auto-columns, grid-auto-flow, grid-auto-rows,
    //     grid-column-gap, grid-row-gap, grid-template-areas, grid-template-columns, grid-template-rows),
    //     justify-content, justify-items, overflow, padding, text-overflow, unicode-bidi
    // FIXME: Transfer all of these properties, not just overflow.
    wrapper->set_overflow(fieldset_box.computed_values().overflow_x(), fieldset_box.computed_values().overflow_y());
    fieldset_box.set_overflow(CSS::InitialValues::overflow(), CSS::InitialValues::overflow());

    for (auto child = fieldset_box.first_child(); child;) {
        auto next = child->next_sibling();
        if (child != legend) {
            fieldset_box.remove_child(*child);
            wrapper->append_child(*child);
        }
        child = next;
    }
    fieldset_box.append_child(*wrapper);
}

static RustFFI::FfiTreeBuilderCallbacks make_ffi_tree_builder_callbacks(TreeBuilder* tree_builder)
{
    return {
        .context = tree_builder,
        .layout_node_facts = ffi_layout_node_facts,
        .parent = ffi_layout_node_parent,
        .first_child = ffi_layout_node_first_child,
        .next_sibling = ffi_layout_node_next_sibling,
        .previous_sibling = ffi_layout_node_previous_sibling,
        .last_child = ffi_layout_node_last_child,
        .remove_nodes = ffi_remove_layout_nodes,
        .wrap_in_anonymous = ffi_wrap_in_anonymous_table_box,
        .update_existing_table_wrapper = ffi_update_existing_table_wrapper,
        .wrap_table_root = ffi_wrap_table_root,
        .create_table_grid = ffi_create_table_grid,
        .destroy_table_grid = ffi_destroy_table_grid,
        .table_grid_column_count = ffi_table_grid_column_count,
        .table_grid_is_occupied = ffi_table_grid_is_occupied,
        .append_missing_table_cell = ffi_append_missing_table_cell,
        .create_and_append_anonymous_wrapper = ffi_create_and_append_anonymous_wrapper,
        .wrap_children_in_anonymous = ffi_wrap_children_in_anonymous,
        .insert_child = ffi_insert_child,
        .set_children_are_inline = ffi_set_children_are_inline,
        .note_tree_restructuring = ffi_note_tree_restructuring,
        .find_first_letter_in_text = ffi_find_first_letter_in_text,
        .wrap_button_contents = ffi_wrap_button_contents,
        .wrap_fieldset_contents = ffi_wrap_fieldset_contents,
    };
}

// https://drafts.csswg.org/css-tables-3/#fixup-algorithm

}
