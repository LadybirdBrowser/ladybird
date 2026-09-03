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
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/GeneratedContent.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/Layout/BlockContainer.h>
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
#include <LibWeb/SVG/SVGSwitchElement.h>

namespace Web::Layout {

class LayoutTreeBuildBridge {
public:
    ~LayoutTreeBuildBridge();

    LayoutTreeBuildResult build(DOM::Node&);

    static void detach_top_layer_element_layout_subtree(DOM::Element&);

    struct FirstLetterTextContext;

private:
    struct PrincipalNodeFrameStorage;
    struct PseudoElementFrameStorage;
    static TraversalDecision clear_stale_layout_node(DOM::Node&, DOM::Node const* cleared_subtree_root = nullptr);
    static TraversalDecision clear_stale_layout_node_in_subtree(DOM::Node&, DOM::Node const& subtree_root, DOM::Node const* cleared_subtree_root = nullptr);

    RustFFI::FfiDomTreeBuilderCallbacks make_ffi_dom_tree_builder_callbacks();
    RustFFI::FfiPseudoTreeBuilderCallbacks make_ffi_pseudo_tree_builder_callbacks();
    RustFFI::FfiTreeBuilderCallbacks make_ffi_tree_builder_callbacks();

    static BlockContainer& create_list_item_marker(BlockContainer& list_box, CSS::LayoutStyle marker_style);
    static RustFFI::FfiFirstLetterNodes create_first_letter_nodes(DOM::Element&, RustFFI::FfiFirstLetterTarget);

    GC::Ptr<DOM::Document> m_document;
    Layout::Viewport* m_layout_root { nullptr };
    OwnPtr<PrincipalNodeFrameStorage> m_principal_frames;
    OwnPtr<PseudoElementFrameStorage> m_pseudo_element_frames;
    OwnPtr<FirstLetterTextContext> m_first_letter_text_context;

