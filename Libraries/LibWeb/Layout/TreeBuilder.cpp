/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Aziz B. Yesilyurt <abyesilyurt@gmail.com>
 * Copyright (c) 2025, Manuel Zahariev <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Optional.h>
#include <AK/TemporaryChange.h>
#include <AK/Utf16String.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
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
#include <LibWeb/Layout/Viewport.h>

namespace Web::Layout {

TreeBuilder::TreeBuilder() = default;

static bool has_inline_or_in_flow_block_children(Layout::Node const& layout_node)
{
    for (auto child = layout_node.first_child(); child; child = child->next_sibling()) {
        if (child->is_inline() || child->is_in_flow())
            return true;
    }
    return false;
}

static bool has_in_flow_block_children(Layout::Node const& layout_node)
{
    if (layout_node.children_are_inline())
        return false;
    for (auto child = layout_node.first_child(); child; child = child->next_sibling()) {
        if (child->is_inline())
            continue;
        if (child->is_in_flow())
            return true;
    }
    return false;
}

// The insertion_parent_for_*() functions maintain the invariant that the in-flow children of
// block-level boxes must be either all block-level or all inline-level.

static Layout::Node& insertion_parent_for_inline_node(Layout::NodeWithStyle& layout_parent)
{
    auto last_child_creating_anonymous_wrapper_if_needed = [](auto& layout_parent) -> Layout::Node& {
        if (!layout_parent.last_child()
            || !layout_parent.last_child()->is_anonymous()
            || !layout_parent.last_child()->children_are_inline()
            || layout_parent.last_child()->is_generated_for_pseudo_element()) {
            layout_parent.append_child(layout_parent.create_anonymous_wrapper());
        }
        return *layout_parent.last_child();
    };

    if (is<FieldSetBox>(layout_parent))
        return last_child_creating_anonymous_wrapper_if_needed(layout_parent);

    if (layout_parent.is_svg_foreign_object_box())
        return last_child_creating_anonymous_wrapper_if_needed(layout_parent);

    if (layout_parent.display().is_inline_outside() && layout_parent.display().is_flow_inside())
        return layout_parent;

    if (layout_parent.display().is_flex_inside() || layout_parent.display().is_grid_inside())
        return last_child_creating_anonymous_wrapper_if_needed(layout_parent);

    if (!has_in_flow_block_children(layout_parent) || layout_parent.children_are_inline())
        return layout_parent;

    // Parent has block-level children, insert into an anonymous wrapper block (and create it first if needed)
    return last_child_creating_anonymous_wrapper_if_needed(layout_parent);
}

static Layout::Node& insertion_parent_for_block_node(Layout::NodeWithStyle& layout_parent, Layout::Node& layout_node)
{
    // Inline is fine for in-flow block children; we'll maintain the (non-)inline invariant after insertion.
    if (!layout_node.is_anonymous() && layout_parent.is_inline() && layout_parent.display().is_flow_inside() && !layout_node.is_out_of_flow())
        return layout_parent;

    // Make sure we're not inserting into an inline node, since those do not support block nodes.
    auto* new_parent = &layout_parent;
    while (is<InlineNode>(new_parent))
        new_parent = new_parent->parent();

    // If the parent block has no children, insert this block into parent.
    if (!has_inline_or_in_flow_block_children(*new_parent))
        return *new_parent;

    // If the block is out-of-flow and is not a pseudo element,
    if (layout_node.is_out_of_flow() && !layout_node.is_generated_for_pseudo_element()) {
        // And the parent's last child is an anonymous block, join that anonymous block.
        if (!new_parent->display().is_flex_inside()
            && !new_parent->display().is_grid_inside()
            && !new_parent->last_child()->is_generated_for_pseudo_element()
            && new_parent->last_child()->is_anonymous()
            && new_parent->last_child()->children_are_inline()) {
            return *new_parent->last_child();
        }

        // Otherwise, insert this block into parent.
        return *new_parent;
    }

    // If the parent block has block-level children, insert this block into parent.
    if (!new_parent->children_are_inline())
        return *new_parent;

    // Parent block has inline-level children (our siblings); wrap these siblings into an anonymous wrapper block.
    auto wrapper = new_parent->create_anonymous_wrapper();
    wrapper->set_children_are_inline(true);

    for (auto child = new_parent->first_child(); child;) {
        auto next_child = child->next_sibling();
        new_parent->remove_child(*child);
        wrapper->append_child(*child);
        child = next_child;
    }

    new_parent->set_children_are_inline(false);
    new_parent->append_child(wrapper);

    // Then it's safe to insert this block into parent.
    return *new_parent;
}

void TreeBuilder::insert_node_into_inline_or_block_ancestor(Layout::Node& node, CSS::Display display, AppendOrPrepend mode)
{
    // Find the nearest ancestor that can host the node.
    auto& nearest_insertion_ancestor = [&]() -> NodeWithStyle& {
        for (auto& ancestor : m_ancestor_stack.in_reverse()) {
            if (ancestor->is_svg_foreign_object_box())
                return *ancestor;

            auto const& ancestor_display = ancestor->display();

            // Out-of-flow nodes cannot be hosted in inline flow nodes.
            if (node.is_out_of_flow() && ancestor_display.is_inline_outside() && ancestor_display.is_flow_inside())
                continue;

            return *ancestor;
        }
        VERIFY_NOT_REACHED();
    }();

    auto& insertion_point = display.is_inline_outside() ? insertion_parent_for_inline_node(nearest_insertion_ancestor)
                                                        : insertion_parent_for_block_node(nearest_insertion_ancestor, node);

    if (mode == AppendOrPrepend::Prepend)
        insertion_point.prepend_child(node);
    else
        insertion_point.append_child(node);

    if (display.is_inline_outside()) {
        // After inserting an inline-level box into a parent, mark the parent as having inline children.
        insertion_point.set_children_are_inline(true);
    } else if (node.is_in_flow()) {
        // After inserting an in-flow block-level box into a parent, mark the parent as having non-inline children.
        insertion_point.set_children_are_inline(false);
    }
}

class GeneratedContentImageProvider final
    : public ImageProvider
    , public CSS::ImageStyleValue::Client {
public:
    virtual ~GeneratedContentImageProvider() override
    {
        unregister_image_style_value_client();
    }

    virtual void layout_node_was_detached() const override
    {
        unregister_image_style_value_client();
        m_layout_node = nullptr;
    }

    static NonnullOwnPtr<GeneratedContentImageProvider> create(DOM::Document& document, NonnullRefPtr<CSS::ImageStyleValue> image)
    {
        return adopt_own(*new GeneratedContentImageProvider(document, move(image)));
    }

    void set_layout_node(Layout::Node& layout_node)
    {
        m_layout_node = layout_node;
    }

    virtual GC::Ptr<HTML::DecodedImageData> decoded_image_data() const override
    {
        if (auto document = this->document())
            return m_image->image_data(*document);
        return nullptr;
    }

private:
    GeneratedContentImageProvider(DOM::Document& document, NonnullRefPtr<CSS::ImageStyleValue> image)
        : Client(document, image)
        , m_image(move(image))
    {
    }

    virtual void image_style_value_did_update(CSS::ImageStyleValue&) override
    {
        if (!m_layout_node)
            return;
        m_layout_node->set_needs_layout_update(DOM::SetNeedsLayoutReason::GeneratedContentImageFinishedLoading);
    }

    void unregister_image_style_value_client() const
    {
        if (!m_registered_as_image_style_value_client)
            return;
        const_cast<GeneratedContentImageProvider&>(*this).image_style_value_finalize();
        m_registered_as_image_style_value_client = false;
    }

    mutable WeakPtr<Layout::Node> m_layout_node;
    NonnullRefPtr<CSS::ImageStyleValue> m_image;
    mutable bool m_registered_as_image_style_value_client { true };
};

struct FirstLetterTarget {
    TextNode* text_node { nullptr };
    size_t letter_start { 0 };
    size_t letter_end { 0 };
};

// https://drafts.csswg.org/css-pseudo-4/#first-letter-pattern
static Optional<FirstLetterTarget> find_first_letter_in_text(TextNode& text_node)
{
    // NB: Matches the first-letter text pattern: (P (Zs|P)*)? (L|N|S) ((Zs|P-(Ps|Pd))* (P-(Ps|Pd))?)?

    // For the preceding run: Zs excluding U+3000 IDEOGRAPHIC SPACE.
    auto is_preceding_intervening_space = [](u32 code_point) {
        if (code_point == 0x3000)
            return false;
        return Unicode::code_point_has_space_separator_general_category(code_point);
    };

    // For the trailing run: Zs excluding U+3000 IDEOGRAPHIC SPACE and word separators.
    auto is_trailing_intervening_space = [](u32 code_point) {
        // NB: css-text-4 defines word separators as a non-exhaustive list, but of the seven code
        //     points it names only U+0020 SPACE and U+00A0 NO-BREAK SPACE are in the Zs category;
        //     the rest are in Po and would never reach this check. Fixed-width spaces are explicitly
        //     not word separators per the spec's note, so they remain valid intervening Zs here.
        if (code_point == 0x0020 || code_point == 0x00A0 || code_point == 0x3000)
            return false;
        return Unicode::code_point_has_space_separator_general_category(code_point);
    };

    auto is_trailing_punctuation = [](u32 code_point) {
        if (!Unicode::code_point_has_punctuation_general_category(code_point))
            return false;

        // NB: The css-pseudo specification excludes Ps and Pd classes (closing punctuation and dashes) from the
        //     trailing run, whereas CSS 2.1 allowed all classes in both the preceding and trailing runs.
        static auto const ps = Unicode::general_category_from_string("Ps"sv).value();
        static auto const pd = Unicode::general_category_from_string("Pd"sv).value();
        return !Unicode::code_point_has_general_category(code_point, ps)
            && !Unicode::code_point_has_general_category(code_point, pd);
    };

    auto is_first_letter_character = [](u32 code_point) {
        return Unicode::code_point_has_letter_general_category(code_point)
            || Unicode::code_point_has_number_general_category(code_point)
            || Unicode::code_point_has_symbol_general_category(code_point);
    };

    auto view = text_node.text().utf16_view();
    auto const code_units = view.length_in_code_units();

    // When white-space preserves segment breaks, a newline before any letter puts the letter on a later line, so the
    // first formatted line is empty and ::first-letter must not match.
    auto const white_space_collapse = text_node.computed_values().white_space_collapse();
    auto const preserves_segment_breaks = first_is_one_of(white_space_collapse,
        CSS::WhiteSpaceCollapse::Preserve, CSS::WhiteSpaceCollapse::PreserveBreaks, CSS::WhiteSpaceCollapse::BreakSpaces);

    auto advance = [&](size_t index) {
        return index + AK::UnicodeUtils::code_unit_length_for_code_point(view.code_point_at(index));
    };

    auto grapheme_segmenter = text_node.document().grapheme_segmenter().clone();
    grapheme_segmenter->set_segmented_text(view);

    auto advance_cluster = [&](size_t index) -> size_t {
        return grapheme_segmenter->next_boundary(index).value_or(code_units);
    };

    for (size_t match_start = 0; match_start < code_units; match_start = advance(match_start)) {
        size_t cursor = match_start;
        auto starting_code_point = view.code_point_at(cursor);

        if (preserves_segment_breaks && (starting_code_point == '\n' || starting_code_point == '\r'))
            return {};

        // A valid match starts with either a P, or the letter itself.
        bool const has_preceding = Unicode::code_point_has_punctuation_general_category(starting_code_point);
        if (!has_preceding && !is_first_letter_character(starting_code_point))
            continue;

        if (has_preceding) {
            // Preceding group: P followed by (Zs|P)*.
            cursor = advance_cluster(cursor);
            while (cursor < code_units) {
                auto code_point = view.code_point_at(cursor);
                if (!Unicode::code_point_has_punctuation_general_category(code_point)
                    && !is_preceding_intervening_space(code_point))
                    break;
                cursor = advance_cluster(cursor);
            }
        }

        // The letter (L|N|S) must follow the preceding group. If the preceding punctuation consumed the entire text
        // node, accept it as the first-letter.
        if (cursor >= code_units)
            return FirstLetterTarget { &text_node, match_start, cursor };
        if (!is_first_letter_character(view.code_point_at(cursor)))
            continue;

        auto letter_end = advance_cluster(cursor);

        // Trailing group: greedy match of (Zs|P-(Ps|Pd))*.
        while (letter_end < code_units) {
            auto code_point = view.code_point_at(letter_end);
            if (!is_trailing_intervening_space(code_point) && !is_trailing_punctuation(code_point))
                break;
            letter_end = advance_cluster(letter_end);
        }

        return FirstLetterTarget { &text_node, match_start, letter_end };
    }
    return {};
}

// https://drafts.csswg.org/css-pseudo-4/#first-letter-application
static Optional<FirstLetterTarget> find_first_letter_in_block(BlockContainer& block)
{
    // NB: This walks a block container's inline descendants looking for the first-letter text. If the block has block
    //     children instead of inline, recurses into each in-flow block child in turn.

    auto is_marker_content = [](Node const& node) {
        return is<ListItemMarkerBox>(node) || node.generated_for_pseudo_element() == CSS::PseudoElement::Marker;
    };

    if (block.children_are_inline()) {
        Optional<FirstLetterTarget> result;
        block.for_each_in_subtree([&](Node& node) {
            if (is_marker_content(node) || node.is_out_of_flow())
                return TraversalDecision::SkipChildrenAndContinue;
            if (auto* text_node = as_if<TextNode>(node)) {
                result = find_first_letter_in_text(*text_node);
                return result.has_value() ? TraversalDecision::Break : TraversalDecision::Continue;
            }
            if (is<InlineNode>(node))
                return TraversalDecision::Continue;

            return TraversalDecision::Break;
        });
        return result;
    }

    // We have no inline content of our own but ::first-letter can still apply to text in an in-flow block descendant,
    // so walk into each in-flow block child in document order until one yields a letter.
    for (auto child = block.first_child(); child; child = child->next_sibling()) {
        if (is_marker_content(*child))
            continue;
        if (child->is_out_of_flow())
            continue;
        auto* inner_block = as_if<BlockContainer>(*child);
        if (!inner_block)
            break;
        // Stop descending if this child block defines its own ::first-letter: the child will style the first letter
        // inside it, so the ancestor's ::first-letter must not also claim the same letter.
        if (auto* dom_element = as_if<DOM::Element>(inner_block->dom_node()); dom_element && dom_element->computed_properties(CSS::PseudoElement::FirstLetter))
            break;
        if (auto target = find_first_letter_in_block(*inner_block); target.has_value())
            return target;
        if (!inner_block->is_anonymous())
            break;
    }
    return {};
}

void TreeBuilder::create_first_letter_wrapper_if_needed(DOM::Element& element, BlockContainer& block_container)
{
    auto first_letter_style = element.computed_properties(CSS::PseudoElement::FirstLetter);
    if (!first_letter_style)
        return;

    auto target = find_first_letter_in_block(block_container);
    if (!target.has_value())
        return;

    auto& text_node = *target->text_node;
    auto const full_length = text_node.text().length_in_code_units();

    auto const letter_end = target->letter_end;

    auto& document = element.document();

    RefPtr<TextNode> remainder_slice;
    RefPtr<TextNode> first_letter_slice;
    if (auto* dom_text = text_node.dom_text()) {
        auto& mutable_dom_text = const_cast<DOM::Text&>(*dom_text);
        auto dom_remainder_slice = make_ref_counted<TextSliceNode>(document, mutable_dom_text, Node::AttachToDOMNode::Yes, letter_end, full_length - letter_end);
        auto dom_first_letter_slice = make_ref_counted<TextSliceNode>(document, mutable_dom_text, Node::AttachToDOMNode::No, 0, letter_end);
        dom_remainder_slice->set_first_letter_slice(*dom_first_letter_slice);
        remainder_slice = move(dom_remainder_slice);
        first_letter_slice = move(dom_first_letter_slice);
    } else {
        auto text = text_node.text();
        remainder_slice = make_ref_counted<GeneratedTextNode>(document, Utf16String::from_utf16(text.utf16_view().substring_view(letter_end, full_length - letter_end)));
        first_letter_slice = make_ref_counted<GeneratedTextNode>(document, Utf16String::from_utf16(text.utf16_view().substring_view(0, letter_end)));
    }

    auto first_letter_wrapper = make_ref_counted<InlineNode>(document, nullptr, *first_letter_style);
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

RefPtr<NodeWithStyle> TreeBuilder::create_pseudo_element_if_needed(DOM::Element& element, CSS::PseudoElement pseudo_element, Optional<AppendOrPrepend> insertion_mode)
{
    auto& document = element.document();

    // Clear stale layout nodes before deciding if this pseudo-element still generates one.
    if (auto existing_pseudo = element.get_synthetic_pseudo_element(pseudo_element); existing_pseudo.has_value() && existing_pseudo->layout_node())
        existing_pseudo->set_layout_node(nullptr);

    auto pseudo_element_style = element.computed_properties(pseudo_element);
    if (!pseudo_element_style)
        return {};

    auto initial_quote_nesting_level = m_quote_nesting_level;
    DOM::AbstractElement element_reference { element, pseudo_element };
    auto [pseudo_element_content, final_quote_nesting_level] = pseudo_element_style->content(element_reference, initial_quote_nesting_level);
    m_quote_nesting_level = final_quote_nesting_level;
    auto pseudo_element_display = pseudo_element_style->display();

    Optional<String> content_from_counter_style;
    // ::before and ::after only exist if they have content. `content: normal` computes to `none` for them.
    // We also don't create them if they are `display: none`.
    if (first_is_one_of(pseudo_element, CSS::PseudoElement::Before, CSS::PseudoElement::After)
        && (pseudo_element_display.is_none()
            || pseudo_element_content.type == CSS::ContentData::Type::Normal
            || pseudo_element_content.type == CSS::ContentData::Type::None))
        return {};

    // For ::marker with content or display 'none' -- do nothing.
    if (pseudo_element == CSS::PseudoElement::Marker
        && (pseudo_element_display.is_none() || pseudo_element_content.type == CSS::ContentData::Type::None))
        return {};

    // For ::marker with content 'normal', create the marker pseudo-element from a ListItemMarkerBox
    // FIXME: This + ListItemBox + ListItemMarkerBox will disappear once ::marker pseudo-elements with 'normal' content
    //        are rendered using the special list-item counter.
    //        See: https://github.com/LadybirdBrowser/ladybird/issues/4782
    // NB: Called during layout tree construction.
    if (pseudo_element == CSS::PseudoElement::Marker && pseudo_element_content.type == CSS::ContentData::Type::Normal)
        if (auto* list_box = as_if<ListItemBox>(*element.unsafe_layout_node())) {
            // https://www.w3.org/TR/css-lists-3/#content-property
            // "::marker does not generate a box" when list-style-type is 'none' and there's no marker image. Custom
            // ::marker content is already excluded by the outer condition checking for Type::Normal.
            auto const& list_style_type = list_box->computed_values().list_style_type();
            if (list_style_type.has<Empty>() && !list_box->list_style_image()) {
                return {};
            }

            auto list_item_marker = make_ref_counted<ListItemMarkerBox>(
                document,
                list_style_type,
                list_box->computed_values().list_style_position(),
                element,
                *pseudo_element_style);
            list_box->set_marker(list_item_marker);
            element.set_computed_properties(CSS::PseudoElement::Marker, pseudo_element_style);
            element.set_synthetic_pseudo_element_node({}, CSS::PseudoElement::Marker, list_item_marker);
            list_box->prepend_child(*list_item_marker);
            return list_item_marker;
        }

    RefPtr<NodeWithStyle> pseudo_element_node;
    if (pseudo_element_display.is_contents()) {
        pseudo_element_node = make_ref_counted<InlineNode>(document, nullptr, *pseudo_element_style);
        pseudo_element_node->mutable_computed_values().set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
    } else {
        pseudo_element_node = DOM::Element::create_layout_node_for_display_type(document, pseudo_element_display, *pseudo_element_style, nullptr);
        if (!pseudo_element_node)
            return {};
    }

    // FIXME: This code actually computes style for element::marker, and shouldn't for element::pseudo::marker
    if (is<ListItemBox>(*pseudo_element_node)) {
        auto& style_computer = document.style_computer();

        auto marker_style = style_computer.compute_style({ element, CSS::PseudoElement::Marker });
        auto list_item_marker = make_ref_counted<ListItemMarkerBox>(
            document,
            pseudo_element_node->computed_values().list_style_type(),
            pseudo_element_node->computed_values().list_style_position(),
            element,
            marker_style);
        static_cast<ListItemBox&>(*pseudo_element_node).set_marker(list_item_marker);
        element.set_synthetic_pseudo_element_node({}, CSS::PseudoElement::Marker, list_item_marker);
        pseudo_element_node->prepend_child(*list_item_marker);

        // FIXME: Support counters on element::pseudo::marker
    }

    pseudo_element_node->set_generated_for(pseudo_element, element);
    pseudo_element_node->set_initial_quote_nesting_level(initial_quote_nesting_level);

    element.set_synthetic_pseudo_element_node({}, pseudo_element, pseudo_element_node);
    if (insertion_mode.has_value())
        insert_node_into_inline_or_block_ancestor(*pseudo_element_node, pseudo_element_node->display(), insertion_mode.value());
    pseudo_element_node->mutable_computed_values().set_content(pseudo_element_content);

    CSS::resolve_counters(element_reference);
    // Now that we have counters, we can compute the content for real. Which is silly.
    if (pseudo_element_content.type == CSS::ContentData::Type::List) {
        auto [new_content, _] = pseudo_element_style->content(element_reference, initial_quote_nesting_level);
        pseudo_element_node->mutable_computed_values().set_content(new_content);

        // FIXME: Handle images, and multiple values
        if (new_content.type == CSS::ContentData::Type::List) {
            push_parent(*pseudo_element_node);
            for (auto& item : new_content.data) {
                RefPtr<Layout::Node> layout_node;
                if (auto const* string = item.get_pointer<String>()) {
                    layout_node = make_ref_counted<GeneratedTextNode>(document, Utf16String::from_utf8(*string));
                } else {
                    auto& image = *item.get<NonnullRefPtr<CSS::ImageStyleValue>>();
                    image.load_any_resources(document);
                    auto image_provider = GeneratedContentImageProvider::create(document, image);
                    auto& image_provider_ref = *image_provider;
                    layout_node = make_ref_counted<ImageBox>(document, *pseudo_element_style, move(image_provider));
                    image_provider_ref.set_layout_node(*layout_node);
                }
                layout_node->set_generated_for(pseudo_element, element);
                insert_node_into_inline_or_block_ancestor(*layout_node, layout_node->display(), AppendOrPrepend::Append);
            }
            pop_parent();
        } else {
            TODO();
        }
    }

    return pseudo_element_node;
}

// Block nodes inside inline nodes are allowed, but to maintain the invariant that either all layout children are
// inline or non-inline, we need to rearrange the tree a bit. All inline ancestors up to the node we've inserted are
// wrapped in an anonymous block, which is inserted into the nearest non-inline ancestor. We then recreate the inline
// ancestors in another anonymous block inserted after the node so we can continue adding children.
//
// Effectively, we try to turn this:
//
//     InlineNode 1
//       TextNode 1
//       InlineNode N
//         TextNode N
//         BlockContainer (node)
//
// Into this:
//
//     BlockContainer (anonymous "before")
//       InlineNode 1
//         TextNode 1
//         InlineNode N
//           TextNode N
//     BlockContainer (anonymous "middle") continuation
//       BlockContainer (node)
//     BlockContainer (anonymous "after")
//       InlineNode 1 continuation
//         InlineNode N
//
// To be able to reconstruct their relation after restructuring, layout nodes keep track of their continuation. The
// top-most inline node of the "after" wrapper points to the "middle" wrapper, which points to the top-most inline node
// of the "before" wrapper. All other inline nodes in the "after" wrapper point to their counterparts in the "before"
// wrapper, to make it easier to create the right paintables since a DOM::Node only has a single Layout::Node.
//
// Appending then continues in the "after" tree. If a new block node is then inserted, we can reuse the "middle" wrapper
// if no inline siblings exist for node or its ancestors, and leave the existing "after" wrapper alone. Otherwise, we
// create new wrappers and extend the continuation chain.
//
// Inspired by: https://webkit.org/blog/115/webcore-rendering-ii-blocks-and-inlines/
void TreeBuilder::restructure_block_node_in_inline_parent(NodeWithStyleAndBoxModelMetrics& node)
{
    // Mark parent as inline again
    auto& parent = *node.parent();
    VERIFY(!parent.children_are_inline());
    parent.set_children_are_inline(true);

    // Find nearest ancestor that establishes a BFC (block container) and is not display: contents or anonymous.
    auto& nearest_block_ancestor = [&] -> NodeWithStyle& {
        for (auto* ancestor = parent.parent(); ancestor; ancestor = ancestor->parent()) {
            if (is<BlockContainer>(*ancestor) && !ancestor->display().is_contents() && !ancestor->is_anonymous())
                return *ancestor;
        }
        VERIFY_NOT_REACHED();
    }();
    nearest_block_ancestor.set_children_are_inline(false);

    // Find the topmost inline ancestor.
    RefPtr<NodeWithStyleAndBoxModelMetrics> topmost_inline_ancestor;
    for (auto* ancestor = &parent; ancestor; ancestor = ancestor->parent()) {
        if (ancestor == &nearest_block_ancestor)
            break;
        if (ancestor->is_inline())
            topmost_inline_ancestor = static_cast<NodeWithStyleAndBoxModelMetrics*>(ancestor);
    }
    VERIFY(topmost_inline_ancestor);

    // We need to host the topmost inline ancestor and its previous siblings in an anonymous "before" wrapper. If an
    // inline wrapper does not already exist, we create a new one and add it to the nearest block ancestor.
    RefPtr<Node> before_wrapper;
    if (auto last_child = nearest_block_ancestor.last_child(); last_child && last_child->is_anonymous() && last_child->children_are_inline()) {
        before_wrapper = last_child;
    } else {
        before_wrapper = nearest_block_ancestor.create_anonymous_wrapper();

        before_wrapper->set_children_are_inline(true);
        nearest_block_ancestor.append_child(*before_wrapper);
    }
    if (topmost_inline_ancestor->parent() != before_wrapper.ptr()) {
        RefPtr<Node> inline_to_move = topmost_inline_ancestor;
        while (inline_to_move) {
            auto next = inline_to_move->previous_sibling();
            inline_to_move->remove();
            before_wrapper->insert_before(*inline_to_move, before_wrapper->first_child());
            inline_to_move = next;
        }
    }

    // If we are part of an existing continuation and all inclusive ancestors have no previous siblings, we can reuse
    // the existing middle wrapper. Otherwiser, we create a new middle wrapper to contain the block node and add it to
    // the nearest block ancestor.
    bool needs_new_continuation = true;
    RefPtr<NodeWithStyleAndBoxModelMetrics> middle_wrapper;
    if (topmost_inline_ancestor->continuation_of_node()) {
        needs_new_continuation = false;
        for (RefPtr<Node> ancestor = node; ancestor != topmost_inline_ancestor; ancestor = ancestor->parent()) {
            if (ancestor->previous_sibling()) {
                needs_new_continuation = true;
                break;
            }
        }
        if (!needs_new_continuation)
            middle_wrapper = topmost_inline_ancestor->continuation_of_node();
    }
    if (!middle_wrapper) {
        middle_wrapper = static_cast<NodeWithStyleAndBoxModelMetrics&>(*nearest_block_ancestor.create_anonymous_wrapper());
        nearest_block_ancestor.append_child(*middle_wrapper);
        middle_wrapper->set_continuation_of_node({}, topmost_inline_ancestor);
    }

    // Move the block node to the middle wrapper.
    node.remove();
    middle_wrapper->append_child(node);

    // If we need a new continuation, recreate inline ancestors in another anonymous block so we can continue adding new
    // nodes. We don't need to do this if we are within an existing continuation and there were no previous siblings in
    // any inclusive ancestor of node in the after wrapper.
    if (needs_new_continuation) {
        auto after_wrapper = nearest_block_ancestor.create_anonymous_wrapper();
        RefPtr<Node> current_parent = after_wrapper;
        for (RefPtr<Node> inline_node = topmost_inline_ancestor;
            inline_node && is<DOM::Element>(inline_node->dom_node()); inline_node = inline_node->last_child()) {
            auto& element = static_cast<DOM::Element&>(*inline_node->dom_node());

            auto style = element.computed_properties();
            auto new_layout_node = element.create_layout_node(*style);
            if (!new_layout_node)
                break;
            auto* new_inline_node = as_if<NodeWithStyleAndBoxModelMetrics>(*new_layout_node);
            if (!new_inline_node)
                break;
            if (inline_node == topmost_inline_ancestor) {
                // The topmost inline ancestor points to the middle wrapper, which in turns points to the original node.
                new_inline_node->set_continuation_of_node({}, middle_wrapper);
                topmost_inline_ancestor = *new_inline_node;
            } else {
                // We need all other inline nodes to point to their original node so we can walk the continuation chain
                // in LayoutState and create the right paintables.
                new_inline_node->set_continuation_of_node({}, &static_cast<NodeWithStyleAndBoxModelMetrics&>(*inline_node));
            }

            current_parent->append_child(*new_inline_node);
            current_parent = *new_inline_node;

            // Replace the node in the ancestor stack with the new node.
            auto& node_with_style = static_cast<NodeWithStyle&>(*inline_node);
            if (auto stack_index = m_ancestor_stack.find_first_index(&node_with_style); stack_index.has_value())
                m_ancestor_stack[stack_index.release_value()] = new_inline_node;

            // Stop recreating nodes when we've reached node's parent.
            if (inline_node == &parent)
                break;
        }

        after_wrapper->set_children_are_inline(true);
        nearest_block_ancestor.append_child(after_wrapper);
    }
}

static bool is_ignorable_whitespace(Layout::Node const& node)
{
    if (auto* text_node = as_if<TextNode>(node); text_node && text_node->text_for_rendering().is_ascii_whitespace())
        return true;

    if (node.is_anonymous() && node.is_block_container() && node.children_are_inline()) {
        bool contains_only_white_space = true;
        node.for_each_in_inclusive_subtree([&contains_only_white_space](auto& descendant) {
            if (auto* text_node = as_if<TextNode>(descendant)) {
                if (!text_node->text_for_rendering().is_ascii_whitespace()) {
                    contains_only_white_space = false;
                    return TraversalDecision::Break;
                }
            } else if (descendant.is_out_of_flow() || !descendant.is_anonymous()) {
                contains_only_white_space = false;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        if (contains_only_white_space)
            return true;
    }

    return false;
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

static DOM::Element* display_contents_style_parent_for_text_node(DOM::Text& text_node)
{
    auto* parent = text_node.flat_tree_parent();
    auto* parent_element = as_if<DOM::Element>(parent);
    if (!parent_element || !parent_element->computed_properties())
        return nullptr;
    if (!parent_element->computed_properties()->display().is_contents())
        return nullptr;
    return parent_element;
}

static bool display_contents_text_needs_style_wrapper(DOM::Text& text_node, DOM::Element const& style_parent)
{
    if (!text_node.data().is_ascii_whitespace())
        return true;

    return !first_is_one_of(style_parent.computed_properties()->white_space_collapse(), CSS::WhiteSpaceCollapse::Collapse);
}

TraversalDecision TreeBuilder::clear_stale_layout_and_paint_node(DOM::Node& node, DOM::Node const* content_visibility_hidden_root)
{
    node.set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
    node.set_child_needs_layout_tree_update(false);

    // NB: Called during layout tree construction.
    RefPtr<Layout::Node> layout_node = node.unsafe_layout_node();
    // SVGPatternBox, SVGMaskBox, and SVGClipBox are created on behalf of a referencing
    // element and attached to that element's layout subtree. Skip them so they survive
    // cleanup of their DOM ancestor, unless their layout attachment is inside the
    // subtree being hidden too.
    if (layout_node && is_svg_resource_box(*layout_node)
        && (!content_visibility_hidden_root || !layout_node_is_attached_to_dom_subtree(*layout_node, *content_visibility_hidden_root))) {
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

void TreeBuilder::update_layout_tree(DOM::Node& dom_node, TreeBuilder::Context& context, MustCreateSubtree must_create_subtree)
{
    // NB: Called during layout tree construction.
    bool should_create_layout_node = must_create_subtree == MustCreateSubtree::Yes
        || dom_node.needs_layout_tree_update()
        || dom_node.document().needs_full_layout_tree_update()
        || (dom_node.is_document() && !dom_node.unsafe_layout_node());

    if (dom_node.is_element()) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        if (element.rendered_in_top_layer() && !context.layout_top_layer)
            return;
    }
    if (dom_node.is_element())
        dom_node.document().style_computer().push_ancestor(static_cast<DOM::Element const&>(dom_node));

    ScopeGuard pop_ancestor_guard = [&] {
        if (dom_node.is_element())
            dom_node.document().style_computer().pop_ancestor(static_cast<DOM::Element const&>(dom_node));
    };

    // NB: Called during layout tree construction.
    RefPtr<Layout::Node> old_layout_node = dom_node.unsafe_layout_node();
    RefPtr<Layout::Node> layout_node;
    Optional<TemporaryChange<bool>> has_svg_root_change;
    auto& document = dom_node.document();
    bool should_clear_stale_layout_subtree_if_no_layout_node = true;

    ScopeGuard remove_stale_layout_node_guard = [&] {
        // If we didn't create a layout node for this DOM node,
        // go through the shadow-including subtree and remove any old layout & paint nodes since they are now all stale.
        if (should_clear_stale_layout_subtree_if_no_layout_node && !layout_node) {
            dom_node.for_each_shadow_including_inclusive_descendant([&](auto& node) {
                return clear_stale_layout_and_paint_node(node);
            });
        }
    };

    if (dom_node.is_svg_container()) {
        has_svg_root_change.emplace(context.has_svg_root, true);
    } else if (dom_node.requires_svg_container() && !context.has_svg_root) {
        return;
    }

    auto& style_computer = document.style_computer();
    RefPtr<CSS::ComputedProperties> style;
    CSS::Display display;

    if (!should_create_layout_node) {
        if (is<DOM::Element>(dom_node)) {
            auto& element = static_cast<DOM::Element&>(dom_node);
            style = element.computed_properties();
            display = style->display();
            if (display.is_contents()) {
                should_clear_stale_layout_subtree_if_no_layout_node = false;
                update_layout_tree_for_display_contents(element, context, must_create_subtree, should_create_layout_node);
                return;
            }
        }
        // NB: Called during layout tree construction.
        layout_node = dom_node.unsafe_layout_node();
    } else {
        if (is<DOM::Element>(dom_node)) {
            auto& element = static_cast<DOM::Element&>(dom_node);
            // ::backdrop is a sibling of the element, not a child, so unlike other pseudo-elements, it's not
            // automatically discarded when element's layout is recomputed. We must remove it manually.
            if (auto old_backdrop_node = element.pseudo_element_unsafe_layout_node(CSS::PseudoElement::Backdrop))
                old_backdrop_node->remove();
            element.clear_synthetic_pseudo_element_layout_nodes(Badge<TreeBuilder> {});
            // Elements inside a `display:none` subtree are skipped by
            // `Document::update_style_recursively`, so a bypass path (top-layer iteration, slot
            // projection, SVG mask/clip-path or pattern reference) may reach an element whose
            // `needs_style_update` flag is still set or whose `computed_properties` is null. Route
            // through `update_style_for_element`, which seeds the style computer's ancestor filter
            // so descendant-combinator selectors continue to match during the lazy re-cascade.
            if (element.needs_style_update() || !element.computed_properties()) {
                document.update_style_for_element({ element });
                element.set_needs_style_update(false);
            }
            style = element.computed_properties();
            display = style->display();
            if (display.is_none())
                return;
            if (display.is_contents()) {
                should_clear_stale_layout_subtree_if_no_layout_node = false;
                update_layout_tree_for_display_contents(element, context, must_create_subtree, should_create_layout_node);
                return;
            }
            // TODO: Implement changing element contents with the `content` property.
            if (context.layout_svg_mask_or_clip_path) {
                if (is<SVG::SVGMaskElement>(dom_node))
                    layout_node = make_ref_counted<Layout::SVGMaskBox>(document, static_cast<SVG::SVGMaskElement&>(dom_node), *style);
                else if (is<SVG::SVGClipPathElement>(dom_node))
                    layout_node = make_ref_counted<Layout::SVGClipBox>(document, static_cast<SVG::SVGClipPathElement&>(dom_node), *style);
                else
                    VERIFY_NOT_REACHED();
                // Only layout direct uses of SVG masks/clipPaths.
                context.layout_svg_mask_or_clip_path = false;
            } else if (context.layout_svg_pattern) {
                layout_node = make_ref_counted<Layout::SVGPatternBox>(document, as<SVG::SVGPatternElement>(dom_node), *style);
                context.layout_svg_pattern = false;
            } else {
                layout_node = element.create_layout_node(*style);
            }
        } else if (is<DOM::Document>(dom_node)) {
            style = style_computer.create_document_style();
            display = style->display();
            layout_node = make_ref_counted<Layout::Viewport>(static_cast<DOM::Document&>(dom_node), *style);
        } else if (is<DOM::Text>(dom_node)) {
            auto& text_node = static_cast<DOM::Text&>(dom_node);
            layout_node = make_ref_counted<Layout::TextNode>(document, text_node);
            display = CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow);
            if (auto* style_parent = display_contents_style_parent_for_text_node(text_node); style_parent && display_contents_text_needs_style_wrapper(text_node, *style_parent)) {
                auto wrapper = make_ref_counted<Layout::InlineNode>(document, nullptr, *style_parent->computed_properties());
                wrapper->mutable_computed_values().set_display(display);
                wrapper->set_children_are_inline(true);
                wrapper->append_child(*layout_node);
                layout_node = move(wrapper);
            }
        }
    }

    if (!layout_node)
        return;

    // Decide whether to replace an existing node (partial tree update) or insert a new one appropriately.
    bool const may_replace_existing_layout_node = must_create_subtree == MustCreateSubtree::No
        && old_layout_node
        && old_layout_node->parent()
        && old_layout_node != layout_node;

    if (dom_node.is_element() && should_create_layout_node) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        // Each element rendered in the top layer has a ::backdrop pseudo-element, for which it is the originating element.
        if (element.rendered_in_top_layer() && context.layout_top_layer) {
            // If we're inserting a new element, we can append the ::backdrop node now, before layout_node is appended.
            // Otherwise, we need to insert the ::backdrop before old_layout_node so it's behind the layout_node.
            if (may_replace_existing_layout_node) {
                if (auto backdrop_node = create_pseudo_element_if_needed(element, CSS::PseudoElement::Backdrop, {})) {
                    old_layout_node->parent()->insert_before(*backdrop_node, old_layout_node);
                }
            } else {
                (void)create_pseudo_element_if_needed(element, CSS::PseudoElement::Backdrop, AppendOrPrepend::Append);
            }
        }
    }

    if (dom_node.is_document()) {
        m_layout_root = layout_node;
    } else if (should_create_layout_node) {
        if (may_replace_existing_layout_node) {
            old_layout_node->prepare_subtree_for_detach_from_layout_tree();
            old_layout_node->parent()->replace_child(*layout_node, *old_layout_node);
        } else if (layout_node->is_svg_box()) {
            m_ancestor_stack.last()->append_child(*layout_node);
        } else {
            insert_node_into_inline_or_block_ancestor(*layout_node, display, AppendOrPrepend::Append);
        }
    }

    auto* dom_element = as_if<DOM::Element>(dom_node);
    auto shadow_root = dom_element ? dom_element->shadow_root() : nullptr;

    auto element_has_content_visibility_hidden = [&dom_node]() {
        if (is<DOM::Element>(dom_node)) {
            auto& element = static_cast<DOM::Element&>(dom_node);
            return element.computed_properties()->content_visibility() == CSS::ContentVisibility::Hidden;
        }
        return false;
    }();

    auto prior_quote_nesting_level = m_quote_nesting_level;

    if (should_create_layout_node) {
        // Resolve counters now that we exist in the layout tree.
        if (auto* element = as_if<DOM::Element>(dom_node)) {
            DOM::AbstractElement element_reference { *element };
            CSS::resolve_counters(element_reference);
        }

        update_layout_tree_before_children(dom_node, *layout_node, context, element_has_content_visibility_hidden);
    }

    if (element_has_content_visibility_hidden) {
        dom_node.for_each_shadow_including_descendant([&](auto& node) {
            return clear_stale_layout_and_paint_node(node, &dom_node);
        });
    }

    auto should_layout_dom_children = [&]() {
        if (auto const* slot_element = as_if<HTML::HTMLSlotElement>(dom_node))
            return slot_element->assigned_nodes_internal().is_empty() && dom_node.has_children();
        return dom_node.has_children();
    }();

    if (should_create_layout_node || dom_node.child_needs_layout_tree_update()) {
        if ((should_layout_dom_children || shadow_root) && layout_node->can_have_children() && !element_has_content_visibility_hidden) {
            push_parent(as<NodeWithStyle>(*layout_node));
            if (shadow_root) {
                // For replaced elements with shadow DOM children, wrap the children in an
                // anonymous BlockContainer so that a BFC handles their layout.
                if (layout_node->is_replaced_box_with_children()) {
                    if (!layout_node->first_child() || !layout_node->first_child()->is_anonymous()) {
                        auto wrapper = as<NodeWithStyle>(*layout_node).create_anonymous_wrapper();
                        m_ancestor_stack.last()->append_child(wrapper);
                    }
                    push_parent(as<NodeWithStyle>(*layout_node->first_child()));
                }
                for (auto* node = shadow_root->first_child(); node; node = node->next_sibling()) {
                    update_layout_tree(*node, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
                }
                if (layout_node->is_replaced_box_with_children())
                    pop_parent();
                shadow_root->set_child_needs_layout_tree_update(false);
                shadow_root->set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
            } else if (should_layout_dom_children) {
                // This is the same as as<DOM::ParentNode>(dom_node).for_each_child
                for (auto* node = as<DOM::ParentNode>(dom_node).first_child(); node; node = node->next_sibling())
                    update_layout_tree(*node, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
            }

            if (dom_node.is_document()) {
                // Elements in the top layer do not lay out normally based on their position in the document; instead they
                // generate boxes as if they were siblings of the root element.
                TemporaryChange<bool> layout_mask(context.layout_top_layer, true);
                for (auto const& top_layer_element : document.top_layer_elements()) {
                    if (top_layer_element->rendered_in_top_layer())
                        update_layout_tree(top_layer_element, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
                }
            }
            pop_parent();
        }
    }

    if (is<HTML::HTMLSlotElement>(dom_node)) {
        auto& slot_element = static_cast<HTML::HTMLSlotElement&>(dom_node);

        if (slot_element.computed_properties()->content_visibility() != CSS::ContentVisibility::Hidden) {
            auto slottables = slot_element.assigned_nodes_internal();
            push_parent(as<NodeWithStyle>(*layout_node));

            MustCreateSubtree must_create_subtree_for_slottable = must_create_subtree;
            if (slot_element.needs_layout_tree_update())
                must_create_subtree_for_slottable = MustCreateSubtree::Yes;

            for (auto const& slottable : slottables) {
                slottable.visit([&](auto& node) { update_layout_tree(node, context, must_create_subtree_for_slottable); });
            }

            pop_parent();
        } else {
            // Assigned slottables are not DOM descendants of the slot, so the generic
            // content-visibility:hidden descendant cleanup above does not reach them.
            for (auto const& slottable : slot_element.assigned_nodes_internal()) {
                slottable.visit([&](DOM::Node& slottable_root) {
                    slottable_root.for_each_shadow_including_inclusive_descendant([&](auto& node) {
                        return clear_stale_layout_and_paint_node(node, &slottable_root);
                    });
                });
            }
        }
    }

    if (should_create_layout_node) {
        update_layout_tree_after_children(dom_node, *layout_node, context, element_has_content_visibility_hidden);
        wrap_in_button_layout_tree_if_needed(dom_node, *layout_node);

        // If we completely finished inserting a block level element into an inline parent, we need to fix up the tree so
        // that we can maintain the invariant that all children are either inline or non-inline. We can't do this earlier,
        // because the restructuring adds new children after this node that become part of the ancestor stack.
        if (auto node_with_metrics = as_if<NodeWithStyleAndBoxModelMetrics>(*layout_node);
            node_with_metrics && node_with_metrics->should_create_inline_continuation())
            restructure_block_node_in_inline_parent(*node_with_metrics);
    }

    // https://www.w3.org/TR/css-contain-2/#containment-style
    // Giving an element style containment has the following effects:
    // 2. The effects of the 'content' property’s 'open-quote', 'close-quote', 'no-open-quote' and 'no-close-quote' must
    //    be scoped to the element’s sub-tree.
    if (layout_node->has_style_or_parent_with_style() && layout_node->has_style_containment()) {
        m_quote_nesting_level = prior_quote_nesting_level;
    }

    dom_node.set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
    dom_node.set_child_needs_layout_tree_update(false);
}

void TreeBuilder::update_layout_tree_for_display_contents(DOM::Element& element, TreeBuilder::Context& context, MustCreateSubtree must_create_subtree, bool should_create_layout_node)
{
    element.clear_synthetic_pseudo_element_layout_nodes(Badge<TreeBuilder> {});

    if (should_create_layout_node) {
        element.for_each_shadow_including_inclusive_descendant([&](auto& node) {
            return clear_stale_layout_and_paint_node(node);
        });

        DOM::AbstractElement element_reference { element };
        CSS::resolve_counters(element_reference);
    }

    auto element_has_content_visibility_hidden = element.computed_properties()->content_visibility() == CSS::ContentVisibility::Hidden;
    if (!element_has_content_visibility_hidden)
        (void)create_pseudo_element_if_needed(element, CSS::PseudoElement::Before, AppendOrPrepend::Append);

    auto should_layout_dom_children = [&]() {
        if (auto const* slot_element = as_if<HTML::HTMLSlotElement>(element))
            return slot_element->assigned_nodes_internal().is_empty() && element.has_children();
        return element.has_children();
    }();

    auto shadow_root = element.shadow_root();
    if (!element_has_content_visibility_hidden && (should_create_layout_node || element.child_needs_layout_tree_update())) {
        if (shadow_root) {
            for (auto* node = shadow_root->first_child(); node; node = node->next_sibling())
                update_layout_tree(*node, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
            shadow_root->set_child_needs_layout_tree_update(false);
            shadow_root->set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
        } else if (should_layout_dom_children) {
            for (auto* node = element.first_child(); node; node = node->next_sibling())
                update_layout_tree(*node, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
        }
    }

    if (is<HTML::HTMLSlotElement>(element)) {
        auto& slot_element = static_cast<HTML::HTMLSlotElement&>(element);

        if (!element_has_content_visibility_hidden) {
            MustCreateSubtree must_create_subtree_for_slottable = must_create_subtree;
            if (slot_element.needs_layout_tree_update())
                must_create_subtree_for_slottable = MustCreateSubtree::Yes;

            for (auto const& slottable : slot_element.assigned_nodes_internal())
                slottable.visit([&](auto& node) { update_layout_tree(node, context, must_create_subtree_for_slottable); });
        } else {
            for (auto const& slottable : slot_element.assigned_nodes_internal()) {
                slottable.visit([&](DOM::Node& slottable_root) {
                    slottable_root.for_each_shadow_including_inclusive_descendant([&](auto& node) {
                        return clear_stale_layout_and_paint_node(node, &slottable_root);
                    });
                });
            }
        }
    }

    if (!element_has_content_visibility_hidden)
        (void)create_pseudo_element_if_needed(element, CSS::PseudoElement::After, AppendOrPrepend::Append);

    element.set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
    element.set_child_needs_layout_tree_update(false);
}

void TreeBuilder::wrap_in_button_layout_tree_if_needed(DOM::Node& dom_node, Layout::Node& layout_node)
{
    auto const* html_element = as_if<HTML::HTMLElement>(dom_node);
    if (!html_element || !html_element->uses_button_layout())
        return;

    // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
    // If the element is an input element, or if it is a button element and its computed value for 'display' is not
    // 'inline-grid', 'grid', 'inline-flex', or 'flex', then the element's box has a child anonymous button content box
    // with the following behaviors:
    auto display = layout_node.display();
    if (!display.is_grid_inside() && !display.is_flex_inside()) {
        auto& parent = as<NodeWithStyle>(layout_node);

        // If the box does not overflow in the vertical axis, then it is centered vertically.
        // FIXME: Only apply alignment when box overflows
        auto flex_wrapper = parent.create_anonymous_wrapper();
        auto& flex_computed_values = flex_wrapper->mutable_computed_values();
        flex_computed_values.set_display(CSS::Display { CSS::DisplayOutside::Block, CSS::DisplayInside::Flex });
        flex_computed_values.set_justify_content(CSS::JustifyContent::Center);
        flex_computed_values.set_flex_direction(CSS::FlexDirection::Column);
        flex_computed_values.set_height(CSS::Size::make_percentage(CSS::Percentage(100)));
        flex_computed_values.set_min_height(parent.computed_values().min_height());

        auto content_box_wrapper = parent.create_anonymous_wrapper();
        auto& content_computed_values = content_box_wrapper->mutable_computed_values();
        // Let percentage-sized descendants shrink to fixed-height buttons instead of the flex
        // item's automatic minimum size.
        content_computed_values.set_min_height(CSS::Size::make_px(CSSPixels(0)));
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
}

void TreeBuilder::update_layout_tree_before_children(DOM::Node& dom_node, Layout::Node& layout_node, TreeBuilder::Context&, bool element_has_content_visibility_hidden)
{
    // Add node for the ::before pseudo-element.
    if (is<DOM::Element>(dom_node) && layout_node.can_have_children() && !element_has_content_visibility_hidden) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        push_parent(as<NodeWithStyle>(layout_node));
        (void)create_pseudo_element_if_needed(element, CSS::PseudoElement::Before, AppendOrPrepend::Prepend);

        pop_parent();
    }
}

void TreeBuilder::update_layout_tree_after_children(DOM::Node& dom_node, Layout::Node& layout_node, TreeBuilder::Context& context, bool element_has_content_visibility_hidden)
{
    if (is<SVG::SVGGraphicsElement>(dom_node)) {
        auto& graphics_element = static_cast<SVG::SVGGraphicsElement&>(dom_node);
        // Create the layout tree for the SVG mask/clip paths as a child of the masked element.
        // Note: This will create a new subtree for each use of the mask (so there's  not a 1-to-1 mapping
        // from DOM node to mask layout node). Each use of a mask may be laid out differently so this
        // duplication is necessary.
        auto layout_mask_or_clip_path = [&](GC::Ptr<SVG::SVGElement const> mask_or_clip_path) {
            TemporaryChange<bool> layout_mask(context.layout_svg_mask_or_clip_path, true);
            push_parent(as<NodeWithStyle>(layout_node));

            // Check for reference cycle
            for (auto* ancestor : m_ancestor_stack) {
                if (ancestor->dom_node() == mask_or_clip_path) {
                    // FIXME: Somehow either remove ancestor from the layout tree or mark it as invalid.
                    pop_parent();
                    return;
                }
            }
            update_layout_tree(const_cast<SVG::SVGElement&>(*mask_or_clip_path), context, MustCreateSubtree::Yes);
            pop_parent();
        };
        if (auto mask = graphics_element.mask())
            layout_mask_or_clip_path(mask);
        if (auto clip_path = graphics_element.clip_path())
            layout_mask_or_clip_path(clip_path);

        HashTable<SVG::SVGPatternElement const*> seen_content_elements;
        auto layout_pattern = [&](GC::Ptr<SVG::SVGPatternElement const> pattern) {
            if (!pattern)
                return;
            auto content_element = pattern->pattern_content_element();
            if (!content_element)
                return;
            if (seen_content_elements.set(content_element.ptr()) != AK::HashSetResult::InsertedNewEntry)
                return;
            TemporaryChange<bool> layout_flag(context.layout_svg_pattern, true);
            push_parent(as<NodeWithStyle>(layout_node));
            for (auto* ancestor : m_ancestor_stack) {
                if (ancestor->dom_node() == content_element.ptr()) {
                    pop_parent();
                    return;
                }
            }
            update_layout_tree(const_cast<SVG::SVGPatternElement&>(*content_element), context, MustCreateSubtree::Yes);
            pop_parent();
        };
        if (auto fill = graphics_element.fill_pattern())
            layout_pattern(fill);
        if (auto stroke = graphics_element.stroke_pattern())
            layout_pattern(stroke);
    }

    // Add nodes for the ::after pseudo-element.
    if (is<DOM::Element>(dom_node) && layout_node.can_have_children() && !element_has_content_visibility_hidden) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        push_parent(as<NodeWithStyle>(layout_node));

        // https://drafts.csswg.org/css-lists-3/#marker-pseudo
        // The marker box is generated by the ::marker pseudo-element of a list item as the list item’s first child,
        // before the ::before pseudo-element (if it exists on the element). It is filled with content as defined
        // in § 3.2 Generating Marker Contents.
        // NOTE: This happens in update_layout_tree_after_children (and not in ..._before_...), since potential
        //       block container wrapper children are created after update_layout_tree_before_children.
        if (layout_node.is_list_item_box())
            (void)create_pseudo_element_if_needed(element, CSS::PseudoElement::Marker, AppendOrPrepend::Prepend);

        (void)create_pseudo_element_if_needed(element, CSS::PseudoElement::After, AppendOrPrepend::Append);
        pop_parent();

        if (auto* block_container = as_if<BlockContainer>(layout_node))
            create_first_letter_wrapper_if_needed(element, *block_container);
    }

    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    // The anonymous fieldset content box is expected to appear after the rendered legend and is expected to contain the
    // content (including the '::before' and '::after' pseudo-elements) of the fieldset element except for the rendered
    // legend, if there is one.
    if (auto* fieldset_box = as_if<FieldSetBox>(layout_node)) {
        if (auto legend = fieldset_box->rendered_legend()) {
            auto wrapper = fieldset_box->create_anonymous_wrapper();
            auto& wrapper_mutable_values = wrapper->mutable_computed_values();
            wrapper_mutable_values.set_display(CSS::Display::from_short(CSS::Display::Short::FlowRoot));

            // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
            // The following properties are expected to inherit from the fieldset element:
            //     align-content, align-items, border-radius, column-count, column-fill, column-gap, column-rule,
            //     column-width, flex-direction, flex-wrap, grid (grid-auto-columns, grid-auto-flow, grid-auto-rows,
            //     grid-column-gap, grid-row-gap, grid-template-areas, grid-template-columns, grid-template-rows),
            //     justify-content, justify-items, overflow, padding, text-overflow, unicode-bidi
            // FIXME: Transfer all of these properties, not just overflow.
            auto& fieldset_mutable_values = fieldset_box->mutable_computed_values();

            wrapper_mutable_values.set_overflow_x(fieldset_box->computed_values().overflow_x());
            fieldset_mutable_values.set_overflow_x(CSS::InitialValues::overflow());

            wrapper_mutable_values.set_overflow_y(fieldset_box->computed_values().overflow_y());
            fieldset_mutable_values.set_overflow_y(CSS::InitialValues::overflow());

            for (auto child = fieldset_box->first_child(); child;) {
                auto next = child->next_sibling();
                if (child != legend) {
                    fieldset_box->remove_child(*child);
                    wrapper->append_child(*child);
                }
                child = next;
            }
            fieldset_box->append_child(*wrapper);
        }
    }
}

RefPtr<Layout::Node> TreeBuilder::build(DOM::Node& dom_node)
{
    VERIFY(dom_node.is_document());

    dom_node.document().style_computer().reset_ancestor_filter();

    Context context;
    m_quote_nesting_level = 0;
    update_layout_tree(dom_node, context, MustCreateSubtree::No);

    // NB: Called during layout tree construction.
    if (auto* root = dom_node.document().unsafe_layout_node())
        fixup_tables(*root);

    return m_layout_root;
}

template<CSS::DisplayInternal internal, typename Callback>
void TreeBuilder::for_each_in_tree_with_internal_display(NodeWithStyle& root, Callback callback)
{
    root.for_each_in_inclusive_subtree_of_type<Box>([&](auto& box) {
        auto const display = box.display();
        if (display.is_internal() && display.internal() == internal)
            callback(box);
        return TraversalDecision::Continue;
    });
}

template<CSS::DisplayInside inside, typename Callback>
void TreeBuilder::for_each_in_tree_with_inside_display(NodeWithStyle& root, Callback callback)
{
    root.for_each_in_inclusive_subtree_of_type<Box>([&](auto& box) {
        auto const display = box.display();
        if (display.is_outside_and_inside() && display.inside() == inside)
            callback(box);
        return TraversalDecision::Continue;
    });
}

// https://drafts.csswg.org/css-tables-3/#fixup-algorithm
void TreeBuilder::fixup_tables(NodeWithStyle& root)
{
    remove_irrelevant_boxes(root);
    generate_missing_child_wrappers(root);
    auto table_root_boxes = generate_missing_parents(root);
    missing_cells_fixup(table_root_boxes);
}

// https://drafts.csswg.org/css-tables-3/#fixup-algorithm
// 1. Remove irrelevant boxes:
void TreeBuilder::remove_irrelevant_boxes(NodeWithStyle& root)
{
    // The following boxes are discarded as if they were display:none:

    Vector<NonnullRefPtr<Node>> to_remove;

    // 1. Children of a table-column.
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableColumn>(root, [&](Box& table_column) {
        table_column.for_each_child([&](auto& child) {
            to_remove.append(child);
            return IterationDecision::Continue;
        });
    });

    // 2. Children of a table-column-group which are not a table-column.
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableColumnGroup>(root, [&](Box& table_column_group) {
        table_column_group.for_each_child([&](auto& child) {
            if (!child.display().is_table_column())
                to_remove.append(child);
            return IterationDecision::Continue;
        });
    });

    // FIXME:
    // 3. Anonymous inline boxes which contain only white space and are between two immediate siblings each of which is a table-non-root box.
    // 4. Anonymous inline boxes which meet all of the following criteria:
    //    - they contain only white space
    //    - they are the first and/or last child of a tabular container
    //    - whose immediate sibling, if any, is a table-non-root box

    for (auto& box : to_remove)
        box->parent()->remove_child(*box);
}

static bool is_table_track(CSS::Display display)
{
    return display.is_table_row() || display.is_table_column();
}

static bool is_table_track_group(CSS::Display display)
{
    // Unless explicitly mentioned otherwise, mentions of table-row-groups in this spec also encompass the specialized
    // table-header-groups and table-footer-groups.
    return display.is_table_row_group()
        || display.is_table_header_group()
        || display.is_table_footer_group()
        || display.is_table_column_group();
}

static bool is_proper_table_child(Node const& node)
{
    auto const display = node.display();
    return is_table_track_group(display) || is_table_track(display) || display.is_table_caption();
}

static bool is_not_proper_table_child(Node const& node)
{
    if (!node.has_style())
        return true;
    return !is_proper_table_child(node);
}

static bool is_not_table_row(Node const& node)
{
    if (!node.has_style())
        return true;
    return !TableGrid::is_table_row(node);
}

static bool is_table_column(Node const& node)
{
    return node.display().is_table_column();
}

static bool is_table_cell(Node const& node)
{
    return node.display().is_table_cell();
}

static bool is_not_table_cell(Node const& node)
{
    if (!node.has_style())
        return true;
    return !is_table_cell(node);
}

static bool is_table_row_group_column_group_or_caption(Node const& node)
{
    auto const display = node.display();
    return is_table_track_group(display) || display.is_table_caption();
}

template<typename Matcher, typename Callback>
static void for_each_sequence_of_consecutive_children_matching(NodeWithStyle& parent, Matcher matcher, Callback callback)
{
    Vector<NonnullRefPtr<Node>> sequence;

    auto sequence_is_all_ignorable_whitespace = [&]() -> bool {
        for (auto& node : sequence) {
            if (!is_ignorable_whitespace(*node))
                return false;
        }
        return true;
    };

    for (auto child = parent.first_child(); child; child = child->next_sibling()) {
        if (matcher(*child) || (!sequence.is_empty() && is_ignorable_whitespace(*child))) {
            sequence.append(*child);
        } else {
            if (!sequence.is_empty()) {
                if (!sequence_is_all_ignorable_whitespace())
                    callback(sequence, child);
                sequence.clear();
            }
        }
    }
    if (!sequence.is_empty() && !sequence_is_all_ignorable_whitespace())
        callback(sequence, nullptr);
}

template<typename WrapperBoxType>
static void wrap_in_anonymous(Vector<NonnullRefPtr<Node>>& sequence, Node* nearest_sibling, CSS::Display display)
{
    VERIFY(!sequence.is_empty());
    auto& parent = *sequence.first()->parent();
    auto computed_values = parent.computed_values().clone_inherited_values();
    static_cast<CSS::MutableComputedValues&>(*computed_values).set_display(display);
    auto wrapper = make_ref_counted<WrapperBoxType>(parent.document(), nullptr, move(computed_values));
    for (auto& child : sequence) {
        parent.remove_child(*child);
        wrapper->append_child(*child);
    }
    wrapper->set_children_are_inline(parent.children_are_inline());
    if (nearest_sibling)
        parent.insert_before(*wrapper, *nearest_sibling);
    else
        parent.append_child(*wrapper);
}

// https://drafts.csswg.org/css-tables-3/#fixup-algorithm
// 2. Generate missing child wrappers:
void TreeBuilder::generate_missing_child_wrappers(NodeWithStyle& root)
{
    // 1. An anonymous table-row box must be generated around each sequence of consecutive children of a table-root box
    //    which are not proper table child boxes.
    for_each_in_tree_with_inside_display<CSS::DisplayInside::Table>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_proper_table_child, [&](auto sequence, auto nearest_sibling) {
            wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
        });
    });

    // 2. An anonymous table-row box must be generated around each sequence of consecutive children of a table-row-group
    //    box which are not table-row boxes.
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableRowGroup>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_table_row, [&](auto& sequence, auto nearest_sibling) {
            wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
        });
    });
    // Unless explicitly mentioned otherwise, mentions of table-row-groups in this spec also encompass the specialized
    // table-header-groups and table-footer-groups.
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableHeaderGroup>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_table_row, [&](auto& sequence, auto nearest_sibling) {
            wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
        });
    });
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableFooterGroup>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_table_row, [&](auto& sequence, auto nearest_sibling) {
            wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
        });
    });

    // 3. An anonymous table-cell box must be generated around each sequence of consecutive children of a table-row box
    //    which are not table-cell boxes.
    for_each_in_tree_with_internal_display<CSS::DisplayInternal::TableRow>(root, [&](auto& parent) {
        for_each_sequence_of_consecutive_children_matching(parent, is_not_table_cell, [&](auto& sequence, auto nearest_sibling) {
            wrap_in_anonymous<BlockContainer>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableCell });
        });
    });
}

