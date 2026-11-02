/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <AK/Bitmap.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/Find.h>
#include <AK/FixedBitmap.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf8View.h>
#include <LibGC/Heap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/Animations/ViewTimeline.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSScopeRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceState.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineEffectTiming.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Page/RenderingStats.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>
#include <math.h>

namespace Web::CSS {

// What a rule contributes, for the two rule types that carry a declaration block.
static RustDeclarationBlock const& declaration_of_rule(CSSRule const& rule)
{
    if (rule.type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(rule).declaration();
    if (rule.type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(rule).declaration();
    VERIFY_NOT_REACHED();
}

static bool custom_property_inherits(DOM::Document const& document, Utf16FlyString const& name)
{
    // A custom property inherits unless it has been registered with an explicit `inherits: false`.
    auto registration = document.get_registered_custom_property(name);
    return !registration.has_value() || registration->inherit;
}

enum class IsCustomProperty : u8 {
    No,
    Yes,
};

enum class Inherits : u8 {
    No,
    Yes,
};

enum class NameIsValid : u8 {
    No,
    Yes,
};

enum class IsValid : u8 {
    No,
    Yes,
};

static JsonObject serialize_devtools_style_declaration(
    String name,
    String value,
    Important important,
    IsCustomProperty is_custom_property,
    Inherits inherits,
    NameIsValid is_name_valid,
    IsValid is_valid)
{
    JsonObject serialized_property;
    serialized_property.set("name"sv, move(name));
    serialized_property.set("value"sv, move(value));
    serialized_property.set("priority"sv, important == Important::Yes ? "important"sv : ""sv);
    serialized_property.set("isCustomProperty"sv, is_custom_property == IsCustomProperty::Yes);
    serialized_property.set("inherits"sv, inherits == Inherits::Yes);
    serialized_property.set("isNameValid"sv, is_name_valid == NameIsValid::Yes);
    serialized_property.set("isValid"sv, is_valid == IsValid::Yes);
    return serialized_property;
}

static JsonArray serialize_devtools_style_declarations(DOM::Document const& document, RustDeclarationBlock const& declaration)
{
    JsonArray declarations;

    auto serialize_property = [&](Utf16FlyString const& name, StyleProperty const& property, IsCustomProperty is_custom_property, Inherits inherits) {
        declarations.must_append(serialize_devtools_style_declaration(
            name.to_utf16_string().to_utf8_but_should_be_ported_to_utf16(),
            property.value->to_string(SerializationMode::Normal),
            property.important,
            is_custom_property,
            inherits,
            NameIsValid::Yes,
            IsValid::Yes));
    };

    for (auto const& property : declaration.properties()) {
        serialize_property(
            string_from_property_id(property.property_id),
            property,
            IsCustomProperty::No,
            is_inherited_property(property.property_id) ? Inherits::Yes : Inherits::No);
    }

    for (auto const& custom_property : declaration.custom_properties())
        serialize_property(
            custom_property.key,
            custom_property.value,
            IsCustomProperty::Yes,
            custom_property_inherits(document, custom_property.key) ? Inherits::Yes : Inherits::No);

    return declarations;
}

static JsonArray serialize_devtools_style_declarations(DOM::Document const& document, Vector<Parser::DevToolsStyleDeclaration> const& declarations)
{
    JsonArray serialized_declarations;

    for (auto const& declaration : declarations) {
        bool inherits = declaration.is_custom_property
            ? custom_property_inherits(document, declaration.name)
            : PropertyNameAndID::from_name(declaration.name)
                  .map([](auto const& property) { return !property.is_custom_property() && is_inherited_property(property.id()); })
                  .value_or(false);

        serialized_declarations.must_append(serialize_devtools_style_declaration(
            MUST(declaration.name.view().to_utf8()),
            declaration.value.to_utf8(),
            declaration.important,
            declaration.is_custom_property ? IsCustomProperty::Yes : IsCustomProperty::No,
            inherits ? Inherits::Yes : Inherits::No,
            declaration.is_name_valid ? NameIsValid::Yes : NameIsValid::No,
            declaration.is_valid ? IsValid::Yes : IsValid::No));
    }

    return serialized_declarations;
}

static Vector<Parser::DevToolsStyleDeclaration> parse_devtools_style_declarations(DOM::Document const& document, StringView declaration_block)
{
    return Parser::parse_css_declaration_block_for_devtools(Parser::ParsingParams(document), declaration_block);
}

static Vector<Parser::DevToolsStyleDeclaration> parse_devtools_style_declarations(DOM::Document const& document, Utf16View declaration_block)
{
    return Parser::parse_css_declaration_block_for_devtools(Parser::ParsingParams(document), declaration_block);
}

static Optional<size_t> source_offset_for_line_and_column(StringView source, SourcePosition const& position)
{
    size_t line = 0;
    size_t column = 0;

    Utf8View source_code_points { source };
    for (auto it = source_code_points.begin(); it != source_code_points.end();) {
        auto offset = source_code_points.byte_offset_of(it);
        if (line == position.line && column == position.column)
            return offset;

        auto code_point = *it;
        ++it;

        if (code_point == '\r') {
            if (offset + 1 < source.length() && source[offset + 1] == '\n')
                ++it;
            ++line;
            column = 0;
        } else if (code_point == '\n' || code_point == '\f') {
            ++line;
            column = 0;
        } else {
            ++column;
        }
    }

    if (line == position.line && column == position.column)
        return source.length();

    return {};
}

static Optional<String> extract_css_declaration_block_from_source(CSSRule const& rule)
{
    if (rule.type() != CSSRule::Type::Style)
        return {};

    auto const* style_sheet = rule.parent_style_sheet();
    if (!style_sheet)
        return {};

    auto const source_text = style_sheet->source_text();
    if (!source_text.has_value())
        return {};

    auto source = source_text->to_utf8();
    auto source_view = source.bytes_as_string_view();
    auto const& source_location = rule.source_location();
    if (!source_location.has_value())
        return {};

    auto maybe_offset = source_offset_for_line_and_column(source_view, *source_location);
    if (!maybe_offset.has_value())
        return {};

    Optional<u8> string_quote;
    bool in_comment = false;
    bool escaped = false;
    Optional<size_t> block_start;
    size_t block_depth = 0;

    for (size_t offset = *maybe_offset; offset < source_view.length(); ++offset) {
        auto ch = source_view[offset];
        auto next_ch = offset + 1 < source_view.length() ? source_view[offset + 1] : '\0';

        if (in_comment) {
            if (ch == '*' && next_ch == '/') {
                in_comment = false;
                ++offset;
            }
            continue;
        }

        if (string_quote.has_value()) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == *string_quote)
                string_quote = {};
            continue;
        }

        if (ch == '/' && next_ch == '*') {
            in_comment = true;
            ++offset;
            continue;
        }

        if (ch == '"' || ch == '\'') {
            string_quote = ch;
            continue;
        }

        if (ch == '{') {
            if (!block_start.has_value())
                block_start = offset + 1;
            ++block_depth;
            continue;
        }

        if (ch == '}' && block_start.has_value()) {
            VERIFY(block_depth > 0);
            --block_depth;
            if (block_depth == 0)
                return MUST(String::from_utf8(source_view.substring_view(*block_start, offset - *block_start)));
        }
    }

    return {};
}

static bool has_inherited_declaration(DOM::Document const& document, ReadonlySpan<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> const& custom_properties)
{
    if (any_of(properties, [](auto const& property) {
            return CSS::is_inherited_property(property.property_id);
        })) {
        return true;
    }

    return any_of(custom_properties, [&](auto const& custom_property) {
        return custom_property_inherits(document, custom_property.key);
    });
}

static JsonObject serialize_devtools_style_sheet_identifier(StyleSheetIdentifier const& identifier)
{
    JsonObject serialized_identifier;
    serialized_identifier.set("type"sv, style_sheet_identifier_type_to_string(identifier.type));
    if (identifier.dom_element_unique_id.has_value())
        serialized_identifier.set("domElementUniqueId"sv, identifier.dom_element_unique_id->value());
    if (identifier.url.has_value())
        serialized_identifier.set("url"sv, identifier.url->to_utf8());
    serialized_identifier.set("ruleCount"sv, identifier.rule_count);
    return serialized_identifier;
}

// What DevTools shows for one rule the engine says decides for this element. Which of the rule's
// selectors matched is asked with a selector query, because the engine reports the rule rather than
// the entry, and a panel can afford to match each selector again.
static JsonObject serialize_devtools_applied_rule(DOM::Document& document, CSSRule const& rule, DOM::AbstractElement const& element)
{
    auto const& declaration = declaration_of_rule(rule);
    auto authored_text = extract_css_declaration_block_from_source(rule);
    SelectorList const* selector_list = nullptr;
    if (auto const* style_rule = as_if<CSSStyleRule>(rule))
        selector_list = &style_rule->absolutized_selectors();
    else if (auto const* nested = as_if<CSSNestedDeclarations>(rule))
        selector_list = &nested->absolutized_selectors();
    SelectorList const empty_selectors;
    auto const& selectors = selector_list ? *selector_list : empty_selectors;

    JsonArray serialized_selectors;
    JsonArray specificities;
    JsonArray matched_selector_indexes;
    for (size_t index = 0; index < selectors.size(); ++index) {
        auto const& selector = selectors[index];
        serialized_selectors.must_append(selector->serialize().to_utf8());
        specificities.must_append(selector->specificity());
        SelectorList selector_query_list;
        selector_query_list.append(selector);
        auto selector_query = DOM::SelectorQuery::create(move(selector_query_list));
        if (selector_query->matches(element.element(), document))
            matched_selector_indexes.must_append(index);
    }

    JsonObject serialized_rule;
    serialized_rule.set("type"sv, to_underlying(rule.type()));
    serialized_rule.set("className"sv, rule.type() == CSSRule::Type::Style ? "CSSStyleRule"sv : "CSSNestedDeclarations"sv);
    serialized_rule.set("selectors"sv, move(serialized_selectors));
    serialized_rule.set("selectorsSpecificity"sv, move(specificities));
    serialized_rule.set("matchedSelectorIndexes"sv, move(matched_selector_indexes));
    serialized_rule.set("cssText"sv, rule.serialized().to_utf8());
    if (authored_text.has_value()) {
        serialized_rule.set("authoredText"sv, *authored_text);
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, parse_devtools_style_declarations(document, authored_text->bytes_as_string_view())));
    } else {
        auto style = rule.type() == CSSRule::Type::Style
            ? static_cast<CSSStyleRule const&>(rule).style()
            : static_cast<CSSNestedDeclarations const&>(rule).style();
        serialized_rule.set("authoredText"sv, style->serialized().to_utf8());
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, declaration));
    }
    if (auto* sheet = rule.parent_style_sheet()) {
        if (auto identifier = style_sheet_identifier_for(*sheet); identifier.has_value())
            serialized_rule.set("ruleId"sv, serialize_devtools_style_sheet_identifier(*identifier));
    }
    return serialized_rule;
}

