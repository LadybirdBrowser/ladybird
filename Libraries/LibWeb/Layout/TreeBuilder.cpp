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
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/SVG/SVGSwitchElement.h>

namespace Web::Layout {

TreeBuilder::TreeBuilder() = default;

void TreeBuilder::note_tree_restructuring_at(Layout::Node const& node)
{
    if (m_current_rebuild_root && !m_current_rebuild_root->is_inclusive_ancestor_of(node))
        m_layout_tree_update_escaped_rebuild_roots = true;
}

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

static bool is_out_of_flow_table_internal_child_of_table_root(Layout::NodeWithStyle const& parent, Layout::Node const& child)
{
    auto const* child_with_style = as_if<Layout::NodeWithStyle>(child);
    return parent.display().is_table_inside()
        && child_with_style
        && !child.is_anonymous()
        && child_with_style->is_out_of_flow()
        && !child_with_style->has_replaced_element_table_display_adjustment()
        && child_with_style->display_before_box_type_transformation().is_internal_table();
}

static Optional<CSS::Display> adjusted_table_display_for_replaced_element(CSS::Display display)
{
    // https://drafts.csswg.org/css-tables-3/#table-structure
    // Replaced elements with a table-root display behave as block or inline depending on their
    // outer display type. Replaced elements with a table-internal display behave as inline.
    if (display.is_table_inside()) {
        if (display.is_block_outside())
            return CSS::Display::from_short(CSS::Display::Short::Block);
        return CSS::Display::from_short(CSS::Display::Short::Inline);
    }
    if (display.is_internal_table() || display.is_table_caption())
        return CSS::Display::from_short(CSS::Display::Short::Inline);
    return {};
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

static Layout::Node& insertion_parent_for_block_node(Layout::NodeWithStyle& layout_parent, Layout::Node& layout_node, TreeBuilder::AppendOrPrepend mode, TreeBuilder& tree_builder)
{
    // Inline is fine for in-flow block children (interrupting blocks) and for out-of-flow children;
    // the inline formatting context emits items for both.
    if (!layout_node.is_anonymous() && layout_parent.is_inline() && layout_parent.display().is_flow_inside())
        return layout_parent;

    // Make sure we're not inserting into an inline node, since those do not support block nodes.
    auto* new_parent = &layout_parent;
    while (is<InlineNode>(new_parent))
        new_parent = new_parent->parent();

    // If the parent block has no children, insert this block into parent.
    if (!has_inline_or_in_flow_block_children(*new_parent))
        return *new_parent;

    // Table-internal boxes may have been blockified before insertion, but table fixup still needs to see them as
    // direct table children instead of grouping them with neighboring table whitespace.
    if (is_out_of_flow_table_internal_child_of_table_root(*new_parent, layout_node))
        return *new_parent;

    // If the block is out-of-flow,
    if (layout_node.is_out_of_flow()) {
        // And we're appending while the parent's last child is an anonymous block, join that
        // anonymous block. Prepended boxes (e.g. an absolutely positioned ::before) belong at the
        // very start of the parent, not at the start of its trailing inline run.
        if (mode == TreeBuilder::AppendOrPrepend::Append
            && !new_parent->display().is_flex_inside()
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
    tree_builder.note_tree_restructuring_at(*new_parent);
    auto wrapper = new_parent->create_anonymous_wrapper();
    wrapper->set_children_are_inline(true);

    for (auto child = new_parent->first_child(); child;) {
        auto next_child = child->next_sibling();
        if (is_out_of_flow_table_internal_child_of_table_root(*new_parent, *child)) {
            child = next_child;
            continue;
        }
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
    auto& nearest_insertion_ancestor = *m_ancestor_stack.last();

    auto& insertion_point = display.is_inline_outside() ? insertion_parent_for_inline_node(nearest_insertion_ancestor)
                                                        : insertion_parent_for_block_node(nearest_insertion_ancestor, node, mode, *this);

    // Insertion parents can be above the subtree being rebuilt in place: inline ancestors are
    // skipped, and out-of-flow boxes can join a trailing anonymous sibling.
    note_tree_restructuring_at(insertion_point);

    if (mode == AppendOrPrepend::Prepend)
        insertion_point.prepend_child(node);
    else
        insertion_point.append_child(node);

    if (display.is_inline_outside()) {
        // After inserting an inline-level box into a parent, mark the parent as having inline children.
        insertion_point.set_children_are_inline(true);
    } else if (node.is_in_flow()) {
        // Inline-flow parents keep their inline children flag; their IFC may contain interrupting blocks.
        if (!as<NodeWithStyle>(insertion_point).display().is_inline_outside() || !as<NodeWithStyle>(insertion_point).display().is_flow_inside())
            insertion_point.set_children_are_inline(false);
    }
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
    auto const white_space_collapse = text_node.parent()->computed_values().white_space_collapse();
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
            if (node.is_fragmented_inline())
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
        if (auto* dom_element = as_if<DOM::Element>(inner_block->dom_node()); dom_element && dom_element->computed_values(CSS::PseudoElement::FirstLetter))
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
    if (!element.computed_values(CSS::PseudoElement::FirstLetter))
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

RefPtr<NodeWithStyle> TreeBuilder::create_pseudo_element_if_needed(DOM::Element& element, CSS::PseudoElement pseudo_element, Optional<AppendOrPrepend> insertion_mode)
{
    auto& document = element.document();

    // Clear stale layout nodes before deciding if this pseudo-element still generates one.
    if (auto existing_pseudo = element.get_synthetic_pseudo_element(pseudo_element); existing_pseudo.has_value() && existing_pseudo->layout_node())
        existing_pseudo->set_layout_node(nullptr);

    auto pseudo_element_values = element.computed_values(pseudo_element);
    if (!pseudo_element_values)
        return {};

    auto initial_quote_nesting_level = m_quote_nesting_level;
    DOM::AbstractElement element_reference { element, pseudo_element };
    auto [pseudo_element_content, final_quote_nesting_level] = pseudo_element_values->resolved_content(element_reference, initial_quote_nesting_level);
    m_quote_nesting_level = final_quote_nesting_level;
    auto pseudo_element_display = pseudo_element_values->display();

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
                NonnullRefPtr { *pseudo_element_values });
            list_item_marker->attach_style_resources();
            list_box->set_marker(list_item_marker);
            element.set_synthetic_pseudo_element_node({}, CSS::PseudoElement::Marker, list_item_marker);
            list_box->prepend_child(*list_item_marker);
            return list_item_marker;
        }

    RefPtr<NodeWithStyle> pseudo_element_node;
    if (pseudo_element_display.is_contents()) {
        pseudo_element_node = make_ref_counted<InlineNode>(document, nullptr, NonnullRefPtr { *pseudo_element_values });
        pseudo_element_node->modify_computed_values([](auto& values) {
            values.set_display(CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow));
        });
    } else {
        pseudo_element_node = DOM::Element::create_layout_node_for_display_type(document, pseudo_element_display, NonnullRefPtr { *pseudo_element_values }, nullptr);
        if (!pseudo_element_node)
            return {};
    }
    pseudo_element_node->attach_style_resources();

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
        list_item_marker->attach_style_resources();
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
    pseudo_element_node->modify_computed_values([&](auto& values) {
        values.set_content(pseudo_element_content);
    });

    CSS::resolve_counters(element_reference);
    // Now that we have counters, we can compute the content for real. Which is silly.
    if (pseudo_element_content.type == CSS::ContentData::Type::List) {
        auto [new_content, _] = pseudo_element_values->resolved_content(element_reference, initial_quote_nesting_level);
        pseudo_element_node->modify_computed_values([&](auto& values) {
            values.set_content(new_content);
        });

        // FIXME: Handle images, and multiple values
        if (new_content.type == CSS::ContentData::Type::List) {
            push_parent(*pseudo_element_node);
            for (auto& item : new_content.data) {
                RefPtr<Layout::Node> layout_node;
                if (auto const* string = item.get_pointer<Utf16String>()) {
                    layout_node = make_ref_counted<GeneratedTextNode>(document, *string);
                } else {
                    auto& image = *item.get<NonnullRefPtr<CSS::AbstractImageStyleValue>>();
                    layout_node = create_content_image_box(document, nullptr, NonnullRefPtr { pseudo_element_node->computed_values() }, image);
                    static_cast<NodeWithStyle&>(*layout_node).attach_style_resources();
                }
                layout_node->set_generated_for(pseudo_element, element);
                auto display = layout_node->is_text_node() ? CSS::Display::from_short(CSS::Display::Short::Inline) : as<NodeWithStyle>(*layout_node).display();
                insert_node_into_inline_or_block_ancestor(*layout_node, display, AppendOrPrepend::Append);
            }
            pop_parent();
        } else {
            TODO();
        }
    }

    return pseudo_element_node;
}

RefPtr<NodeWithStyle> TreeBuilder::create_content_replacement_if_needed(DOM::Element& element, NonnullRefPtr<CSS::ComputedValues const> computed_values) const
{
    auto const& content = computed_values->computed_content();
    if (content.type != CSS::ComputedContentData::Type::List
        || content.items.size() != 1
        || !content.items.first().has<NonnullRefPtr<CSS::AbstractImageStyleValue const>>()) {
        return {};
    }

    auto& image = const_cast<CSS::AbstractImageStyleValue&>(*content.items.first().get<NonnullRefPtr<CSS::AbstractImageStyleValue const>>());
    return create_content_image_box(element.document(), element, move(computed_values), image);
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
    // Assigned slottables are flat tree children of a slot, not DOM descendants.
    if (auto* slot_element = as_if<HTML::HTMLSlotElement>(element)) {
        for (auto const& slottable : slot_element->assigned_nodes_internal()) {
            slottable.visit([&](DOM::Node& slottable_root) {
                slottable_root.for_each_shadow_including_inclusive_descendant([&](auto& node) {
                    return clear_stale_layout_and_paint_node(node, &slottable_root);
                });
            });
        }
    }
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

void TreeBuilder::update_layout_tree(DOM::Node& dom_node, TreeBuilder::Context& context, MustCreateSubtree must_create_subtree)
{
    // NB: Called during layout tree construction.
    bool should_create_layout_node = must_create_subtree == MustCreateSubtree::Yes
        || dom_node.needs_layout_tree_update()
        || dom_node.document().needs_full_layout_tree_update()
        || (dom_node.is_document() && !dom_node.unsafe_layout_node());

    if (dom_node.is_element()) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        if (element.rendered_in_top_layer() && !context.layout_top_layer) {
            // A member found here without an attached box was cleared together with a hidden
            // ancestor subtree, and nothing is scheduled to rebuild it: request a top layer
            // zone rebuild, which runs as another update_layout pass and re-marks every member
            // itself. Marking the member here instead would strand dirty flags under ancestors
            // whose walks already finished, and a later update_layout would then treat the
            // detached member boxes as up to date.
            // NB: Called during layout tree construction.
            auto* element_layout_node = element.unsafe_layout_node();
            bool element_box_is_missing_or_detached = !element_layout_node || !element_layout_node->parent();
            if (element_box_is_missing_or_detached && !element.needs_layout_tree_update())
                element.document().set_top_layer_needs_layout_zone_rebuild();
            return;
        }
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
    RefPtr<CSS::ComputedValues const> computed_values;
    CSS::Display display;

    if (!should_create_layout_node) {
        if (is<DOM::Element>(dom_node)) {
            auto& element = static_cast<DOM::Element&>(dom_node);
            computed_values = element.computed_values();
            display = computed_values->display();
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
            if (auto old_backdrop_node = element.pseudo_element_unsafe_layout_node(CSS::PseudoElement::Backdrop)) {
                // A sibling-level mutation that runs before this element's own rebuild root is
                // established, so it always escapes the rebuilt subtrees.
                m_layout_tree_update_escaped_rebuild_roots = true;
                old_backdrop_node->remove();
            }
            element.clear_synthetic_pseudo_element_layout_nodes(Badge<TreeBuilder> {});
            // Elements inside a `display:none` subtree are skipped by
            // `Document::update_style_recursively`, so a bypass path (top-layer iteration, slot
            // projection, SVG mask/clip-path or pattern reference) may reach an element whose
            // `needs_style_update` flag is still set or whose `computed_values` is null. Route
            // through `update_style_for_element`, which seeds the style computer's ancestor filter
            // so descendant-combinator selectors continue to match during the lazy re-cascade.
            if (element.needs_style_update() || !element.computed_values()) {
                document.update_style_for_element({ element });
                element.set_needs_style_update(false);
            }
            computed_values = element.computed_values();
            display = computed_values->display();
            if (display.is_none())
                return;
            if (display.is_contents()) {
                should_clear_stale_layout_subtree_if_no_layout_node = false;
                update_layout_tree_for_display_contents(element, context, must_create_subtree, should_create_layout_node);
                return;
            }
            if (auto content_replacement = create_content_replacement_if_needed(element, NonnullRefPtr { *computed_values })) {
                layout_node = content_replacement.release_nonnull();
            } else if (context.layout_svg_mask_or_clip_path) {
                if (is<SVG::SVGMaskElement>(dom_node))
                    layout_node = make_ref_counted<Layout::SVGMaskBox>(document, static_cast<SVG::SVGMaskElement&>(dom_node), computed_values.release_nonnull());
                else if (is<SVG::SVGClipPathElement>(dom_node))
                    layout_node = make_ref_counted<Layout::SVGClipBox>(document, static_cast<SVG::SVGClipPathElement&>(dom_node), computed_values.release_nonnull());
                else
                    VERIFY_NOT_REACHED();
                // Only layout direct uses of SVG masks/clipPaths.
                context.layout_svg_mask_or_clip_path = false;
            } else if (context.layout_svg_pattern) {
                layout_node = make_ref_counted<Layout::SVGPatternBox>(document, as<SVG::SVGPatternElement>(dom_node), computed_values.release_nonnull());
                context.layout_svg_pattern = false;
            } else {
                layout_node = element.create_layout_node(computed_values.release_nonnull());
            }
        } else if (is<DOM::Document>(dom_node)) {
            auto document_style = style_computer.create_document_style();
            computed_values = move(document_style);
            display = computed_values->display();
            layout_node = make_ref_counted<Layout::Viewport>(static_cast<DOM::Document&>(dom_node), computed_values.release_nonnull());
        } else if (is<DOM::Text>(dom_node)) {
            auto& text_node = static_cast<DOM::Text&>(dom_node);
            layout_node = make_ref_counted<Layout::TextNode>(document, text_node);
            display = CSS::Display(CSS::DisplayOutside::Inline, CSS::DisplayInside::Flow);
            if (auto* style_parent = display_contents_style_parent_for_text_node(text_node); style_parent && display_contents_text_needs_style_wrapper(text_node, *style_parent)) {
                auto wrapper = make_ref_counted<Layout::InlineNode>(document, nullptr, style_parent->computed_values().release_nonnull());
                wrapper->attach_style_resources();
                wrapper->modify_computed_values([&](auto& values) {
                    values.set_display(display);
                });
                wrapper->set_children_are_inline(true);
                wrapper->append_child(*layout_node);
                layout_node = move(wrapper);
            }
        }
    }

    if (!layout_node)
        return;

    if (is<DOM::Element>(dom_node) || is<DOM::Document>(dom_node))
        as<NodeWithStyle>(*layout_node).attach_style_resources();

    if (layout_node->is_replaced_element()) {
        if (auto adjusted_display = adjusted_table_display_for_replaced_element(display); adjusted_display.has_value()) {
            display = *adjusted_display;
            as<NodeWithStyle>(*layout_node).modify_computed_values([&](auto& values) {
                values.set_display(display);
            });
        }
    }

    // Decide whether to replace an existing node (partial tree update) or insert a new one appropriately.
    bool const may_replace_existing_layout_node = must_create_subtree == MustCreateSubtree::No
        && old_layout_node
        && old_layout_node->parent()
        && old_layout_node != layout_node;
    Optional<TemporaryChange<Layout::Node*>> current_rebuild_root_change;
    if (may_replace_existing_layout_node && !m_current_rebuild_root) {
        current_rebuild_root_change.emplace(m_current_rebuild_root, layout_node.ptr());
        m_rebuilt_subtree_roots.append(layout_node.ptr());
    } else if (should_create_layout_node && !old_layout_node && !m_current_rebuild_root && !dom_node.is_document()) {
        // A fresh subtree that replaces no box in place (a ShadowRoot direct child, or a
        // top-layer element revealed from display:none) belongs to no rebuild root, so nothing
        // would lay its new boxes out on the partial path.
        m_layout_tree_update_escaped_rebuild_roots = true;
    }

    if (dom_node.is_element() && should_create_layout_node) {
        auto& element = static_cast<DOM::Element&>(dom_node);
        // Each element rendered in the top layer has a ::backdrop pseudo-element, for which it is the originating element.
        if (element.rendered_in_top_layer() && context.layout_top_layer) {
            // If we're inserting a new element, we can append the ::backdrop node now, before layout_node is appended.
            // Otherwise, we need to insert the ::backdrop before old_layout_node so it's behind the layout_node.
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
    }

    // A top layer member nested inside this member must be skipped at its normal position
    // like anywhere else in the walk, so its own turn of the pass builds its viewport box.
    Optional<TemporaryChange<bool>> layout_top_layer_cleared_for_member_descendants;
    if (auto* element = as_if<DOM::Element>(dom_node); element && element->rendered_in_top_layer() && context.layout_top_layer)
        layout_top_layer_cleared_for_member_descendants.emplace(context.layout_top_layer, false);

    if (dom_node.is_document()) {
        m_layout_root = layout_node;
    } else if (should_create_layout_node) {
        if (may_replace_existing_layout_node) {
            // The replacement box represents the same element in the same tree position, so the
            // layout inputs saved by the previous layout pass carry over to it.
            if (auto const* old_box = as_if<Box>(*old_layout_node)) {
                if (auto* new_box = as_if<Box>(*layout_node)) {
                    if (old_box->saved_abspos_layout_inputs())
                        new_box->set_saved_abspos_layout_inputs(*old_box->saved_abspos_layout_inputs());
                }
            }
            // A replaced node that participated in inline layout is referenced by the flat
            // fragment and inline-box-piece lists held by its containing block; repoint those
            // references at the replacement, since a subtree relayout that skips the containing
            // block never rebuilds them.
            if (auto* containing_block = old_layout_node->containing_block()) {
                if (auto* paintable_with_lines = as_if<Painting::PaintableWithLines>(containing_block->paintable().ptr())) {
                    for (auto& fragment : paintable_with_lines->fragments()) {
                        if (fragment.has_layout_node() && &fragment.layout_node() == old_layout_node.ptr())
                            fragment.set_layout_node(*layout_node);
                    }
                    for (auto& piece : paintable_with_lines->inline_box_pieces()) {
                        if (piece.node.ptr() == old_layout_node.ptr())
                            piece.node = layout_node.ptr();
                    }
                }
            }
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
            return element.computed_values()->content_visibility() == CSS::ContentVisibility::Hidden;
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
                if (auto* switch_element = as_if<SVG::SVGSwitchElement>(dom_node)) {
                    update_layout_tree_for_svg_switch_children(*switch_element, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
                } else {
                    // This is the same as as<DOM::ParentNode>(dom_node).for_each_child
                    for (auto* node = as<DOM::ParentNode>(dom_node).first_child(); node; node = node->next_sibling())
                        update_layout_tree(*node, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
                }
            }

            if (dom_node.is_document()) {
                // Elements in the top layer do not lay out normally based on their position in the document; instead they
                // generate boxes as if they were siblings of the root element.
                TemporaryChange<bool> layout_mask(context.layout_top_layer, true);
                for (auto const& top_layer_element : document.top_layer_elements()) {
                    if (!top_layer_element->rendered_in_top_layer())
                        continue;
                    if (element_has_an_unrendered_flat_tree_ancestor(top_layer_element)) {
                        top_layer_element->for_each_shadow_including_inclusive_descendant([&](auto& node) {
                            return clear_stale_layout_and_paint_node(node, top_layer_element.ptr());
                        });
                        continue;
                    }
                    update_layout_tree(top_layer_element, context, should_create_layout_node ? MustCreateSubtree::Yes : MustCreateSubtree::No);
                }
            }
            pop_parent();
        }
    }

    if (is<HTML::HTMLSlotElement>(dom_node)) {
        auto& slot_element = static_cast<HTML::HTMLSlotElement&>(dom_node);

        if (slot_element.computed_values()->content_visibility() != CSS::ContentVisibility::Hidden) {
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
    }

    // https://www.w3.org/TR/css-contain-2/#containment-style
    // Giving an element style containment has the following effects:
    // 2. The effects of the 'content' property’s 'open-quote', 'close-quote', 'no-open-quote' and 'no-close-quote' must
    //    be scoped to the element’s sub-tree.
    if (auto const* node_with_style = as_if<NodeWithStyle>(*layout_node); node_with_style && node_with_style->has_style_containment()) {
        m_quote_nesting_level = prior_quote_nesting_level;
    }

    dom_node.set_needs_layout_tree_update(false, DOM::SetNeedsLayoutTreeUpdateReason::None);
    dom_node.set_child_needs_layout_tree_update(false);
}

void TreeBuilder::update_layout_tree_for_display_contents(DOM::Element& element, TreeBuilder::Context& context, MustCreateSubtree must_create_subtree, bool should_create_layout_node)
{
    // A display:contents member builds its children through this path, so the top layer flag
    // is consumed here the same way update_layout_tree does for members with a box.
    Optional<TemporaryChange<bool>> layout_top_layer_cleared_for_member_descendants;
    if (element.rendered_in_top_layer() && context.layout_top_layer)
        layout_top_layer_cleared_for_member_descendants.emplace(context.layout_top_layer, false);

    element.clear_synthetic_pseudo_element_layout_nodes(Badge<TreeBuilder> {});

    if (should_create_layout_node) {
        element.for_each_shadow_including_inclusive_descendant([&](auto& node) {
            return clear_stale_layout_and_paint_node(node);
        });

        DOM::AbstractElement element_reference { element };
        CSS::resolve_counters(element_reference);
    }

    auto element_has_content_visibility_hidden = element.computed_values()->content_visibility() == CSS::ContentVisibility::Hidden;
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

void TreeBuilder::update_layout_tree_for_svg_switch_children(SVG::SVGSwitchElement& switch_element, Context& context, MustCreateSubtree must_create_subtree)
{
    // https://svgwg.org/svg2-draft/struct.html#SwitchElement
    // The ‘switch’ element evaluates the ‘requiredExtensions’ and ‘systemLanguage’ attributes on its direct child
    // elements in order, and then processes and renders the first child for which these attributes evaluate to true.
    // All others will be bypassed and therefore not rendered. If the child element is a container element such as a
    // ‘g’, then the entire subtree is either processed/rendered or bypassed/not rendered.

    auto* rendered_child = [&] -> DOM::Node* {
        for (auto* node = switch_element.first_child_of_type<SVG::SVGElement>(); node; node = node->next_sibling_of_type<SVG::SVGElement>()) {
            // FIXME: Evaluate the requiredExtensions and systemLanguage attributes.
            return node;
        }
        return nullptr;
    }();

    // NB: Clean up any stale children that should no longer be rendered.
    switch_element.for_each_child([&](DOM::Node& child_node) {
        if (&child_node != rendered_child)
            clear_stale_layout_and_paint_node(child_node);
        return IterationDecision::Continue;
    });

    if (rendered_child)
        update_layout_tree(*rendered_child, context, must_create_subtree);
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
    auto display = as<NodeWithStyle>(layout_node).display();
    if (!display.is_grid_inside() && !display.is_flex_inside()) {
        auto& parent = as<NodeWithStyle>(layout_node);

        // If the box does not overflow in the vertical axis, then it is centered vertically.
        // FIXME: Only apply alignment when box overflows
        auto flex_wrapper = parent.create_anonymous_wrapper();
        flex_wrapper->modify_computed_values([](auto& values) {
            values.set_display(CSS::Display { CSS::DisplayOutside::Block, CSS::DisplayInside::Flex });
            values.set_justify_content(CSS::JustifyContent::Center);
            values.set_flex_direction(CSS::FlexDirection::Column);
            values.set_height(CSS::Size::make_percentage(CSS::Percentage(100)));
        });

        auto content_box_wrapper = parent.create_anonymous_wrapper();
        // Let percentage-sized descendants shrink to fixed-height buttons instead of the flex
        // item's automatic minimum size.
        content_box_wrapper->modify_computed_values([](auto& values) {
            values.set_min_height(CSS::Size::make_px(CSSPixels(0)));
        });
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
            const_cast<SVG::SVGElement&>(*mask_or_clip_path).register_resource_box_referencing_element({}, graphics_element);
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
            // NB: The referenced pattern may inherit its content from another pattern via href. Removing either
            //     element invalidates the attached resource box, so register the referencer with both.
            const_cast<SVG::SVGPatternElement&>(*content_element).register_resource_box_referencing_element({}, graphics_element);
            if (pattern != content_element)
                const_cast<SVG::SVGPatternElement&>(*pattern).register_resource_box_referencing_element({}, graphics_element);
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
            wrapper->modify_computed_values([](auto& values) {
                values.set_display(CSS::Display::from_short(CSS::Display::Short::FlowRoot));
            });

            // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
            // The following properties are expected to inherit from the fieldset element:
            //     align-content, align-items, border-radius, column-count, column-fill, column-gap, column-rule,
            //     column-width, flex-direction, flex-wrap, grid (grid-auto-columns, grid-auto-flow, grid-auto-rows,
            //     grid-column-gap, grid-row-gap, grid-template-areas, grid-template-columns, grid-template-rows),
            //     justify-content, justify-items, overflow, padding, text-overflow, unicode-bidi
            // FIXME: Transfer all of these properties, not just overflow.
            wrapper->modify_computed_values([&](auto& values) {
                values.set_overflow_x(fieldset_box->computed_values().overflow_x());
                values.set_overflow_y(fieldset_box->computed_values().overflow_y());
            });
            fieldset_box->modify_computed_values([](auto& values) {
                values.set_overflow_x(CSS::InitialValues::overflow());
                values.set_overflow_y(CSS::InitialValues::overflow());
            });

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

static bool is_first_or_last_child_with_table_non_root_sibling_if_any(Node const& node)
{
    auto is_table_non_root_box = [](Node const& node) {
        auto const* node_with_style = as_if<NodeWithStyle>(node);
        if (!node_with_style)
            return false;
        auto const display = node_with_style->display();
        return display.is_table_row()
            || display.is_table_column()
            || display.is_table_row_group()
            || display.is_table_header_group()
            || display.is_table_footer_group()
            || display.is_table_column_group()
            || display.is_table_cell()
            || display.is_table_caption();
    };

    auto previous_sibling = node.previous_sibling();
    auto next_sibling = node.next_sibling();
    if (previous_sibling && next_sibling)
        return false;

    if (previous_sibling && !is_table_non_root_box(*previous_sibling))
        return false;

    if (next_sibling && !is_table_non_root_box(*next_sibling))
        return false;

    return true;
}

// https://drafts.csswg.org/css-tables-3/#tabular-container
static bool is_tabular_container(Node const& node)
{
    auto const* node_with_style = as_if<NodeWithStyle>(node);
    if (!node_with_style)
        return false;
    auto const& display = node_with_style->display();
    return display.is_table_inside()
        || display.is_table_row()
        || display.is_table_row_group()
        || display.is_table_header_group()
        || display.is_table_footer_group();
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
            auto const* child_with_style = as_if<NodeWithStyle>(child);
            if (!child_with_style || !child_with_style->display().is_table_column())
                to_remove.append(child);
            return IterationDecision::Continue;
        });
    });

    // FIXME: 3. Anonymous inline boxes which contain only white space and are between two immediate siblings each of
    //           which is a table-non-root box.

    // 4. Anonymous inline boxes which meet all of the following criteria:
    //    - they contain only white space
    //    - they are the first and/or last child of a tabular container
    //    - whose immediate sibling, if any, is a table-non-root box
    root.for_each_in_inclusive_subtree_of_type<Box>([&](auto& box) {
        auto* parent = box.parent();
        if (!parent
            || !is_tabular_container(*parent)
            || !is_first_or_last_child_with_table_non_root_sibling_if_any(box)) {
            return TraversalDecision::Continue;
        }

        if (is_ignorable_whitespace(box)) {
            to_remove.append(box);
            return TraversalDecision::SkipChildrenAndContinue;
        }
        return TraversalDecision::Continue;
    });

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

static CSS::Display display_for_table_fixup(NodeWithStyle const& node)
{
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // For the purposes of these rules, out-of-flow elements are represented as inline elements of zero width and
    // height. Their containing blocks are chosen accordingly.
    //
    // AD-HOC: Table-internal boxes can be blockified before fixup. Use the pre-transformation display for authored
    // boxes so an out-of-flow table-header-group is still recognized as a proper table child during fixup.
    if (node.has_replaced_element_table_display_adjustment())
        return node.display();
    if (!node.is_anonymous())
        return node.display_before_box_type_transformation();
    return node.display();
}

static bool is_proper_table_child(NodeWithStyle const& node)
{
    auto const display = display_for_table_fixup(node);
    return is_table_track_group(display) || is_table_track(display) || display.is_table_caption();
}

static bool is_not_proper_table_child(Node const& node)
{
    auto const* node_with_style = as_if<NodeWithStyle>(node);
    if (!node_with_style)
        return true;
    return !is_proper_table_child(*node_with_style);
}

static bool is_not_table_row(Node const& node)
{
    auto const* node_with_style = as_if<NodeWithStyle>(node);
    if (!node_with_style)
        return true;
    return !TableGrid::is_table_row(*node_with_style);
}

static bool is_table_column(Node const& node)
{
    auto const* node_with_style = as_if<NodeWithStyle>(node);
    return node_with_style && node_with_style->display().is_table_column();
}

static bool is_table_cell(Node const& node)
{
    auto const* node_with_style = as_if<NodeWithStyle>(node);
    return node_with_style && node_with_style->display().is_table_cell();
}

static bool is_not_table_cell(Node const& node)
{
    if (!node.has_style())
        return true;
    return !is_table_cell(node);
}

static bool is_table_row_group_column_group_or_caption(Node const& node)
{
    auto const* node_with_style = as_if<NodeWithStyle>(node);
    if (!node_with_style)
        return false;
    auto const display = display_for_table_fixup(*node_with_style);
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
    auto builder = CSS::ComputedValues::Builder::create_inheriting_from(parent.computed_values());
    builder->set_display(display);
    auto wrapper = make_ref_counted<WrapperBoxType>(parent.document(), nullptr, move(builder).build());
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

        auto builder = CSS::ComputedValues::Builder::create_inheriting_from(table_box->computed_values());
        table_box->transfer_table_box_computed_values_to_wrapper_computed_values(builder);
        auto wrapper_computed_values = move(builder).build();

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

        auto builder = CSS::ComputedValues::Builder::create_inheriting_from(row_box.computed_values());
        builder->set_display(Web::CSS::Display { CSS::DisplayInternal::TableCell });
        // Ensure that the cell (with zero content height) will have the same height as the row by setting vertical-align to middle.
        builder->set_vertical_align(CSS::VerticalAlign::Middle);
        auto cell_box = make_ref_counted<BlockContainer>(row_box.document(), nullptr, move(builder).build());
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
