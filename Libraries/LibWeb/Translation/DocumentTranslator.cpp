/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/CharacterTypes.h>
#include <AK/ScopeGuard.h>
#include <AK/TemporaryChange.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/ARIA/AttributeNames.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationType.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLTitleElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/TagNames.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/Translation/DocumentTranslator.h>

namespace Web::Translation {

// Lower is more urgent: The title first, then the text the user can see, then the rest of the rendered text, and the
// text that isn't rendered last, so a menu shown later is still translated.
enum class PriorityClass : u64 {
    Title,
    InViewport,
    Rendered,
    NotRendered,
};

static ReadonlySpan<Utf16FlyString const*> translated_attributes()
{
    static auto const names = to_array<Utf16FlyString const*>({
        &HTML::AttributeNames::title,
        &HTML::AttributeNames::alt,
        &HTML::AttributeNames::placeholder,
        &HTML::AttributeNames::value,
        &ARIA::AttributeNames::aria_label,
        &ARIA::AttributeNames::aria_description,
        &ARIA::AttributeNames::aria_placeholder,
        &ARIA::AttributeNames::aria_role_description,
        &ARIA::AttributeNames::aria_value_text,
    });
    return names;
}

// Attributes whose change can decide whether, or how, an element's text is translated.
static ReadonlySpan<Utf16FlyString const*> attributes_that_change_translation()
{
    static auto const names = to_array<Utf16FlyString const*>({
        &HTML::AttributeNames::class_,
        &HTML::AttributeNames::contenteditable,
        &HTML::AttributeNames::hidden,
        &HTML::AttributeNames::lang,
        &HTML::AttributeNames::style,
        &HTML::AttributeNames::translate,
    });
    return names;
}

static bool is_same_language(StringView a, StringView b)
{
    auto primary_subtag = [](StringView language) {
        if (auto separator = language.find_any_of("-_"sv); separator.has_value())
            return language.substring_view(0, *separator);
        return language;
    };
    return primary_subtag(a).equals_ignoring_ascii_case(primary_subtag(b));
}

static bool is_html_element_named(DOM::Element const& element, Utf16FlyString const& name)
{
    return element.namespace_uri() == Namespace::HTML && element.local_name() == name;
}

static bool is_attribute_translated(DOM::Element const& element, Utf16FlyString const& name)
{
    if (name == HTML::AttributeNames::title)
        return true;
    if (name == ARIA::AttributeNames::aria_label || name == ARIA::AttributeNames::aria_description || name == ARIA::AttributeNames::aria_placeholder
        || name == ARIA::AttributeNames::aria_role_description || name == ARIA::AttributeNames::aria_value_text)
        return true;
    if (name == HTML::AttributeNames::alt)
        return is_html_element_named(element, HTML::TagNames::img) || is_html_element_named(element, HTML::TagNames::area) || is_html_element_named(element, HTML::TagNames::input);
    if (name == HTML::AttributeNames::placeholder)
        return is_html_element_named(element, HTML::TagNames::input) || is_html_element_named(element, HTML::TagNames::textarea);
    if (name == HTML::AttributeNames::value) {
        auto const* input = as_if<HTML::HTMLInputElement>(element);
        if (!input)
            return false;
        switch (input->type_state()) {
        case HTML::HTMLInputElement::TypeAttributeState::Button:
        case HTML::HTMLInputElement::TypeAttributeState::ResetButton:
            return true;
        case HTML::HTMLInputElement::TypeAttributeState::SubmitButton:
            // A named submit button's value is submitted with its form, which translating it would change.
            return !input->has_attribute(HTML::AttributeNames::name);
        default:
            return false;
        }
    }
    return false;
}

// Whether the text has anything to translate, rather than only spaces, digits, and punctuation.
static bool has_translatable_text(Utf16View const& text)
{
    return any_of(text, [](u32 code_point) { return Unicode::code_point_has_letter_general_category(code_point); });
}

// Collapsible white space reads as one space, so it's sent as one.
static Utf16String text_to_send(DOM::Text& text)
{
    auto const& data = text.data();
    auto white_space_collapse = Editing::resolved_keyword(text, CSS::PropertyID::WhiteSpaceCollapse);
    if (white_space_collapse.has_value() && *white_space_collapse != CSS::Keyword::Collapse)
        return data;

    Utf16StringBuilder builder;
    bool last_was_space = false;
    for (auto code_point : data.utf16_view()) {
        if (is_ascii_space(code_point)) {
            if (!last_was_space)
                builder.append_code_point(' ');
            last_was_space = true;
            continue;
        }
        builder.append_code_point(code_point);
        last_was_space = false;
    }
    return builder.to_string();
}

static Utf16String joined_text(Vector<Web::TranslationRun> const& runs)
{
    Utf16StringBuilder builder;
    for (auto const& run : runs)
        builder.append(run.text);
    return builder.to_string();
}

// Content that's never shown as text, or that isn't text at all, e.g. a script or an image.
static bool is_untranslatable_content(DOM::Element const& element)
{
    if (is<SVG::SVGElement>(element) || is<MathML::MathMLElement>(element))
        return true;
    static auto const tags = to_array<Utf16FlyString const*>({
        &HTML::TagNames::audio,
        &HTML::TagNames::base,
        &HTML::TagNames::canvas,
        &HTML::TagNames::datalist,
        &HTML::TagNames::embed,
        &HTML::TagNames::frame,
        &HTML::TagNames::head,
        &HTML::TagNames::iframe,
        &HTML::TagNames::img,
        &HTML::TagNames::input,
        &HTML::TagNames::link,
        &HTML::TagNames::meta,
        &HTML::TagNames::meter,
        &HTML::TagNames::noscript,
        &HTML::TagNames::object,
        &HTML::TagNames::param,
        &HTML::TagNames::picture,
        &HTML::TagNames::progress,
        &HTML::TagNames::script,
        &HTML::TagNames::select,
        &HTML::TagNames::source,
        &HTML::TagNames::style,
        &HTML::TagNames::template_,
        &HTML::TagNames::textarea,
        &HTML::TagNames::title,
        &HTML::TagNames::track,
        &HTML::TagNames::video,
    });
    return element.namespace_uri() == Namespace::HTML && any_of(tags, [&](auto const* tag) { return element.local_name() == *tag; });
}

// An element with no style of its own, e.g. in a hidden subtree, is taken to be laid out the way it is by default.
static bool is_block_by_default(DOM::Element const& element)
{
    static auto const tags = to_array<Utf16FlyString const*>({
        &HTML::TagNames::address,
        &HTML::TagNames::article,
        &HTML::TagNames::aside,
        &HTML::TagNames::blockquote,
        &HTML::TagNames::body,
        &HTML::TagNames::button,
        &HTML::TagNames::caption,
        &HTML::TagNames::dd,
        &HTML::TagNames::details,
        &HTML::TagNames::dialog,
        &HTML::TagNames::div,
        &HTML::TagNames::dl,
        &HTML::TagNames::dt,
        &HTML::TagNames::fieldset,
        &HTML::TagNames::figcaption,
        &HTML::TagNames::figure,
        &HTML::TagNames::footer,
        &HTML::TagNames::form,
        &HTML::TagNames::h1,
        &HTML::TagNames::h2,
        &HTML::TagNames::h3,
        &HTML::TagNames::h4,
        &HTML::TagNames::h5,
        &HTML::TagNames::h6,
        &HTML::TagNames::header,
        &HTML::TagNames::hgroup,
        &HTML::TagNames::hr,
        &HTML::TagNames::html,
        &HTML::TagNames::legend,
        &HTML::TagNames::li,
        &HTML::TagNames::main,
        &HTML::TagNames::menu,
        &HTML::TagNames::nav,
        &HTML::TagNames::ol,
        &HTML::TagNames::p,
        &HTML::TagNames::pre,
        &HTML::TagNames::search,
        &HTML::TagNames::section,
        &HTML::TagNames::summary,
        &HTML::TagNames::table,
        &HTML::TagNames::tbody,
        &HTML::TagNames::td,
        &HTML::TagNames::tfoot,
        &HTML::TagNames::th,
        &HTML::TagNames::thead,
        &HTML::TagNames::tr,
        &HTML::TagNames::ul,
    });
    return element.namespace_uri() == Namespace::HTML && any_of(tags, [&](auto const* tag) { return element.local_name() == *tag; });
}

// Content that loses its state when it's taken out of the document: An iframe's document is discarded, a video pauses,
// and a form control loses its focus. A paragraph rebuilt around it would take it out, so it ends the paragraph instead.
static bool is_stateful_content(DOM::Element const& element)
{
    static auto const tags = to_array<Utf16FlyString const*>({
        &HTML::TagNames::audio,
        &HTML::TagNames::embed,
        &HTML::TagNames::frame,
        &HTML::TagNames::iframe,
        &HTML::TagNames::input,
        &HTML::TagNames::object,
        &HTML::TagNames::select,
        &HTML::TagNames::textarea,
        &HTML::TagNames::video,
    });
    return element.namespace_uri() == Namespace::HTML && any_of(tags, [&](auto const* tag) { return element.local_name() == *tag; });
}

static bool is_hidden(DOM::Element const& element)
{
    auto display = Editing::resolved_display(const_cast<DOM::Element&>(element));
    return !display.has_value() || display->is_none();
}

// The child of parent that node is in, if node is in parent at all.
static GC::Ptr<DOM::Node> child_containing(DOM::Node& parent, DOM::Node& node)
{
    for (GC::Ptr<DOM::Node> current = node; current; current = current->parent()) {
        if (current->parent() == &parent)
            return current;
    }
    return nullptr;
}

DocumentTranslator::DocumentTranslator(DOM::Document& document)
    : m_document(document)
{
}

void DocumentTranslator::Unit::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(container);
    visitor.visit(range);
    visitor.visit(nodes);
    for (auto& piece : pieces) {
        visitor.visit(piece.node);
        visitor.visit(piece.owner);
    }
    visitor.visit(inline_elements);
    for (auto& anchored_node : anchored_nodes) {
        visitor.visit(anchored_node.node);
        visitor.visit(anchored_node.owner);
    }
    visitor.visit(element);
}