static JsonObject serialize_devtools_inline_style(DOM::Document const& document, DOM::AbstractElement abstract_element, CSSStyleProperties const& declaration)
{
    auto authored_text = abstract_element.element().get_attribute(HTML::AttributeNames::style);

    JsonObject serialized_rule;
    serialized_rule.set("type"sv, 100);
    serialized_rule.set("className"sv, 100);
    serialized_rule.set("cssText"sv, declaration.serialized().to_utf8());
    if (authored_text.has_value()) {
        auto authored_text_utf8 = authored_text->to_utf8();
        serialized_rule.set("authoredText"sv, authored_text_utf8);
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, parse_devtools_style_declarations(document, authored_text->utf16_view())));
    } else {
        serialized_rule.set("authoredText"sv, declaration.serialized().to_utf8());
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, declaration.declaration_block()));
    }
    serialized_rule.set("isSystem"sv, false);
    serialized_rule.set("nodeId"sv, abstract_element.element().unique_id().value());
    return serialized_rule;
}

static void append_devtools_applied_style_entry(JsonArray& entries, JsonObject rule, Optional<UniqueNodeID> inherited_node_id = {})
{
    JsonObject entry;

    JsonValue matched_selector_indexes { JsonArray {} };
    if (auto value = rule.get("matchedSelectorIndexes"sv); value.has_value())
        matched_selector_indexes = *value;
    rule.remove("matchedSelectorIndexes"sv);
    auto is_system = rule.get_bool("isSystem"sv).value_or(false);

    entry.set("rule"sv, move(rule));
    entry.set("isSystem"sv, is_system);
    entry.set("matchedSelectorIndexes"sv, move(matched_selector_indexes));
    if (inherited_node_id.has_value())
        entry.set("inheritedNodeId"sv, inherited_node_id->value());
    else
        entry.set("inherited"sv, JsonValue {});

    entries.must_append(move(entry));
}