    Vector<Layout::Node*> m_rebuilt_subtree_roots;
    bool m_layout_tree_update_escaped_rebuild_roots { false };
    bool m_needs_another_build_pass { false };
};

void LayoutTreeBuilderAccess::clear_synthetic_pseudo_element_layout_nodes(DOM::Element& element)
{
    element.clear_synthetic_pseudo_element_layout_nodes({});
}

void LayoutTreeBuilderAccess::detach_layout_node(DOM::Node& node)
{
    node.detach_layout_node({});
}

void LayoutTreeBuilderAccess::register_svg_resource_reference(SVG::SVGElement& resource, DOM::Element& referencing_element)
{
    resource.register_resource_box_referencing_element({}, referencing_element);
}

void LayoutTreeBuilderAccess::set_synthetic_pseudo_element_node(DOM::Element& element, CSS::PseudoElement pseudo_element, Layout::NodeWithStyle* layout_node)
{
    element.set_synthetic_pseudo_element_node({}, pseudo_element, layout_node);
}

static RustFFI::FfiPrincipalDisplayFacts ffi_principal_display_facts(CSS::Display);
static void update_style_if_needed_for_layout_tree_bypass_path(DOM::Element&);
struct PrincipalNodeFrame;
static RustFFI::NodeSlotId create_layout_node_for_text(PrincipalNodeFrame&, DOM::Text&);

static bool may_reuse_layout_node_for_child_list_insertion(DOM::Node const& node)
{
    if (!node.may_reuse_layout_node_for_child_list_insertion())
        return false;

    auto const* element = as_if<DOM::Element>(node);
    auto const* layout_node = as_if<NodeWithStyle>(node.unsafe_layout_node());
    if (!element || !layout_node || element->shadow_root() || is<HTML::HTMLSlotElement>(*element))
        return false;

    auto collapsing_whitespace_can_be_inserted = [&](DOM::Text const& text) {
        enum class SiblingDirection {
            Previous,
            Next,
        };

        Node const* last_layout_child = nullptr;
        for (auto* layout_child = layout_node->first_child(); layout_child; layout_child = layout_child->next_sibling())
            last_layout_child = layout_child;
        auto const* trailing_inline_wrapper = last_layout_child && last_layout_child->is_anonymous() && last_layout_child->children_are_inline()
                && !last_layout_child->is_generated_for_pseudo_element()
            ? last_layout_child
            : nullptr;
        bool will_join_trailing_inline_wrapper = false;

        auto can_place_next_to_layout_node = [&](Node const* sibling_layout_node, SiblingDirection direction, Optional<CSS::PseudoElement> pseudo_element) {
            if (!sibling_layout_node)
                return true;
            if (auto const* node_with_style = as_if<NodeWithStyle>(*sibling_layout_node); node_with_style && node_with_style->is_out_of_flow()) {
                if (pseudo_element.has_value() || direction == SiblingDirection::Next)
                    return false;
            }

            while (sibling_layout_node->parent() && sibling_layout_node->parent() != layout_node)
                sibling_layout_node = sibling_layout_node->parent();
            if (sibling_layout_node->parent() != layout_node)
                return false;
            if (pseudo_element.has_value()) {
                // ::after cannot anchor a newly appended anonymous wrapper. In normal flow,
                // inline ::before content also has to remain in its existing inline run, while
                // blockified flex/grid pseudo-elements remain separate items.
                if (direction == SiblingDirection::Next)
                    return layout_node->children_are_inline();

                auto pseudo_style = element->computed_style(*pseudo_element);
                auto parent_display = layout_node->display();
                auto pseudo_belongs_to_inline_run = pseudo_style
                    && (pseudo_style->display().is_inline_outside() || pseudo_style->display().is_contents())
                    && !parent_display.is_flex_inside() && !parent_display.is_grid_inside();
                return layout_node->children_are_inline() || !pseudo_belongs_to_inline_run;
            }
            if (sibling_layout_node->is_anonymous()) {
                if (sibling_layout_node != trailing_inline_wrapper)
                    return false;
                will_join_trailing_inline_wrapper = true;
            }
            return true;
        };

        auto can_place_next_to_sibling = [&](DOM::Node const* sibling, SiblingDirection direction) {
            for (; sibling; sibling = direction == SiblingDirection::Next ? sibling->next_sibling() : sibling->previous_sibling()) {
                if (auto const* sibling_element = as_if<DOM::Element>(*sibling)) {
                    auto computed_style = sibling_element->computed_style();
                    if (computed_style && computed_style->display().is_contents())
                        return false;
                }

                auto const* sibling_layout_node = sibling->unsafe_layout_node();
                if (!sibling_layout_node)
                    continue;
                return can_place_next_to_layout_node(sibling_layout_node, direction, {});
            }

            auto pseudo_element = direction == SiblingDirection::Previous
                ? CSS::PseudoElement::Before
                : CSS::PseudoElement::After;
            return can_place_next_to_layout_node(element->pseudo_element_unsafe_layout_node(pseudo_element), direction, pseudo_element);
        };

        if (!can_place_next_to_sibling(text.previous_sibling(), SiblingDirection::Previous)
            || !can_place_next_to_sibling(text.next_sibling(), SiblingDirection::Next)) {
            return false;
        }

        // Incremental inline insertion always reuses a trailing anonymous inline wrapper. If the
        // whitespace belongs to a different run, only a full rebuild can place it correctly.
        return !trailing_inline_wrapper || will_join_trailing_inline_wrapper;
    };

    auto const* first_letter_owner = node.first_letter_owner_for_layout_subtree_from(node);

    bool has_pending_collapsing_whitespace_since_layout_node = false;
    bool pending_children_can_preserve_parent = true;
    for (auto const* child = node.first_child(); child; child = child->next_sibling()) {
        if (child->unsafe_layout_node()) {
            has_pending_collapsing_whitespace_since_layout_node = false;
            if (child->needs_layout_tree_update() || child->child_needs_layout_tree_update()) {
                pending_children_can_preserve_parent = false;
            }
            continue;
        }
        if (!child->needs_layout_tree_update())
            continue;
        if (auto const* text = as_if<DOM::Text>(*child); text && text->data().is_ascii_whitespace()
            && layout_node->white_space_collapse() == CSS::WhiteSpaceCollapse::Collapse
            && !first_letter_owner
            && collapsing_whitespace_can_be_inserted(*text)) {
            if (has_pending_collapsing_whitespace_since_layout_node) {
                pending_children_can_preserve_parent = false;
                break;
            }
            has_pending_collapsing_whitespace_since_layout_node = true;
            continue;
        }
        auto const* child_element = as_if<DOM::Element>(*child);
        if (!child_element) {
            pending_children_can_preserve_parent = false;
            break;
        }
        auto computed_style = child_element->computed_style();
        if (!computed_style || !computed_style->display().is_none()) {
            pending_children_can_preserve_parent = false;
            break;
        }
    }
    // An empty set means every insertion was canceled before layout. Moves are marked dirty at
    // their destination, so they still enter one of the rejection paths above.
    if (pending_children_can_preserve_parent)
        return true;

    auto parent_display = layout_node->display();
    auto parent_has_children = layout_node->has_children();
    auto parent_lays_out_flex_or_grid_children = parent_display.is_flex_inside() || parent_display.is_grid_inside();
    auto parent_lays_out_inline_children = (parent_display.is_flow_inside() || parent_display.is_flow_root_inside())
        && (layout_node->children_are_inline() || !parent_has_children);
    auto parent_lays_out_block_children = (parent_display.is_flow_inside() || parent_display.is_flow_root_inside())
        && !layout_node->children_are_inline();
    auto parent_lays_out_table_rows = parent_display.is_table_row_group()
        || parent_display.is_table_header_group()
        || parent_display.is_table_footer_group();
    if (!parent_lays_out_flex_or_grid_children && !parent_lays_out_inline_children && !parent_lays_out_block_children && !parent_lays_out_table_rows) {
        return false;
    }
    if (first_letter_owner)
        return false;

    if (parent_lays_out_table_rows) {
        enum class SiblingDirection {
            Previous,
            Next,
        };
        auto has_table_row_sibling = [&](DOM::Node const* sibling, SiblingDirection direction) {
            for (; sibling; sibling = direction == SiblingDirection::Next ? sibling->next_sibling() : sibling->previous_sibling()) {
                if (auto const* sibling_layout_node = sibling->unsafe_layout_node()) {
                    auto const* node_with_style = as_if<NodeWithStyle>(*sibling_layout_node);
                    return sibling_layout_node->parent() == layout_node && node_with_style && node_with_style->display().is_table_row();
                }

                if (auto const* sibling_element = as_if<DOM::Element>(*sibling)) {
                    auto computed_style = sibling_element->computed_style();
                    if (!computed_style)
                        return false;
                    if (!computed_style->display().is_none())
                        return sibling->needs_layout_tree_update() && computed_style->display().is_table_row();
                } else if (auto const* sibling_text = as_if<DOM::Text>(*sibling); sibling_text && !sibling_text->data().is_ascii_whitespace()) {
                    return false;
                }
            }
            return false;
        };

        // NB: Table fixup discards whitespace at the edge of a row group. Inserting a row outside
        //     that whitespace makes it interior, so only a rebuild can create its anonymous table-row box.
        for (auto const* child = node.first_child(); child; child = child->next_sibling()) {
            auto const* text = as_if<DOM::Text>(*child);
            if (!text || child->unsafe_layout_node() || child->needs_layout_tree_update() || !text->data().is_ascii_whitespace())
                continue;
            if (has_table_row_sibling(child->previous_sibling(), SiblingDirection::Previous)
                && has_table_row_sibling(child->next_sibling(), SiblingDirection::Next)) {
                return false;
            }
        }
    }

    bool will_insert_inline_child = false;
    bool will_insert_block_child = false;
    bool has_indirect_existing_child = false;
    for (auto const* child = node.first_child(); child; child = child->next_sibling()) {
        if (auto const* child_layout_node = child->unsafe_layout_node()) {
            if (child_layout_node->parent() != layout_node)
                has_indirect_existing_child = true;
            continue;
        }

        auto const* child_element = as_if<DOM::Element>(*child);
        if (!child_element) {
            if (child->needs_layout_tree_update() && is<DOM::Text>(*child))
                return false;
            continue;
        }

        auto computed_style = child_element->computed_style();
        if (!computed_style || computed_style->display().is_contents())
            return false;
        if (!child->needs_layout_tree_update() || computed_style->display().is_none())
            continue;
        if (CSS::subtree_affects_generated_content_state(*child_element))
            return false;
        auto child_display = computed_style->display();
        if (child_element->rendered_in_top_layer() || is<SVG::SVGElement>(*child_element))
            return false;
        if (parent_lays_out_flex_or_grid_children)
            continue;
        if (parent_lays_out_table_rows && child_display.is_table_row())
            continue;
        if (parent_lays_out_block_children && child_display.is_block_outside()) {
            will_insert_block_child = true;
            if (will_insert_inline_child)
                return false;
            continue;
        }
        if (parent_lays_out_inline_children && child_display.is_inline_outside()
            && (child_display.is_flow_root_inside() || child_display.is_flex_inside() || child_display.is_grid_inside())) {
            will_insert_inline_child = true;
            if (will_insert_block_child)
                return false;
            continue;
        }
        return false;
    }
    return !has_indirect_existing_child;
}

static size_t ffi_assigned_node_count(void* slot_element_pointer)
{
    VERIFY(slot_element_pointer);
    return static_cast<HTML::HTMLSlotElement*>(slot_element_pointer)->assigned_nodes_internal().size();
}

static void* ffi_assigned_node_at(void* slot_element_pointer, size_t index)
{
    VERIFY(slot_element_pointer);
    auto assigned_nodes = static_cast<HTML::HTMLSlotElement*>(slot_element_pointer)->assigned_nodes_internal();
    VERIFY(index < assigned_nodes.size());
    DOM::Node* node = nullptr;
    assigned_nodes[index].visit([&](auto& assigned_node) { node = assigned_node.ptr(); });
    return node;
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
        if (!m_image_client)
            return nullptr;
        return m_image_client->decoded_image_data();
    }