void DocumentTranslator::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
    for (auto& it : m_units)
        it.value.visit_edges(visitor);
}

void DocumentTranslator::start(String source_language, String target_language)
{
    if (is_started())
        return;
    m_source_language = move(source_language);
    m_target_language = move(target_language);

    // The page's text is in the target language once it's translated, which decides e.g. its fonts and hyphenation.
    if (auto* document_element = m_document->document_element()) {
        TemporaryChange applying { m_is_applying, true };
        document_element->set_attribute_value(HTML::AttributeNames::lang, Utf16String::from_utf8(m_target_language->bytes_as_string_view()));
    }

    scan_title();
    if (auto* document_element = m_document->document_element())
        scan(*document_element);
    m_document->page().notify_translation_progress();
}

// Builds the paragraphs of one container. Each paragraph spans a run of the container's children that flow inline.
class DocumentTranslator::ParagraphBuilder {
public:
    ParagraphBuilder(DocumentTranslator& translator, DOM::Node& container)
        : m_translator(translator)
        , m_container(container)
    {
    }

    // Adds a child of the container that flows inline: text, an inline element, or a node anchored among them.
    void add_child(DOM::Node& child, NodeRole role)
    {
        if (role == NodeRole::Anchored) {
            // A node before the paragraph's first text isn't part of the paragraph.
            if (!m_unit_id.has_value()) {
                m_translator.scan_anchored(child);
                return;
            }
            unit().range.append(child);
            add_anchored(child);
            ++m_trailing_anchored_count;
            return;
        }
        if (!m_unit_id.has_value())
            m_unit_id = m_translator.create_unit(UnitKind::Paragraph);
        unit().range.append(child);
        m_trailing_anchored_count = 0;
        add_node(child, role);
    }