JsonArray StyleComputer::collect_devtools_applied_style_rules(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, bool include_inherited, bool include_user_agent_styles)
{
    JsonArray entries;

    auto append_rules_for_abstract_element = [&](DOM::AbstractElement current_element, Optional<UniqueNodeID> inherited_node_id) {
        if (auto inline_style = current_element.inline_style()) {
            if (!inherited_node_id.has_value() || has_inherited_declaration(m_document, inline_style->properties(), inline_style->custom_properties()))
                append_devtools_applied_style_entry(entries, serialize_devtools_inline_style(m_document, current_element, *inline_style), inherited_node_id);
        }

        auto node = current_element.element().style_node_id();
        if (node == 0)
            return;
        Vector<StyleEngine::MatchedRule> matches;
        if (!style_engine().matched_rules(read, node, matches))
            return;

        // The engine reports the rules in the order the cascade applies them, and the panel lists
        // the winning one first.
        for (auto const& match : matches.in_reverse()) {
            if (match.pseudo_element != NumericLimits<u32>::max())
                continue;
            auto target = style_engine_rule_target(read, StyleEngineRuleID { match.rule });
            if (!target.has_value() || !target->source_style_sheet)
                continue;
            if (target->cascade_origin == CascadeOrigin::UserAgent && !include_user_agent_styles)
                continue;
            if (inherited_node_id.has_value()) {
                struct Context {
                    GC::Ref<DOM::Document const> document;
                    bool has_inherited_declaration { false };
                } context { m_document };
                Parser::ValueParserFFI::rust_declaration_data_visit(target->declaration.data(), &context, [](void* raw_context, Parser::ValueParserFFI::FfiDeclaredProperty const* property) {
                    auto& context = *static_cast<Context*>(raw_context);
                    if (context.has_inherited_declaration)
                        return;
                    if (property->name.length == 0)
                        context.has_inherited_declaration = is_inherited_property(static_cast<PropertyID>(property->property_id));
                    else
                        context.has_inherited_declaration = custom_property_inherits(context.document, Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(property->name.utf16), property->name.length }));
                });
                if (!context.has_inherited_declaration)
                    continue;
            }
            auto* rule = target->source_style_sheet->rules().rule_for_identity(target->rule_identity);
            if (!rule)
                continue;
            append_devtools_applied_style_entry(entries, serialize_devtools_applied_rule(m_document, *rule, current_element), inherited_node_id);
        }
    };

    append_rules_for_abstract_element(abstract_element, {});

    if (!include_inherited)
        return entries;

    for (auto current_element = abstract_element.element_to_inherit_style_from(); current_element.has_value(); current_element = current_element->element_to_inherit_style_from())
        append_rules_for_abstract_element(*current_element, current_element->element().unique_id());

    return entries;
}

}