// https://drafts.csswg.org/css-tables-3/#fixup-algorithm
// 3. Generate missing parents:
Vector<NonnullRefPtr<Box>> TreeBuilder::generate_missing_parents(NodeWithStyle& root)
{
    Vector<NonnullRefPtr<Box>> table_roots_to_wrap;
    root.for_each_in_inclusive_subtree_of_type<NodeWithStyle>([&](auto& parent) {
        // 1. An anonymous table-row box must be generated around each sequence of consecutive table-cell boxes whose
        //    parent is not a table-row.
        if (is_not_table_row(parent)) {
            for_each_sequence_of_consecutive_children_matching(parent, is_table_cell, [&](auto& sequence, auto nearest_sibling) {
                wrap_in_anonymous<Box>(sequence, nearest_sibling, CSS::Display { CSS::DisplayInternal::TableRow });
            });
        }

        // 2. An anonymous table or inline-table box must be generated around each sequence of consecutive proper table
        //    child boxes which are misparented.
        {
            // If the box’s parent is an inline, run-in, or ruby box (or any box that would perform inlinification of
            // its children), then an inline-table box must be generated; otherwise it must be a table box.
            // FIXME: run-in and ruby boxes
            auto display = CSS::Display::from_short(parent.display().is_inline_outside() ? CSS::Display::Short::InlineTable : CSS::Display::Short::Table);

            // A table-row is misparented if its parent is neither a table-row-group nor a table-root box.
            if (!TableGrid::is_table_row_group(parent) && !parent.display().is_table_inside()) {
                for_each_sequence_of_consecutive_children_matching(parent, TableGrid::is_table_row, [&](auto& sequence, auto nearest_sibling) {
                    wrap_in_anonymous<Box>(sequence, nearest_sibling, display);
                });
            }

            // A table-column box is misparented if its parent is neither a table-column-group box nor a table-root box.
            if (!TableGrid::is_table_column_group(parent) && !parent.display().is_table_inside()) {
                for_each_sequence_of_consecutive_children_matching(parent, is_table_column, [&](auto& sequence, auto nearest_sibling) {
                    wrap_in_anonymous<Box>(sequence, nearest_sibling, display);
                });
            }

            // A table-row-group, table-column-group, or table-caption box is misparented if its parent is not a table-root box.
            if (!parent.display().is_table_inside()) {
                for_each_sequence_of_consecutive_children_matching(parent, is_table_row_group_column_group_or_caption, [&](auto& sequence, auto nearest_sibling) {
                    wrap_in_anonymous<Box>(sequence, nearest_sibling, display);
                });
            }
        }

        // 3. An anonymous table-wrapper box must be generated around each table-root.
        if (auto* box = as_if<Box>(parent); box && box->display().is_table_inside()) {
            if (box->has_been_wrapped_in_table_wrapper()) {
                VERIFY(parent.parent());
                VERIFY(parent.parent()->is_table_wrapper());
                return TraversalDecision::Continue;
            }

            table_roots_to_wrap.append(*box);
        }

        return TraversalDecision::Continue;
    });

    for (auto& table_box : table_roots_to_wrap) {
        auto nearest_sibling = table_box->next_sibling();
        auto& parent = *table_box->parent();

        auto wrapper_computed_values = table_box->computed_values().clone_inherited_values();
        table_box->transfer_table_box_computed_values_to_wrapper_computed_values(*wrapper_computed_values);

        if (parent.is_table_wrapper()) {
            auto& existing_wrapper = static_cast<TableWrapper&>(parent);
            existing_wrapper.set_computed_values(move(wrapper_computed_values));
            continue;
        }

        auto wrapper = make_ref_counted<TableWrapper>(parent.document(), nullptr, move(wrapper_computed_values));

        parent.remove_child(*table_box);
        wrapper->append_child(*table_box);

        if (nearest_sibling)
            parent.insert_before(*wrapper, *nearest_sibling);
        else
            parent.append_child(*wrapper);

        table_box->set_has_been_wrapped_in_table_wrapper(true);
    }

    return table_roots_to_wrap;
}