    void finish()
    {
        ScopeGuard reset = [&] {
            m_unit_id.clear();
            m_open_elements.clear();
            m_last_token_inside.clear();
            m_anchored_awaiting_text.clear();
            m_trailing_anchored_count = 0;
        };
        if (!m_unit_id.has_value())
            return;
        auto& unit = this->unit();

        // A paragraph ends at its last text: The nodes after it stay where they are.
        for (; m_trailing_anchored_count > 0; --m_trailing_anchored_count) {
            unit.range.take_last();
            unit.nodes.take_last();
            unit.anchored_nodes.take_last();
        }

        // A paragraph whose translation is on its way isn't sent again. If the page changed it, the translation is
        // dropped as it comes back, and the paragraph is scanned again then.
        auto has_new_text = any_of(unit.runs, [](auto const& run) { return !run.keep_as_is && has_translatable_text(run.text); });
        if (!has_new_text || m_translator.is_paragraph_in_flight(unit.range)) {
            m_translator.m_units.remove(*m_unit_id);
            return;
        }
        for (auto& anchored_node : unit.anchored_nodes)
            anchored_node.following_token = first_token_inside(unit, anchored_node.owner, anchored_node.following_token);
        unit.container = m_container;
    }

private:
    Unit& unit() { return m_translator.unit(*m_unit_id); }

    Optional<size_t> current_owner_index() const
    {
        if (m_open_elements.is_empty())
            return {};
        return m_open_elements.last();
    }

    GC::Ptr<DOM::Element> current_owner()
    {
        if (auto index = current_owner_index(); index.has_value())
            return unit().inline_elements[*index];
        return nullptr;
    }

    void add_node(DOM::Node& node, NodeRole role)
    {
        unit().nodes.append(node);

        if (auto* text = as_if<DOM::Text>(node)) {
            // Text the translator wrote goes along for context, but isn't translated again.
            auto is_translated = m_translator.is_translated_text(*text);
            add_piece({ node, current_owner(), current_owner_index(), text->data(), is_translated }, text_to_send(*text));
            return;
        }

        auto& element = as<DOM::Element>(node);
        m_translator.scan_attributes(element);
        if (role == NodeRole::Kept) {
            add_piece({ node, current_owner(), current_owner_index(), {}, true }, element.text_content().value_or({}));
            return;
        }

        auto& unit = this->unit();
        unit.inline_elements.append(element);
        unit.inline_element_parents.append(current_owner_index());
        m_open_elements.append(unit.inline_elements.size() - 1);
        for (auto* child = element.first_child(); child; child = child->next_sibling()) {
            auto child_role = m_translator.role_of(*child);
            if (child_role == NodeRole::Anchored)
                add_anchored(*child);
            else
                add_node(*child, child_role);
        }
        m_open_elements.take_last();
    }

    void add_piece(Piece piece, Utf16String text)
    {
        auto& unit = this->unit();
        auto starts_with_space = !text.is_empty() && is_ascii_space(text.code_unit_at(0));
        for (auto index : m_anchored_awaiting_text)
            unit.anchored_nodes[index].space_after = starts_with_space;
        m_anchored_awaiting_text.clear();
        auto keep_as_is = piece.keep_as_is;
        unit.pieces.append(move(piece));
        auto token = static_cast<u32>(unit.pieces.size());
        unit.runs.append({ move(text), token, keep_as_is });
        // The token is inside the container, and inside every inline element open around it.
        m_last_token_inside.set(CONTAINER, token);
        for (auto index : m_open_elements)
            m_last_token_inside.set(index, token);
    }

    void add_anchored(DOM::Node& node)
    {
        m_translator.scan_anchored(node);
        auto& unit = this->unit();
        unit.nodes.append(node);
        auto owner_index = current_owner_index();
        auto preceding_token = m_last_token_inside.get(owner_index.value_or(CONTAINER)).value_or(0);
        auto space_before = !unit.runs.is_empty() && !unit.runs.last().text.is_empty()
            && is_ascii_space(unit.runs.last().text.code_unit_at(unit.runs.last().text.length_in_code_units() - 1));
        // NB: The following token is found once every token is known; until then, this is the number of tokens so far.
        unit.anchored_nodes.append({ node, current_owner(), owner_index, preceding_token, static_cast<u32>(unit.pieces.size()), space_before, false });
        m_anchored_awaiting_text.append(unit.anchored_nodes.size() - 1);
    }

    // The first token after the given one whose text is inside the owner; 0 if there's none.
    static u32 first_token_inside(Unit const& unit, GC::Ptr<DOM::Element> owner, u32 after_token)
    {
        for (size_t index = after_token; index < unit.pieces.size(); ++index) {
            if (!owner || owner->is_ancestor_of(*unit.pieces[index].node))
                return static_cast<u32>(index + 1);
        }
        return 0;
    }

    // The key of the container itself, among the keys of its inline elements, which are their indices.
    static constexpr size_t CONTAINER = NumericLimits<size_t>::max();

    DocumentTranslator& m_translator;
    GC::Ref<DOM::Node> m_container;
    Optional<u64> m_unit_id;
    Vector<size_t> m_open_elements;
    HashMap<size_t, u32> m_last_token_inside;
    Vector<size_t> m_anchored_awaiting_text;
    size_t m_trailing_anchored_count { 0 };
};