    virtual Optional<CSSPixels> intrinsic_width() const override { return natural_size().width; }
    virtual Optional<CSSPixels> intrinsic_height() const override { return natural_size().height; }
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override { return natural_size().aspect_ratio; }

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
        auto& remainder_slice = allocate_layout_node<TextSliceNode>(document, mutable_dom_text, Node::AttachToDOMNode::Yes, letter_end, full_length - letter_end);
        auto& first_letter_slice = allocate_layout_node<TextSliceNode>(document, mutable_dom_text, Node::AttachToDOMNode::No, 0, letter_end);
        remainder_slice.set_first_letter_slice(first_letter_slice);
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

    auto first_letter_values = element.computed_style(CSS::PseudoElement::FirstLetter);
    VERIFY(first_letter_values);
    auto display = first_letter_values->display();
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

BlockContainer& LayoutTreeBuildBridge::create_list_item_marker(BlockContainer& list_box, CSS::LayoutStyle marker_style)
{
    auto& list_item_marker = allocate_layout_node<BlockContainer>(list_box.document(), nullptr, move(marker_style), RustFFI::NodeKind::ListItemMarkerBox);
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
static CSS::ContentData resolve_normal_marker_content(DOM::AbstractElement& element_reference, BlockContainer const& list_box, BlockContainer const& marker)
{
    CSS::ContentData content;
    content.type = CSS::ContentData::Type::List;

    if (auto const* list_style_image = marker.list_style_image()) {
        content.data.append(NonnullRefPtr { const_cast<CSS::AbstractImageStyleValue&>(*list_style_image) });
        return content;
    }

    if (CSS::marker_text_depends_on_list_item_counter_value(list_box.list_style_type()))
        element_reference.element().document().did_render_list_item_counter_value(element_reference.element());

    auto counter_value = element_reference.ensure_counters_set().counter_value_for_use(CSS::list_item_counter_name(), element_reference);

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
    CSS::StyleRecordID style_record_identity;
    GC::Ptr<CSS::StyleComputer> style_record_owner;
    CSS::Display display;
    RefPtr<CSS::AbstractImageStyleValue const> replacement_image;
    BlockContainer* originating_list_box { nullptr };
    NodeWithStyle* layout_node { nullptr };
    CSS::ContentData resolved_content;
    Layout::Node* content_item { nullptr };
};

struct LayoutTreeBuildBridge::PseudoElementFrameStorage {
    Vector<NonnullOwnPtr<PseudoElementFrame>> frames;
    size_t active_frame_count { 0 };
};

struct LayoutTreeBuildBridge::FirstLetterTextContext {
    FirstLetterTextContext(Utf16View text, NonnullOwnPtr<Unicode::Segmenter> grapheme_segmenter)
        : text(text)
        , grapheme_segmenter(move(grapheme_segmenter))
    {
    }

    Utf16View text;
    NonnullOwnPtr<Unicode::Segmenter> grapheme_segmenter;
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
            auto computed_values = element.computed_style(pseudo_element);
            if (!computed_values) {
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
                    .marker_position_is_inside = false,
                };
            }
            frame.display = computed_values->display();
            auto const computed_content_type = computed_values->computed_content().type;
            frame.replacement_image = content_replacement_image(computed_values->computed_content());
            if (pseudo_element == CSS::PseudoElement::Marker)
                frame.originating_list_box = element.unsafe_layout_node()->is_list_item_box() ? static_cast<BlockContainer*>(element.unsafe_layout_node()) : nullptr;
            auto const normal_marker_has_content = frame.originating_list_box
                && (!frame.originating_list_box->list_style_type().has<Empty>() || frame.originating_list_box->list_style_image());
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
                .marker_position_is_inside = frame.originating_list_box
                    && frame.originating_list_box->list_style_position() == CSS::ListStylePosition::Inside,
            }; },
        .create_layout_node = [](void* builder_pointer, void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement, RustFFI::FfiPseudoElementDecision decision) -> RustFFI::NodeSlotId {
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
        .attach_style_resources = [](void* frame_pointer) {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            frame.layout_node->attach_style_resources(); },
        .apply_replaced_display_adjustment = [](void* frame_pointer, RustFFI::FfiReplacedElementDisplayAdjustment adjustment) {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            if (adjustment == RustFFI::FfiReplacedElementDisplayAdjustment::Block)
                frame.layout_node->set_display(CSS::Display::from_short(CSS::Display::Short::Block));
            else if (adjustment == RustFFI::FfiReplacedElementDisplayAdjustment::Inline)
                frame.layout_node->set_display(CSS::Display::from_short(CSS::Display::Short::Inline));
            else
                VERIFY_NOT_REACHED(); },
        .create_nested_list_marker = [](void* frame_pointer, void* element_pointer) -> RustFFI::NodeSlotId {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            VERIFY(frame.layout_node);
            auto marker_style = element.document().style_computer().materialize_style_record({ element, CSS::PseudoElement::Marker });
            auto& list_item_marker = create_list_item_marker(as<BlockContainer>(*frame.layout_node), move(marker_style));
            list_item_marker.attach_style_resources();
            list_item_marker.set_generated_for(CSS::PseudoElement::Marker, element);
            LayoutTreeBuilderAccess::set_synthetic_pseudo_element_node(element, CSS::PseudoElement::Marker, &list_item_marker);
            return Node::slot_id(&list_item_marker); },
        .create_nested_list_marker_content = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement originating_pseudo, void* marker_pointer) -> RustFFI::NodeSlotId {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            VERIFY(marker_pointer);
            auto& frame = *static_cast<PseudoElementFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto& list_box = as<BlockContainer>(*frame.layout_node);
            auto& list_item_marker = as<BlockContainer>(*static_cast<Node*>(marker_pointer));
            DOM::AbstractElement element_reference { element, css_pseudo_element(originating_pseudo) };
            auto content = resolve_normal_marker_content(element_reference, list_box, list_item_marker);
            auto& content_node = [&]() -> Node& {
                if (auto const* text = content.data.first().get_pointer<Utf16String>()) {
                    auto& text_node = allocate_layout_node<GeneratedTextNode>(list_box.document(), *text);
                    text_node.set_generated_for(CSS::PseudoElement::Marker, element);
                    return text_node;
                }
                auto& image = *content.data.first().get<NonnullRefPtr<CSS::AbstractImageStyleValue>>();
                auto& image_box = create_content_image_box(list_box.document(), nullptr, list_item_marker.copy_computed_values(), image);
                image_box.set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
                image_box.attach_style_resources();
                image_box.set_generated_for(CSS::PseudoElement::Marker, element);
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
            auto computed_values = element_reference.computed_style();
            VERIFY(computed_values);
            if (auto* marker = frame.layout_node->is_list_item_marker_box() ? static_cast<BlockContainer*>(frame.layout_node) : nullptr;
                marker && computed_values->computed_content().type == CSS::ComputedContentData::Type::Normal) {
                VERIFY(frame.originating_list_box);
                frame.resolved_content = resolve_normal_marker_content(element_reference, *frame.originating_list_box, *marker);
                frame.layout_node->set_content(frame.resolved_content);
                return {
                    .final_quote_nesting_level = initial_quote_nesting_level,
                    .content_is_list = true,
                    .content_item_count = frame.resolved_content.data.size(),
                };
            }
            auto [content, final_quote_nesting_level] = computed_values->resolved_content(element_reference, initial_quote_nesting_level, CSS::NotifyListItemCounterRendered::Yes);
            frame.resolved_content = move(content);
            frame.layout_node->set_content(frame.resolved_content);
            return {
                .final_quote_nesting_level = final_quote_nesting_level,
                .content_is_list = frame.resolved_content.type == CSS::ContentData::Type::List,
                .content_item_count = frame.resolved_content.data.size(),
            }; },
        .create_content_item = [](void* frame_pointer, void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo, size_t index) -> RustFFI::NodeSlotId {
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

static bool is_svg_resource_box(Node const& layout_node)
{
    return layout_node.is_svg_pattern_box() || layout_node.is_svg_mask_box() || layout_node.is_svg_clip_box();
}

TraversalDecision LayoutTreeBuildBridge::clear_stale_layout_node_in_subtree(DOM::Node& node, DOM::Node const& subtree_root, DOM::Node const* cleared_subtree_root)
{
    if (&node != &subtree_root) {
        auto const* element = as_if<DOM::Element>(node);
        if (element && element->rendered_in_top_layer())
            return TraversalDecision::SkipChildrenAndContinue;
    }
    return clear_stale_layout_node(node, cleared_subtree_root);
}

TraversalDecision LayoutTreeBuildBridge::clear_stale_layout_node(DOM::Node& node, DOM::Node const* cleared_subtree_root)
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
        if (RustFFI::rust_should_preserve_svg_resource_layout_node(
                &callbacks, layout_node->arena_handle(), Node::slot_id(layout_node), const_cast<DOM::Node*>(cleared_subtree_root)))
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
        .element_layout_node = [](void* element_pointer) -> RustFFI::NodeSlotId {
            VERIFY(element_pointer);
            // NB: Called at DOM mutation processing time, outside layout tree construction.
            return Node::slot_id(static_cast<DOM::Element*>(element_pointer)->unsafe_layout_node()); },
        .prepare_subtree_for_detach = [](void* layout_node_pointer) {
            VERIFY(layout_node_pointer);
            static_cast<Layout::Node*>(layout_node_pointer)->prepare_subtree_for_detach_from_layout_tree(); },
        .clear_stale_subtree = [](void* root_pointer) {
            VERIFY(root_pointer);
            auto& root = *static_cast<DOM::Node*>(root_pointer);
            root.for_each_shadow_including_inclusive_descendant([&](auto& node) {
                return clear_stale_layout_node_in_subtree(node, root, &root);
            }); },
        .slot_element = [](void* element_pointer) -> void* {
            VERIFY(element_pointer);
            return as_if<HTML::HTMLSlotElement>(*static_cast<DOM::Element*>(element_pointer)); },
        .assigned_node_count = ffi_assigned_node_count,
        .assigned_node_at = ffi_assigned_node_at,
    };
    RustFFI::rust_detach_top_layer_element_layout_subtree(
        &callbacks, element.document().layout_node_arena().handle(), &element);
}