static void fixup_row(Box& row_box, TableGrid const& table_grid, size_t row_index)
{
    for (size_t column_index = 0; column_index < table_grid.column_count(); ++column_index) {
        if (table_grid.occupancy_grid().contains({ column_index, row_index }))
            continue;

        auto computed_values = row_box.computed_values().clone_inherited_values();
        auto& mutable_computed_values = static_cast<CSS::MutableComputedValues&>(*computed_values);
        mutable_computed_values.set_display(Web::CSS::Display { CSS::DisplayInternal::TableCell });
        // Ensure that the cell (with zero content height) will have the same height as the row by setting vertical-align to middle.
        mutable_computed_values.set_vertical_align(CSS::VerticalAlign::Middle);
        auto cell_box = make_ref_counted<BlockContainer>(row_box.document(), nullptr, move(computed_values));
        row_box.append_child(cell_box);
    }
}

// https://drafts.csswg.org/css-tables-3/#missing-cells-fixup
void TreeBuilder::missing_cells_fixup(Vector<NonnullRefPtr<Box>> const& table_root_boxes)
{
    // Once the amount of columns in a table is known, any table-row box must be modified such that it owns enough
    // cells to fill all the columns of the table, when taking spans into account. New table-cell anonymous boxes must
    // be appended to its rows content until this condition is met.
    for (auto& table_box : table_root_boxes) {
        auto table_grid = TableGrid::calculate_row_column_grid(*table_box);
        size_t row_index = 0;
        TableGrid::for_each_child_box_matching(*table_box, TableGrid::is_table_row_group, [&](auto& row_group_box) {
            TableGrid::for_each_child_box_matching(row_group_box, TableGrid::is_table_row, [&](auto& row_box) {
                fixup_row(row_box, table_grid, row_index);
                ++row_index;
                return IterationDecision::Continue;
            });
        });

        TableGrid::for_each_child_box_matching(*table_box, TableGrid::is_table_row, [&](auto& row_box) {
            fixup_row(row_box, table_grid, row_index);
            ++row_index;
            return IterationDecision::Continue;
        });
    }
}

}