DocumentTranslator::NodeRole DocumentTranslator::role_of(DOM::Node const& node) const
{
    if (is<DOM::Text>(node))
        return NodeRole::Text;
    auto const* element = as_if<DOM::Element>(node);
    if (element && (is_html_element_named(*element, HTML::TagNames::br) || is_stateful_content(*element)))
        return NodeRole::Break;
    if (!element || is_untranslatable_content(*element))
        return NodeRole::Anchored;

    auto flows_inline = is_inline(*element);
    // An element with a shadow tree shows that tree's content, which has paragraphs of its own. And a custom element
    // runs its callbacks again when it's taken out and put back, which may make it render its content again. So its
    // text is translated where it is, rather than in the paragraph around it.
    if (auto shadow_root = element->shadow_root(); (shadow_root && !shadow_root->is_user_agent_internal()) || element->is_custom())
        flows_inline = false;
    // So does an inline element that holds a block or a line break.
    if (flows_inline && contains_paragraph_boundary(*element))
        flows_inline = false;

    if (is_excluded(*element))
        return flows_inline ? NodeRole::Kept : NodeRole::Excluded;
    if (!flows_inline)
        return NodeRole::Container;

    // An inline element that can't be seen, or that has no text, e.g. a link around an image, stays as it is.
    if (is_hidden(*element))
        return NodeRole::Anchored;
    bool has_text = false;
    element->for_each_in_subtree([&](DOM::Node const& descendant) {
        if (auto const* descendant_element = as_if<DOM::Element>(descendant); descendant_element && is_untranslatable_content(*descendant_element))
            return TraversalDecision::SkipChildrenAndContinue;
        if (auto const* text = as_if<DOM::Text>(descendant); text && has_translatable_text(text->data())) {
            has_text = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return has_text ? NodeRole::Inline : NodeRole::Anchored;
}

bool DocumentTranslator::is_inline(DOM::Element const& element) const
{
    auto display = Editing::resolved_display(const_cast<DOM::Element&>(element));
    if (!display.has_value() || display->is_none())
        return !is_block_by_default(element);
    return (display->is_inline_outside() && display->is_flow_inside()) || display->is_contents();
}

bool DocumentTranslator::contains_paragraph_boundary(DOM::Element const& element) const
{
    bool contains_boundary = false;
    element.for_each_in_subtree([&](DOM::Node const& descendant) {
        auto const* descendant_element = as_if<DOM::Element>(descendant);
        if (!descendant_element)
            return TraversalDecision::Continue;
        if (is_stateful_content(*descendant_element)) {
            contains_boundary = true;
            return TraversalDecision::Break;
        }
        if (is_untranslatable_content(*descendant_element))
            return TraversalDecision::SkipChildrenAndContinue;
        // An element with a shadow tree shows that tree's content, which has paragraphs of its own.
        auto shadow_root = descendant_element->shadow_root();
        auto has_author_shadow_tree = shadow_root && !shadow_root->is_user_agent_internal();
        if (is_html_element_named(*descendant_element, HTML::TagNames::br) || has_author_shadow_tree || descendant_element->is_custom() || !is_inline(*descendant_element)) {
            contains_boundary = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return contains_boundary;
}

bool DocumentTranslator::is_excluded(DOM::Element const& element) const
{
    if (element.translation_mode() == DOM::Element::TranslationMode::NoTranslate)
        return true;
    // Sites mark text that machine translation should leave alone with this class.
    if (element.has_class(u"notranslate"sv))
        return true;
    // Computer code, keyboard input, sample output, and variable names read the same in any language.
    for (auto const* tag : { &HTML::TagNames::code, &HTML::TagNames::kbd, &HTML::TagNames::samp, &HTML::TagNames::var }) {
        if (is_html_element_named(element, *tag))
            return true;
    }
    // Translating text the user edits would change what they wrote.
    if (element.is_editable_or_editing_host())
        return true;
    // Text marked as being in another language isn't the page's language, so the translation can't translate it. The
    // root's language isn't trusted for this, since the page's text may say otherwise.
    if (&element != m_document->document_element()) {
        if (auto language = element.get_attribute(HTML::AttributeNames::lang); language.has_value() && !language->is_empty()) {
            auto language_utf8 = language->to_utf8_but_should_be_ported_to_utf16();
            if (!is_same_language(language_utf8.bytes_as_string_view(), m_source_language->bytes_as_string_view()))
                return true;
        }
    }
    return false;
}

bool DocumentTranslator::is_translated_text(DOM::Text const& text) const
{
    return m_translated_text.contains(const_cast<DOM::Text&>(text));
}

void DocumentTranslator::scan(DOM::Node& root)
{
    scan_container(root);
}

void DocumentTranslator::scan_container(DOM::Node& container)
{
    if (auto* element = as_if<DOM::Element>(container)) {
        scan_attributes(*element);
        if (auto shadow_root = element->shadow_root(); shadow_root && !shadow_root->is_user_agent_internal())
            scan_container(*shadow_root);
    }

    ParagraphBuilder builder { *this, container };
    for (auto* child = container.first_child(); child; child = child->next_sibling()) {
        auto role = role_of(*child);
        switch (role) {
        case NodeRole::Text:
        case NodeRole::Inline:
        case NodeRole::Kept:
        case NodeRole::Anchored:
            builder.add_child(*child, role);
            break;
        case NodeRole::Container:
            builder.finish();
            scan_container(*child);
            break;
        case NodeRole::Break:
            builder.finish();
            scan_anchored(*child);
            break;
        case NodeRole::Excluded:
            builder.finish();
            break;
        }
    }
    builder.finish();
}

void DocumentTranslator::scan_anchored(DOM::Node& node)
{
    auto* element = as_if<DOM::Element>(node);
    if (!element || is_excluded(*element))
        return;
    scan_attributes(*element);
    if (is_html_element_named(*element, HTML::TagNames::select))
        scan_options(*element);
}

void DocumentTranslator::scan_attributes(DOM::Element& element)
{
    if (is_excluded(element) && !element.is_editable_or_editing_host())
        return;
    for (auto const* name : translated_attributes()) {
        if (!is_attribute_translated(element, *name))
            continue;
        auto value = element.get_attribute(*name);
        if (!value.has_value() || !has_translatable_text(*value))
            continue;
        if (auto written = m_written_attributes.get(element); written.has_value()) {
            if (auto written_value = written->get(*name); written_value.has_value() && *written_value == *value)
                continue;
        }
        auto& unit = this->unit(create_unit(UnitKind::Attribute));
        unit.element = element;
        unit.attribute = *name;
        unit.original_text = *value;
        unit.runs.append({ *value, 1, false });
    }
}

void DocumentTranslator::scan_options(DOM::Element& select)
{
    select.for_each_in_subtree_of_type<HTML::HTMLOptionElement>([&](HTML::HTMLOptionElement& option) {
        if (is_excluded(option))
            return TraversalDecision::Continue;
        auto text = option.text();
        if (!has_translatable_text(text) || m_written_text.get(option) == text)
            return TraversalDecision::Continue;
        auto& unit = this->unit(create_unit(UnitKind::OptionText));
        unit.element = option;
        unit.original_text = text;
        unit.runs.append({ move(text), 1, false });
        return TraversalDecision::Continue;
    });
}

void DocumentTranslator::scan_title()
{
    auto title = m_document->title_element();
    if (!title || is_excluded(*title))
        return;
    auto text = title->text_content().value_or({});
    if (!has_translatable_text(text) || m_written_text.get(*title) == text)
        return;
    auto& unit = this->unit(create_unit(UnitKind::Title));
    unit.element = title;
    unit.original_text = text;
    unit.runs.append({ move(text), 1, false });
}

u64 DocumentTranslator::create_unit(UnitKind kind)
{
    auto id = m_document->page().allocate_translation_unit_id();
    m_units.ensure(id).kind = kind;
    return id;
}

bool DocumentTranslator::is_paragraph_in_flight(Vector<GC::Ref<DOM::Node>> const& range) const
{
    for (auto const& [id, unit] : m_units) {
        if (unit.in_flight && unit.kind == UnitKind::Paragraph && unit.range == range)
            return true;
    }
    return false;
}

Optional<DocumentTranslator::Candidate> DocumentTranslator::next_candidate()
{
    // Units whose text has left the document are dropped.
    m_units.remove_all_matching([](u64, Unit const& unit) {
        if (unit.in_flight)
            return false;
        auto node = unit.kind == UnitKind::Paragraph ? GC::Ptr<DOM::Node> { unit.container } : GC::Ptr<DOM::Node> { unit.element };
        return !node || !node->is_connected();
    });

    // OPTIMIZATION: A unit's priority comes from where it's laid out, which takes a layout to tell. So it's worked out
    //               once, and again only when the viewport moves, rather than for every unit each time one is sent.
    auto navigable = m_document->navigable();
    auto viewport_rect = navigable ? navigable->viewport_rect() : CSSPixelRect {};
    auto viewport_moved = m_prioritized_viewport_rect != viewport_rect;
    m_prioritized_viewport_rect = viewport_rect;

    Optional<Candidate> best;
    for (auto& [id, unit] : m_units) {
        if (unit.in_flight)
            continue;
        if (viewport_moved || !unit.priority.has_value())
            unit.priority = priority_of(unit);
        // NB: Ties go to the earlier unit, which comes first in the document when both were found in the same scan.
        Candidate candidate { id, *unit.priority };
        if (!best.has_value() || candidate.priority < best->priority || (candidate.priority == best->priority && candidate.unit_id < best->unit_id))
            best = candidate;
    }
    return best;
}

u64 DocumentTranslator::priority_of(Unit const& unit) const
{
    auto priority = [](PriorityClass priority_class, u64 distance) {
        return (to_underlying(priority_class) << 32) | min(distance, static_cast<u64>(0xffffffff));
    };
    if (unit.kind == UnitKind::Title)
        return priority(PriorityClass::Title, 0);

    GC::Ptr<DOM::Element> element = unit.element;
    if (unit.kind == UnitKind::Paragraph) {
        if (auto* first_element = as_if<DOM::Element>(*unit.range.first()))
            element = first_element;
        else if (auto* shadow_root = as_if<DOM::ShadowRoot>(*unit.container))
            element = shadow_root->host();
        else
            element = as_if<DOM::Element>(*unit.container);
    }
    auto navigable = m_document->navigable();
    if (!element || !navigable)
        return priority(PriorityClass::NotRendered, 0);

    auto rect = element->get_bounding_client_rect();
    if (rect.is_empty())
        return priority(PriorityClass::NotRendered, 0);
    auto viewport_size = navigable->viewport_rect().size();
    CSSPixelRect viewport { {}, viewport_size };
    if (rect.intersects(viewport))
        return priority(PriorityClass::InViewport, 0);
    // Text just below or above the viewport is likelier to be read next than text far from it.
    auto distance = rect.bottom() < 0 ? -rect.bottom() : rect.top() - viewport_size.height();
    return priority(PriorityClass::Rendered, max(distance, CSSPixels(0)).to_int());
}

Web::TranslationUnit DocumentTranslator::take_unit_for_translation(u64 unit_id)
{
    auto& unit = m_units.find(unit_id)->value;
    unit.in_flight = true;
    return { unit_id, unit.runs };
}

void DocumentTranslator::did_translate(Web::TranslatedUnit const& translated_unit)
{
    auto it = m_units.find(translated_unit.id);
    if (it == m_units.end())
        return;
    // NB: The unit stays where visit_edges() reaches it until it's applied, since applying it detaches the nodes it holds
    //     before putting them back, and a collection in between would otherwise free them.
    ScopeGuard forget_unit = [&] { m_units.remove(translated_unit.id); };
    if (!translated_unit.runs.has_value())
        return;
    TemporaryChange applying { m_is_applying, true };
    apply(it->value, *translated_unit.runs);
}

bool DocumentTranslator::is_in_excluded_element(DOM::Node const& node) const
{
    for (auto const* ancestor = &node; ancestor; ancestor = ancestor->parent()) {
        if (auto const* element = as_if<DOM::Element>(*ancestor); element && is_excluded(*element))
            return true;
    }
    return false;
}

void DocumentTranslator::apply(Unit& unit, Vector<Web::TranslationRun> const& runs)
{
    // Text the page made ineligible while its translation was on its way, e.g. by marking it not to be translated, is
    // left as it is.
    if (unit.kind == UnitKind::Paragraph) {
        if (!unit.container || is_in_excluded_element(*unit.container) || any_of(unit.inline_elements, [&](auto const& element) { return is_excluded(*element); }))
            return;
        apply_paragraph(unit, runs);
        return;
    }

    auto element = unit.element;
    if (!element || !element->is_connected() || is_in_excluded_element(*element))
        return;
    if (unit.kind == UnitKind::Attribute && !is_attribute_translated(*element, *unit.attribute))
        return;
    auto translated_text = joined_text(runs);

    switch (unit.kind) {
    case UnitKind::Attribute: {
        // An attribute the page changed since it was sent is left with the page's value.
        if (element->get_attribute(*unit.attribute) != unit.original_text)
            return;
        element->set_attribute_value(*unit.attribute, translated_text);
        m_written_attributes.ensure(*element).set(*unit.attribute, move(translated_text));
        break;
    }
    case UnitKind::Title: {
        if (element->text_content() != unit.original_text)
            return;
        element->string_replace_all(translated_text);
        m_written_text.set(*element, move(translated_text));
        break;
    }
    case UnitKind::OptionText: {
        auto& option = as<HTML::HTMLOptionElement>(*element);
        if (option.text() != unit.original_text)
            return;
        // An option without a value submits its text, so the text it had is kept as its value.
        if (!option.has_attribute(HTML::AttributeNames::value))
            option.set_attribute_value(HTML::AttributeNames::value, unit.original_text);
        option.set_text(translated_text);
        m_written_text.set(option, move(translated_text));
        break;
    }
    case UnitKind::Paragraph:
        VERIFY_NOT_REACHED();
    }
}

bool DocumentTranslator::paragraph_is_unchanged(Unit const& unit) const
{
    auto& container = *unit.container;
    for (size_t index = 0; index < unit.range.size(); ++index) {
        auto& node = *unit.range[index];
        if (node.parent() != &container)
            return false;
        if (index + 1 < unit.range.size() && node.next_sibling() != unit.range[index + 1].ptr())
            return false;
    }

    // The nodes are the same, in the same order, and the pieces' text is what was sent.
    size_t next_node = 0;
    bool unchanged = true;
    Function<void(DOM::Node const&)> visit = [&](DOM::Node const& node) {
        if (!unchanged)
            return;
        if (next_node == unit.nodes.size() || unit.nodes[next_node].ptr() != &node) {
            unchanged = false;
            return;
        }
        ++next_node;
        if (any_of(unit.inline_elements, [&](auto const& element) { return element.ptr() == &node; })) {
            for (auto const* child = node.first_child(); child; child = child->next_sibling())
                visit(*child);
        }
    };
    for (auto const& node : unit.range)
        visit(*node);
    if (!unchanged || next_node != unit.nodes.size())
        return false;

    return all_of(unit.pieces, [](Piece const& piece) {
        auto const* text = as_if<DOM::Text>(*piece.node);
        return !text || text->data() == piece.original_text;
    });
}

// Rebuilds a paragraph from its translation. Each run's text goes into the inline elements its token's text was in:
// The first time an element is needed, it's the element itself, so it keeps its identity and its event listeners; an
// element the translation needs a second time, apart from the first, is a shallow clone of it. The nodes that aren't
// translated, e.g. images, are put back among the translated text next to the text they were next to.
void DocumentTranslator::apply_paragraph(Unit& unit, Vector<Web::TranslationRun> const& runs)
{
    // A paragraph the page changed is scanned again, so this translation is dropped.
    if (!paragraph_is_unchanged(unit)) {
        m_dirty_nodes.append(*unit.container);
        schedule_rescan();
        return;
    }
    if (!any_of(runs, [](auto const& run) { return !run.keep_as_is && !run.text.is_empty(); }))
        return;

    auto& container = *unit.container;
    auto& document = *m_document;

    // The inline elements a piece's text is in, from the outermost to its owner, as indices among the inline elements.
    auto chain_of = [&](Optional<size_t> owner_index) {
        Vector<size_t> chain;
        for (auto index = owner_index; index.has_value(); index = unit.inline_element_parents[*index])
            chain.prepend(*index);
        return chain;
    };
    auto piece_for = [&](u32 token) -> Piece* {
        if (token == 0 || token > unit.pieces.size())
            return nullptr;
        return &unit.pieces[token - 1];
    };

    // The elements the translation puts text into. An element it puts none into stays as it was, text and all.
    Vector<bool> is_used;
    is_used.resize(unit.inline_elements.size());
    for (auto const& run : runs) {
        if (auto* piece = piece_for(run.token)) {
            for (auto index : chain_of(piece->owner_index))
                is_used[index] = true;
        }
    }
    auto is_used_element = [&](Optional<size_t> index) { return !index.has_value() || is_used[*index]; };

    // Take the paragraph apart.
    GC::Ptr<DOM::Node> range_end = unit.range.last()->next_sibling();
    for (auto const& node : unit.range)
        node->remove();
    for (size_t index = 0; index < unit.inline_elements.size(); ++index) {
        if (!is_used[index])
            continue;
        while (auto* child = unit.inline_elements[index]->first_child())
            child->remove();
    }

    // Put the translated text together.
    struct OpenElement {
        size_t index { 0 };
        GC::Ref<DOM::Node> output;
    };
    GC::ConservativeVector<OpenElement> open_elements;
    Vector<bool> is_opened;
    is_opened.resize(unit.inline_elements.size());
    Vector<bool> is_placed;
    is_placed.resize(unit.pieces.size());
    GC::RootHashMap<u32, GC::Ref<DOM::Node>> first_placed;
    GC::RootHashMap<u32, GC::Ref<DOM::Node>> last_placed;

    auto insert = [&](DOM::Node& node) {
        if (open_elements.is_empty())
            container.insert_before(node, range_end);
        else
            open_elements.last().output->insert_before(node, nullptr);
    };

    for (auto const& run : runs) {
        auto* piece = piece_for(run.token);
        // Text the translator leaves as it is goes back once, as it was.
        if (piece && piece->keep_as_is && is_placed[run.token - 1])
            continue;
        auto chain = chain_of(piece ? piece->owner_index : Optional<size_t> {});

        size_t common_depth = 0;
        while (common_depth < open_elements.size() && common_depth < chain.size() && open_elements[common_depth].index == chain[common_depth])
            ++common_depth;
        open_elements.shrink(common_depth);
        for (size_t depth = common_depth; depth < chain.size(); ++depth) {
            auto index = chain[depth];
            auto& original = *unit.inline_elements[index];
            GC::Ptr<DOM::Node> output;
            if (!is_opened[index]) {
                is_opened[index] = true;
                output = original;
            } else if (auto clone = original.clone_node(nullptr, false); !clone.is_error()) {
                output = clone.release_value();
            } else {
                break;
            }
            insert(*output);
            open_elements.append({ index, *output });
        }

        GC::Ptr<DOM::Node> placed;
        if (piece && (piece->keep_as_is || (is<DOM::Text>(*piece->node) && !is_placed[run.token - 1]))) {
            placed = piece->node;
            if (!piece->keep_as_is)
                as<DOM::Text>(*placed).set_data(run.text);
            is_placed[run.token - 1] = true;
        } else {
            placed = document.create_text_node(run.text);
        }
        insert(*placed);
        if (auto* text = as_if<DOM::Text>(*placed); text && !(piece && piece->keep_as_is))
            m_translated_text.set(*text);
        first_placed.ensure(run.token, [&] { return GC::Ref { *placed }; });
        last_placed.set(run.token, *placed);
    }

    // Put back the nodes that aren't translated: the anchored ones, the kept text the translation left out, and the
    // inline elements it put no text into.
    GC::ConservativeVector<AnchoredNode> nodes_to_place;
    for (auto const& anchored_node : unit.anchored_nodes)
        nodes_to_place.append(anchored_node);
    for (size_t index = 0; index < unit.pieces.size(); ++index) {
        auto const& piece = unit.pieces[index];
        if (!piece.keep_as_is || is_placed[index])
            continue;
        auto token = static_cast<u32>(index + 1);
        nodes_to_place.append({ piece.node, piece.owner, piece.owner_index, token > 1 ? token - 1 : 0, token < unit.pieces.size() ? token + 1 : 0 });
    }
    for (size_t element_index = 0; element_index < unit.inline_elements.size(); ++element_index) {
        if (is_used[element_index])
            continue;
        auto& element = *unit.inline_elements[element_index];
        auto parent_index = unit.inline_element_parents[element_index];
        GC::Ptr<DOM::Element> parent = parent_index.has_value() ? unit.inline_elements[*parent_index].ptr() : nullptr;
        u32 first_inside = 0;
        u32 last_inside = 0;
        for (size_t index = 0; index < unit.pieces.size(); ++index) {
            if (element.is_ancestor_of(*unit.pieces[index].node)) {
                if (first_inside == 0)
                    first_inside = index + 1;
                last_inside = index + 1;
            }
        }
        nodes_to_place.append({ element, parent, parent_index, first_inside > 1 ? first_inside - 1 : 0, last_inside < unit.pieces.size() ? last_inside + 1 : 0 });
    }

    GC::RootHashMap<GC::Ptr<DOM::Node>, GC::Ref<DOM::Node>> last_inserted_after;
    auto is_space = [](Utf16View const& text, size_t index) { return is_ascii_space(text.code_unit_at(index)); };
    auto restore_spaces = [&](AnchoredNode const& placed) {
        auto& node = *placed.node;
        auto* parent = node.parent();
        if (!parent)
            return;
        if (placed.space_after) {
            if (auto* next = as_if<DOM::Text>(node.next_sibling())) {
                if (!next->data().is_empty() && !is_space(next->data(), 0))
                    next->set_data(Utf16String::formatted(" {}", next->data()));
            } else if (node.next_sibling()) {
                auto space = document.create_text_node(" "_utf16);
                parent->insert_before(space, node.next_sibling());
                m_translated_text.set(space);
            }
        }
        if (placed.space_before) {
            if (auto* previous = as_if<DOM::Text>(node.previous_sibling())) {
                auto const& data = previous->data();
                if (!data.is_empty() && !is_space(data, data.length_in_code_units() - 1))
                    previous->set_data(Utf16String::formatted("{} ", data));
            } else if (node.previous_sibling()) {
                auto space = document.create_text_node(" "_utf16);
                parent->insert_before(space, node);
                m_translated_text.set(space);
            }
        }
    };

    for (auto const& node_to_place : nodes_to_place) {
        ScopeGuard restore_spaces_around_node = [&] { restore_spaces(node_to_place); };
        auto owner = node_to_place.owner;
        // A node in an element the translation didn't use is still in that element.
        if (!is_used_element(node_to_place.owner_index))
            continue;
        if (owner && !owner->is_inclusive_ancestor_of(*node_to_place.node) && node_to_place.node->parent())
            continue;
        DOM::Node& parent = owner ? static_cast<DOM::Node&>(*owner) : container;

        if (auto anchor = last_placed.get(node_to_place.preceding_token); node_to_place.preceding_token && anchor.has_value()) {
            if (auto child = child_containing(parent, **anchor)) {
                auto after = last_inserted_after.get(child).value_or(*child);
                parent.insert_before(*node_to_place.node, after->next_sibling());
                last_inserted_after.set(child, *node_to_place.node);
                continue;
            }
        }
        if (auto anchor = first_placed.get(node_to_place.following_token); node_to_place.following_token && anchor.has_value()) {
            if (auto child = child_containing(parent, **anchor)) {
                parent.insert_before(*node_to_place.node, child);
                continue;
            }
        }
        if (owner)
            owner->insert_before(*node_to_place.node, nullptr);
        else
            container.insert_before(*node_to_place.node, range_end);
    }
}

void DocumentTranslator::did_mutate(DOM::Node& target, Utf16FlyString const& type, Optional<Utf16FlyString> const& attribute_name, Optional<Utf16String> const& old_value)
{
    if (m_is_applying || !is_started())
        return;

    if (type == DOM::MutationType::attributes) {
        auto matches = [&](auto const* name) { return attribute_name == *name; };
        if (!any_of(translated_attributes(), matches) && !any_of(attributes_that_change_translation(), matches))
            return;
        auto* element = as_if<DOM::Element>(target);
        if (!element)
            return;
        // OPTIMIZATION: Pages change the classes and styles of the root and the body all the time, e.g. as they scroll,
        //               and scanning the whole page again for each change would be costly. So a class change counts only
        //               if it marks text as not to be translated, or stops marking it, and a change of the root's or the
        //               body's style counts not at all.
        if (attribute_name == HTML::AttributeNames::class_) {
            auto had_notranslate = old_value.has_value() && any_of(old_value->utf16_view().split_view(' ', SplitBehavior::Nothing), [](auto const& name) { return name == u"notranslate"sv; });
            if (had_notranslate == element->has_class(u"notranslate"sv))
                return;
        }
        if ((attribute_name == HTML::AttributeNames::style || attribute_name == HTML::AttributeNames::hidden)
            && (element == m_document->document_element() || element == m_document->body()))
            return;
    } else if (type == DOM::MutationType::characterData) {
        // Text the page changed is the page's own again, and is translated again.
        if (auto* text = as_if<DOM::Text>(target))
            m_translated_text.remove(*text);
    }
    // NB: A change that only removes nodes leaves nothing new to translate. But it drops the translation still on its way
    //     for the paragraph it changed, so the paragraph is scanned again, to translate what's left of it.

    m_dirty_nodes.append(target);
    schedule_rescan();
}

void DocumentTranslator::schedule_rescan()
{
    if (m_rescan_scheduled)
        return;
    m_rescan_scheduled = true;
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(m_document), GC::create_function(m_document->heap(), [document = GC::Weak<DOM::Document> { m_document }] {
        auto strong_document = document.ptr();
        if (!strong_document)
            return;
        if (auto* translator = strong_document->document_translator_if_exists()) {
            translator->rescan();
            strong_document->page().notify_translation_progress();
        }
    }));
}

void DocumentTranslator::rescan()
{
    m_rescan_scheduled = false;
    auto dirty_nodes = move(m_dirty_nodes);

    // Each changed node's container is scanned again, once, however many of its nodes changed.
    GC::RootVector<GC::Ref<DOM::Node>> candidates;
    GC::RootHashTable<GC::Ptr<DOM::Node>> candidate_set;
    bool title_changed = false;
    for (auto const& weak_node : dirty_nodes) {
        auto node = weak_node.ptr();
        if (!node || !node->is_connected() || &node->document() != m_document.ptr())
            continue;
        if (auto title = m_document->title_element(); title && title->is_inclusive_ancestor_of(*node)) {
            title_changed = true;
            continue;
        }

        // A node in an excluded element isn't translated, so nothing is scanned for it.
        if (is_in_excluded_element(*node))
            continue;

        GC::Ptr<DOM::Node> container = node;
        while (container) {
            if (is<DOM::ShadowRoot>(*container) || container.ptr() == m_document->document_element())
                break;
            if (is<DOM::Element>(*container) && role_of(*container) == NodeRole::Container)
                break;
            container = container->parent();
        }
        if (!container)
            continue;
        if (candidate_set.set(container) == HashSetResult::InsertedNewEntry)
            candidates.append(*container);
    }

    // A container inside another that's scanned again is scanned with it.
    GC::RootVector<GC::Ref<DOM::Node>> containers;
    for (auto const& candidate : candidates) {
        bool is_inside_another = false;
        for (auto ancestor = candidate->parent(); ancestor && !is_inside_another; ancestor = ancestor->parent())
            is_inside_another = candidate_set.contains(ancestor);
        if (!is_inside_another)
            containers.append(candidate);
    }

    if (title_changed)
        scan_title();
    for (auto const& container : containers) {
        forget_units_within(*container);
        scan_container(*container);
    }
}

void DocumentTranslator::forget_units_within(DOM::Node& container)
{
    // A unit in flight is checked when its translation comes back, so it's kept.
    m_units.remove_all_matching([&](u64, Unit const& unit) {
        if (unit.in_flight)
            return false;
        auto node = unit.kind == UnitKind::Paragraph ? GC::Ptr<DOM::Node> { unit.container } : GC::Ptr<DOM::Node> { unit.element };
        return node && container.is_inclusive_ancestor_of(*node);
    });
}

bool DocumentTranslator::is_settled() const
{
    return m_units.is_empty() && !m_rescan_scheduled;
}

}