struct PrincipalNodeFrame {
    Layout::Node* layout_node { nullptr };
    RefPtr<CSS::ComputedValues const> anonymous_computed_values;
    CSS::StyleRecordID style_record_identity;
    GC::Ptr<CSS::StyleComputer> style_record_owner;
};

struct LayoutTreeBuildBridge::PrincipalNodeFrameStorage {
    Vector<NonnullOwnPtr<PrincipalNodeFrame>> frames;
    size_t active_frame_count { 0 };
};

LayoutTreeBuildBridge::~LayoutTreeBuildBridge()
{
}

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

RustFFI::FfiDomTreeBuilderCallbacks LayoutTreeBuildBridge::make_ffi_dom_tree_builder_callbacks()
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
        .clear_dom_update_flags = [](void* node_pointer) {
            VERIFY(node_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            node.set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
            node.set_child_needs_layout_tree_update(false); },
        .assigned_node_count = ffi_assigned_node_count,
        .assigned_node_at = ffi_assigned_node_at,
        .is_svg_element = [](void* node_pointer) {
            VERIFY(node_pointer);
            return is<SVG::SVGElement>(*static_cast<DOM::Node*>(node_pointer)); },
        .clear_stale_layout_node = [](void* builder_pointer, void* node_pointer) {
            VERIFY(builder_pointer);
            VERIFY(node_pointer);
            (void)static_cast<LayoutTreeBuildBridge*>(builder_pointer)->clear_stale_layout_node(*static_cast<DOM::Node*>(node_pointer)); },
        .display_contents_facts = [](void*, void* element_pointer) -> RustFFI::FfiDisplayContentsFacts {
            VERIFY(element_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto* slot_element = as_if<HTML::HTMLSlotElement>(element);
            auto shadow_root = element.shadow_root();
            return {
                .rendered_in_top_layer = element.rendered_in_top_layer(),
                .content_visibility_hidden = static_cast<CSS::ContentVisibility>(element.style_group<CSS::ComputedValues::InheritedBoxValues>()->content_visibility) == CSS::ContentVisibility::Hidden,
                .should_layout_dom_children = slot_element ? slot_element->assigned_nodes_internal().is_empty() && element.has_children() : element.has_children(),
                .child_needs_layout_tree_update = element.child_needs_layout_tree_update(),
                .dom_children_parent = static_cast<DOM::ParentNode*>(&element),
                .shadow_root = shadow_root ? static_cast<DOM::ParentNode*>(shadow_root.ptr()) : nullptr,
                .slot_element = slot_element,
            };
        },
        .clear_stale_subtree = [](void* builder_pointer, void* root_pointer, RustFFI::FfiStaleSubtreeClearScope scope) {
            VERIFY(builder_pointer);
            VERIFY(root_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            auto& root = *static_cast<DOM::Node*>(root_pointer);
            auto const* cleared_subtree_root = scope == RustFFI::FfiStaleSubtreeClearScope::Inclusive ? nullptr : &root;
            if (scope == RustFFI::FfiStaleSubtreeClearScope::DescendantsBoundedToRoot) {
                root.for_each_shadow_including_descendant([&](auto& node) {
                    return builder.clear_stale_layout_node_in_subtree(node, root, cleared_subtree_root);
                });
            } else {
                root.for_each_shadow_including_inclusive_descendant([&](auto& node) {
                    return builder.clear_stale_layout_node_in_subtree(node, root, cleared_subtree_root);
                });
            } },
        .resolve_counters = [](void* element_pointer, RustFFI::FfiPseudoElement ffi_pseudo) {
            VERIFY(element_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            if (ffi_pseudo == RustFFI::FfiPseudoElement::None) {
                DOM::AbstractElement element_reference { element };
                CSS::resolve_counters(element_reference);
            } else {
                DOM::AbstractElement element_reference { element, css_pseudo_element(ffi_pseudo) };
                CSS::resolve_counters(element_reference);
            } },
        .principal_descendant_facts = [](void*, void* node_pointer, void* layout_node_pointer) -> RustFFI::FfiPrincipalDescendantFacts {
            VERIFY(node_pointer);
            VERIFY(layout_node_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
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
                .content_visibility_hidden = element && static_cast<CSS::ContentVisibility>(element->style_group<CSS::ComputedValues::InheritedBoxValues>()->content_visibility) == CSS::ContentVisibility::Hidden,
                .should_layout_dom_children = slot_element ? slot_element->assigned_nodes_internal().is_empty() && node.has_children() : node.has_children(),
                .child_needs_layout_tree_update = node.child_needs_layout_tree_update(),
                .is_svg_switch_element = is<SVG::SVGSwitchElement>(node),
                .is_document = node.is_document(),
                .dom_children_parent = parent_node,
                .shadow_root = shadow_root ? static_cast<DOM::ParentNode*>(shadow_root.ptr()) : nullptr,
                .slot_element = slot_element,
                .svg_graphics_element = graphics_element,
                .svg_mask = const_cast<SVG::SVGMaskElement*>(mask.ptr()),
                .svg_clip_path = const_cast<SVG::SVGClipPathElement*>(clip_path.ptr()),
                .svg_fill_pattern = const_cast<SVG::SVGPatternElement*>(fill_pattern.ptr()),
                .svg_stroke_pattern = const_cast<SVG::SVGPatternElement*>(stroke_pattern.ptr()),
            }; },
        .layout_node_has_first_letter_style = [](void* layout_node_pointer) {
            VERIFY(layout_node_pointer);
            auto* element = as_if<DOM::Element>(static_cast<Node*>(layout_node_pointer)->dom_node());
            return element && element->has_style(CSS::PseudoElement::FirstLetter); },
        .create_first_letter_nodes = [](void*, void* element_pointer, RustFFI::FfiFirstLetterTarget target) -> RustFFI::FfiFirstLetterNodes {
            VERIFY(element_pointer);
            return create_first_letter_nodes(*static_cast<DOM::Element*>(element_pointer), target); },
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
        .flat_tree_parent = [](void* node_pointer) -> void* {
            VERIFY(node_pointer);
            return static_cast<DOM::Node*>(node_pointer)->flat_tree_parent(); },
        .flat_tree_render_facts = [](void* node_pointer) -> RustFFI::FfiFlatTreeRenderFacts {
            VERIFY(node_pointer);
            auto* element = as_if<DOM::Element>(*static_cast<DOM::Node*>(node_pointer));
            auto computed_values = element ? element->computed_style() : CSS::ComputedStyleRecordView {};
            return {
                .is_element = element != nullptr,
                .has_computed_style = static_cast<bool>(computed_values),
                .display_is_none = computed_values && computed_values->display().is_none(),
            }; },
        .svg_pattern_content_element = [](void* pattern_pointer) -> void* {
            VERIFY(pattern_pointer);
            return const_cast<SVG::SVGPatternElement*>(static_cast<SVG::SVGPatternElement*>(pattern_pointer)->pattern_content_element().ptr()); },
        .register_svg_resource_reference = [](void* resource_pointer, void* graphics_element_pointer) {
            VERIFY(resource_pointer);
            VERIFY(graphics_element_pointer);
            LayoutTreeBuilderAccess::register_svg_resource_reference(
                *static_cast<SVG::SVGElement*>(resource_pointer),
                *static_cast<SVG::SVGGraphicsElement*>(graphics_element_pointer)); },
        .element_layout_node = [](void* element_pointer) -> RustFFI::NodeSlotId {
            VERIFY(element_pointer);
            // NB: Called during layout tree construction.
            return Node::slot_id(static_cast<DOM::Element*>(element_pointer)->unsafe_layout_node()); },
        .dom_node_layout_node = [](void* node_pointer) -> RustFFI::NodeSlotId {
            VERIFY(node_pointer);
            return Node::slot_id(static_cast<DOM::Node*>(node_pointer)->unsafe_layout_node()); },
        .layout_node_dom_element = [](void* layout_node_pointer) -> void* {
            VERIFY(layout_node_pointer);
            auto* dom_node = static_cast<Layout::Node*>(layout_node_pointer)->dom_node();
            return dom_node ? as_if<DOM::Element>(*dom_node) : nullptr; },
        .element_pseudo_layout_node = [](void* element_pointer, RustFFI::FfiPseudoElement pseudo_element) -> RustFFI::NodeSlotId {
            VERIFY(element_pointer);
            return Node::slot_id(static_cast<DOM::Element*>(element_pointer)->pseudo_element_unsafe_layout_node(css_pseudo_element(pseudo_element))); },
        .principal_node_entry_facts = [](void*, void* node_pointer, bool must_create_subtree) -> RustFFI::FfiPrincipalNodeEntryFacts {
            VERIFY(node_pointer);
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            // NB: Called during layout tree construction.
            auto* existing_layout_node = node.unsafe_layout_node();
            auto* element = as_if<DOM::Element>(node);
            return {
                .must_create_subtree = must_create_subtree,
                .needs_layout_tree_update = node.needs_layout_tree_update(),
                .may_reuse_layout_node_for_child_list_insertion = may_reuse_layout_node_for_child_list_insertion(node),
                .document_needs_full_layout_tree_update = node.document().needs_full_layout_tree_update(),
                .is_document = node.is_document(),
                .has_layout_node = existing_layout_node != nullptr,
                .is_element = element != nullptr,
                .is_text = is<DOM::Text>(node),
                .rendered_in_top_layer = element && element->rendered_in_top_layer(),
                .layout_node_is_attached = existing_layout_node && existing_layout_node->has_parent(),
                .is_svg_container = node.is_svg_container(),
                .requires_svg_container = node.requires_svg_container(),
                .is_svg_foreign_object = node.is_svg_foreign_object_element(),
            }; },
        .request_top_layer_zone_rebuild = [](void* node_pointer) {
            VERIFY(node_pointer);
            static_cast<DOM::Node*>(node_pointer)->document().set_top_layer_needs_layout_zone_rebuild(); },
        .request_layout_tree_rebuild = [](void* builder_pointer, void* element_pointer) {
            VERIFY(builder_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            builder.m_needs_another_build_pass = true;
            if (element_pointer) {
                static_cast<DOM::Element*>(element_pointer)->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::PseudoElementBoxEscapedRebuildRoot);
                return;
            }
            VERIFY(builder.m_layout_root);
            builder.m_layout_root->document().set_needs_full_layout_tree_update(true); },
        .push_principal_frame = [](void* builder_pointer, void* node_pointer) -> RustFFI::FfiPrincipalNodeFrame {
            VERIFY(builder_pointer);
            VERIFY(node_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            if (!builder.m_principal_frames)
                builder.m_principal_frames = make<PrincipalNodeFrameStorage>();
            auto& storage = *builder.m_principal_frames;
            if (storage.active_frame_count == storage.frames.size())
                storage.frames.append(make<PrincipalNodeFrame>());
            auto& frame = *storage.frames[storage.active_frame_count++];
            auto& node = *static_cast<DOM::Node*>(node_pointer);
            frame.layout_node = nullptr;
            frame.anonymous_computed_values = nullptr;
            VERIFY(!frame.style_record_owner);
            frame.style_record_identity = 0;
            // NB: Called during layout tree construction.
            return {
                .frame = &frame,
                .old_layout_node = Node::slot_id(node.unsafe_layout_node()),
            }; },
        .pop_principal_frame = [](void* builder_pointer, void* frame_pointer) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            VERIFY(builder.m_principal_frames);
            auto& storage = *builder.m_principal_frames;
            VERIFY(storage.active_frame_count > 0);
            VERIFY(storage.frames[storage.active_frame_count - 1].ptr() == frame_pointer);
            auto& frame = *storage.frames[storage.active_frame_count - 1];
            if (!!frame.style_record_identity) {
                VERIFY(frame.style_record_owner);
                frame.style_record_owner->unpin_style_record(frame.style_record_identity);
                frame.style_record_owner = nullptr;
            }
            frame.layout_node = nullptr;
            frame.anonymous_computed_values = nullptr;
            frame.style_record_identity = 0;
            --storage.active_frame_count; },
        .prepare_principal_element = [](void* builder_pointer, void* frame_pointer, void* element_pointer, bool should_create_layout_node) -> RustFFI::FfiPreparedPrincipalElementFacts {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            if (should_create_layout_node) {
                LayoutTreeBuilderAccess::clear_synthetic_pseudo_element_layout_nodes(element);
                update_style_if_needed_for_layout_tree_bypass_path(element);
            }
            frame.style_record_identity = element.style_record_identity();
            VERIFY(frame.style_record_identity);
            frame.style_record_owner = &element.document().style_computer();
            frame.style_record_owner->pin_style_record(frame.style_record_identity);
            auto computed_values = element.computed_style();
            VERIFY(computed_values);
            return {
                .display = ffi_principal_display_facts(computed_values->display()),
            }; },
        .principal_element_layout_facts = [](void* frame_pointer, void* element_pointer) -> RustFFI::FfiElementLayoutFacts {
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            auto computed_values = element.computed_style();
            VERIFY(computed_values);
            return {
                .has_content_replacement = content_replacement_image(computed_values->computed_content()) != nullptr,
                .is_svg_mask_element = is<SVG::SVGMaskElement>(element),
                .is_svg_clip_path_element = is<SVG::SVGClipPathElement>(element),
                .is_svg_pattern_element = is<SVG::SVGPatternElement>(element),
            }; },
        .create_principal_element_layout = [](void* builder_pointer, void* frame_pointer, void* element_pointer, RustFFI::FfiElementLayoutKind kind) -> RustFFI::NodeSlotId {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            VERIFY(element_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            auto& element = *static_cast<DOM::Element*>(element_pointer);
            VERIFY(frame.style_record_identity);
            auto computed_values = element.computed_style();
            VERIFY(computed_values);
            CSS::LayoutStyle style { frame.style_record_identity };
            switch (kind) {
            case RustFFI::FfiElementLayoutKind::ContentReplacement: {
                auto computed_content = computed_values->computed_content();
                auto const* replacement_image = content_replacement_image(computed_content);
                VERIFY(replacement_image);
                frame.layout_node = &create_content_image_box(element.document(), element, style, const_cast<CSS::AbstractImageStyleValue&>(*replacement_image));
                break;
            }
            case RustFFI::FfiElementLayoutKind::SvgMask:
                frame.layout_node = &allocate_layout_node<Layout::Box>(element.document(), element, style, RustFFI::NodeKind::SVGMaskBox);
                break;
            case RustFFI::FfiElementLayoutKind::SvgClipPath:
                frame.layout_node = &allocate_layout_node<Layout::Box>(element.document(), element, style, RustFFI::NodeKind::SVGClipBox);
                break;
            case RustFFI::FfiElementLayoutKind::SvgPattern:
                frame.layout_node = &allocate_layout_node<Layout::Box>(element.document(), element, style, RustFFI::NodeKind::SVGPatternBox);
                break;
            case RustFFI::FfiElementLayoutKind::Normal:
                frame.layout_node = element.create_layout_node(style);
                break;
            }
            return Node::slot_id(frame.layout_node); },
        .create_principal_document_layout = [](void* frame_pointer, void* document_pointer) -> RustFFI::NodeSlotId {
            VERIFY(frame_pointer);
            VERIFY(document_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            auto& document = *static_cast<DOM::Document*>(document_pointer);
            frame.anonymous_computed_values = document.style_computer().create_document_style();
            frame.layout_node = &allocate_layout_node<Layout::Viewport>(document, frame.anonymous_computed_values.release_nonnull());
            return Node::slot_id(frame.layout_node); },
        .principal_text_layout_facts = [](void* text_pointer) -> RustFFI::FfiTextLayoutFacts {
            VERIFY(text_pointer);
            auto& text = *static_cast<DOM::Text*>(text_pointer);
            auto* style_parent = as_if<DOM::Element>(text.flat_tree_parent());
            auto style_parent_values = style_parent ? style_parent->computed_style() : CSS::ComputedStyleRecordView {};
            return {
                .has_style_parent = static_cast<bool>(style_parent_values),
                .parent_display_is_contents = style_parent_values && style_parent_values->display().is_contents(),
                .text_is_ascii_whitespace = text.data().is_ascii_whitespace(),
                .parent_collapses_whitespace = style_parent_values && first_is_one_of(style_parent_values->white_space_collapse(), CSS::WhiteSpaceCollapse::Collapse),
                .style_parent_style_record = style_parent ? style_parent->style_record_identity().value() : 0,
            }; },
        .create_principal_text_layout = [](void* frame_pointer, void* text_pointer) -> RustFFI::NodeSlotId {
            VERIFY(frame_pointer);
            VERIFY(text_pointer);
            return create_layout_node_for_text(*static_cast<PrincipalNodeFrame*>(frame_pointer), *static_cast<DOM::Text*>(text_pointer)); },
        .set_principal_layout_node = [](void* builder_pointer, void* frame_pointer, RustFFI::NodeSlotId slot) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            frame.layout_node = static_cast<Node*>(RustFFI::layout_arena_node_shell_if_live(builder.m_document->layout_node_arena().handle(), slot));
            VERIFY(frame.layout_node); },
        .reuse_principal_layout = [](void* frame_pointer, void* node_pointer) {
            VERIFY(frame_pointer);
            VERIFY(node_pointer);
            // NB: Called during layout tree construction.
            static_cast<PrincipalNodeFrame*>(frame_pointer)->layout_node = static_cast<DOM::Node*>(node_pointer)->unsafe_layout_node(); },
        .principal_layout_node = [](void* frame_pointer) -> RustFFI::NodeSlotId {
            VERIFY(frame_pointer);
            return Node::slot_id(static_cast<PrincipalNodeFrame*>(frame_pointer)->layout_node); },
        .attach_principal_style_resources = [](void* frame_pointer) {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            as<NodeWithStyle>(*frame.layout_node).attach_style_resources(); },
        .apply_replaced_display_adjustment = [](void* frame_pointer, RustFFI::FfiReplacedElementDisplayAdjustment adjustment) {
            VERIFY(frame_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            if (adjustment == RustFFI::FfiReplacedElementDisplayAdjustment::Block)
                as<NodeWithStyle>(*frame.layout_node).set_display(CSS::Display::from_short(CSS::Display::Short::Block));
            else if (adjustment == RustFFI::FfiReplacedElementDisplayAdjustment::Inline)
                as<NodeWithStyle>(*frame.layout_node).set_display(CSS::Display::from_short(CSS::Display::Short::Inline));
            else
                VERIFY_NOT_REACHED(); },
        .set_layout_root = [](void* builder_pointer, void* frame_pointer) {
            VERIFY(builder_pointer);
            VERIFY(frame_pointer);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            auto& frame = *static_cast<PrincipalNodeFrame*>(frame_pointer);
            VERIFY(frame.layout_node);
            builder.m_layout_root = &as<Layout::Viewport>(*frame.layout_node); },
        .document_layout_node = [](void* document_pointer) -> RustFFI::NodeSlotId {
            VERIFY(document_pointer);
            // NB: Called during layout tree construction.
            return Node::slot_id(static_cast<DOM::Document*>(document_pointer)->unsafe_layout_node()); },
        .report_rebuild_outcome = [](void* builder_pointer, void* const* rebuilt_root_pointers, size_t rebuilt_root_count, bool layout_tree_update_escaped_rebuild_roots) {
            VERIFY(builder_pointer);
            VERIFY(rebuilt_root_pointers || rebuilt_root_count == 0);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            builder.m_rebuilt_subtree_roots.ensure_capacity(rebuilt_root_count);
            for (size_t index = 0; index < rebuilt_root_count; ++index)
                builder.m_rebuilt_subtree_roots.unchecked_append(static_cast<Layout::Node*>(rebuilt_root_pointers[index]));
            builder.m_layout_tree_update_escaped_rebuild_roots = layout_tree_update_escaped_rebuild_roots; },
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

static RustFFI::NodeSlotId create_layout_node_for_text(PrincipalNodeFrame& frame, DOM::Text& text_node)
{
    frame.layout_node = &allocate_layout_node<Layout::TextNode>(text_node.document(), text_node);
    return Node::slot_id(frame.layout_node);
}

LayoutTreeBuildResult LayoutTreeBuildBridge::build(DOM::Node& dom_node)
{
    m_document = &dom_node.document();
    auto callbacks = make_ffi_dom_tree_builder_callbacks();
    RustFFI::rust_build_layout_tree(&callbacks, dom_node.document().layout_node_arena().handle(), &dom_node);
    return {
        .root = m_layout_root,
        .rebuilt_subtree_roots = move(m_rebuilt_subtree_roots),
        .layout_tree_update_escaped_rebuild_roots = m_layout_tree_update_escaped_rebuild_roots,
        .needs_another_build_pass = m_needs_another_build_pass,
    };
}

LayoutTreeBuildResult build_layout_tree(DOM::Node& dom_node)
{
    LayoutTreeBuildBridge bridge;
    return bridge.build(dom_node);
}

void detach_top_layer_element_layout_subtree(DOM::Element& element)
{
    LayoutTreeBuildBridge::detach_top_layer_element_layout_subtree(element);
}

static size_t ffi_first_letter_code_unit_length(void* context_pointer)
{
    VERIFY(context_pointer);
    return static_cast<LayoutTreeBuildBridge::FirstLetterTextContext*>(context_pointer)->text.length_in_code_units();
}

static u32 ffi_first_letter_code_point_at(void* context_pointer, size_t index)
{
    VERIFY(context_pointer);
    auto& context = *static_cast<LayoutTreeBuildBridge::FirstLetterTextContext*>(context_pointer);
    VERIFY(index < context.text.length_in_code_units());
    return context.text.code_point_at(index);
}

static size_t ffi_first_letter_next_grapheme_boundary(void* context_pointer, size_t index)
{
    VERIFY(context_pointer);
    auto& context = *static_cast<LayoutTreeBuildBridge::FirstLetterTextContext*>(context_pointer);
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

RustFFI::FfiTreeBuilderCallbacks LayoutTreeBuildBridge::make_ffi_tree_builder_callbacks()
{
    return {
        .context = this,
        .take_fieldset_overflow_for_content_wrapper = [](void*, void* fieldset_box_pointer) -> RustFFI::FfiAnonymousStyleOverrides {
            VERIFY(fieldset_box_pointer);
            auto& fieldset_box = as<BlockContainer>(*static_cast<Node*>(fieldset_box_pointer));
            // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
            // The following properties are expected to inherit from the fieldset element:
            //     align-content, align-items, border-radius, column-count, column-fill, column-gap, column-rule,
            //     column-width, flex-direction, flex-wrap, grid (grid-auto-columns, grid-auto-flow, grid-auto-rows,
            //     grid-column-gap, grid-row-gap, grid-template-areas, grid-template-columns, grid-template-rows),
            //     justify-content, justify-items, overflow, padding, text-overflow, unicode-bidi
            // FIXME: Transfer all of these properties, not just overflow.
            RustFFI::FfiAnonymousStyleOverrides overrides {
                .inline_block_wrapper = false,
                .overflow_x = to_underlying(fieldset_box.overflow_x()),
                .overflow_y = to_underlying(fieldset_box.overflow_y()),
            };
            fieldset_box.set_overflow(CSS::InitialValues::overflow(), CSS::InitialValues::overflow());
            return overrides; },
        .prepare_subtree_for_detach = [](void*, void* layout_node_pointer) {
            VERIFY(layout_node_pointer);
            static_cast<Node*>(layout_node_pointer)->prepare_subtree_for_detach_from_layout_tree(); },
        .text_is_ascii_whitespace = [](void*, void* node_pointer) {
            VERIFY(node_pointer);
            return as<TextNode>(*static_cast<Node*>(node_pointer)).text_for_rendering().is_ascii_whitespace(); },
        .prepare_first_letter_text = [](void* builder_pointer, void* node_pointer, RustFFI::FfiFirstLetterTextCallbacks* callbacks) {
            VERIFY(builder_pointer);
            VERIFY(node_pointer);
            VERIFY(callbacks);
            auto& builder = *static_cast<LayoutTreeBuildBridge*>(builder_pointer);
            auto& text_node = as<TextNode>(*static_cast<Node*>(node_pointer));
            auto text = text_node.text().utf16_view();
            auto grapheme_segmenter = text_node.document().grapheme_segmenter().clone();
            grapheme_segmenter->set_segmented_text(text);
            builder.m_first_letter_text_context = make<FirstLetterTextContext>(text, move(grapheme_segmenter));
            *callbacks = {
                .context = builder.m_first_letter_text_context.ptr(),
                .code_unit_length = ffi_first_letter_code_unit_length,
                .code_point_at = ffi_first_letter_code_point_at,
                .next_grapheme_boundary = ffi_first_letter_next_grapheme_boundary,
                .code_point_facts = ffi_first_letter_code_point_facts,
            };

            auto const white_space_collapse = text_node.parent()->white_space_collapse();
            return first_is_one_of(white_space_collapse,
                CSS::WhiteSpaceCollapse::Preserve, CSS::WhiteSpaceCollapse::PreserveBreaks, CSS::WhiteSpaceCollapse::BreakSpaces); },
    };
}

// https://drafts.csswg.org/css-tables-3/#fixup-algorithm

}
