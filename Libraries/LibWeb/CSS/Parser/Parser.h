/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/CSS/Descriptor.h>
#include <LibWeb/CSS/DescriptorID.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/Parser/RuleContext.h>
#include <LibWeb/CSS/Parser/RustSyntaxHandle.h>
#include <LibWeb/CSS/Parser/RustSyntaxParsing.h>
#include <LibWeb/CSS/Parser/SubstitutionFunctionsPresence.h>
#include <LibWeb/CSS/RustMediaList.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/Supports.h>
#include <LibWeb/CSS/URL.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

enum SpecialContext : u8 {
    CanvasContextGenericValue,
    DOMMatrixInitString,
    MediaCondition,
    OnScreenCanvasContextFontValue
};
// FIXME: Use PropertyNameAndID instead of PropertyID as the context, for registered custom properties.
using ValueParsingContext = Variant<PropertyID, SpecialContext>;

enum class ParsingMode {
    Normal,
    SVGPresentationAttribute, // See https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
};

enum class IsUAStyleSheet {
    Yes,
    No,
};

struct WEB_API ParsingParams {
    explicit ParsingParams(ParsingMode = ParsingMode::Normal);
    explicit ParsingParams(ValueParsingContext);
    explicit ParsingParams(IsUAStyleSheet);
    explicit ParsingParams(DOM::Document const&, ParsingMode = ParsingMode::Normal);

    GC::Ptr<DOM::Document const> document;
    ParsingMode mode { ParsingMode::Normal };
    IsUAStyleSheet is_ua_style_sheet { IsUAStyleSheet::No };

    Vector<ValueParsingContext> value_context;
    Vector<RuleContext> rule_context;
    RustNamespaceContext declared_namespaces;
};

struct DevToolsStyleDeclaration {
    Utf16FlyString name;
    Utf16String value;
    Important important { Important::No };
    bool is_custom_property { false };
    bool is_name_valid { false };
    bool is_valid { false };
};

WEB_API Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const&, StringView);
WEB_API Vector<DevToolsStyleDeclaration> parse_css_declaration_block_for_devtools(ParsingParams const&, Utf16View);

// Main-thread context extraction and result wrapping for the Rust CSS parser.
class Parser {
    AK_MAKE_NONCOPYABLE(Parser);
    AK_MAKE_NONMOVABLE(Parser);

public:
    explicit Parser(ParsingParams);
    static void parse_stylesheet_off_thread(ParsingParams const&, Utf16String, Function<void(RustStyleSheetParse)>);

    NonnullRefPtr<CSS::StyleSheetState> parse_as_css_stylesheet(Utf16View, Optional<::URL::URL> location, RustMediaList = {});
    NonnullRefPtr<StyleSheetState> create_css_stylesheet(RustStyleSheetParse const&, Optional<::URL::URL> location, RustMediaList = {});

    RustDeclarationBlock parse_as_property_declaration_block(Utf16View);
    Vector<DevToolsStyleDeclaration> parse_as_devtools_property_declaration_block(Utf16View);
    RustDescriptorBlock parse_as_descriptor_declaration_block(Utf16View, AtRuleID);
    Optional<RustRule> parse_as_css_rule(Utf16View, bool nested = false);
    Optional<RustRule> parse_as_keyframe_rule(Utf16View);
    RustRuleList parse_as_stylesheet_contents(Utf16View);

    enum class SelectorParsingMode {
        Standard,
        // `<forgiving-selector-list>` and `<forgiving-relative-selector-list>`
        // are handled with this parameter, not as separate functions.
        // https://drafts.csswg.org/selectors/#forgiving-selector
        Forgiving
    };
    // Contrary to the name, these parse a comma-separated list of selectors, according to the spec.
    Optional<SelectorList> parse_as_selector(Utf16View, SelectorParsingMode = SelectorParsingMode::Standard);

    Optional<Selector::PseudoElementSelector> parse_as_pseudo_element_selector(Utf16View);

    Optional<RustQueryHandle> parse_as_supports(Utf16View);

    RefPtr<StyleValue const> parse_as_css_value(Utf16View, PropertyID);
    RefPtr<StyleValue const> parse_as_descriptor_value(Utf16View, AtRuleID, DescriptorNameAndID const&);
    RefPtr<StyleValue const> parse_primitive_value_from_source(ValueType, Utf16View, NumericRange const& = infinite_range);

    [[nodiscard]] NonnullRefPtr<StyleValue const> parse_as_sizes_attribute(Utf16View, DOM::Element const& element, HTML::HTMLImageElement const* img = nullptr);

    NonnullRefPtr<StyleValue const> parse_with_a_syntax(Utf16View input, RustSyntaxHandle const& syntax);

    enum class ParseError : u8 {
        SyntaxError,
    };
    template<typename T>
    using ParseErrorOr = ErrorOr<T, ParseError>;

    static ParseErrorOr<void> collect_arbitrary_substitution_function_presence(Utf16View, SubstitutionFunctionsPresence&);

private:
    RustStyleSheetParse parse_stylesheet(Utf16View);
    enum class ParseContextMode {
        Syntax,
        Value,
        RegisteredSyntax,
    };
    // Self-referential: `context` points into this object's storage.
    struct ParseContextStorage {
        AK_MAKE_NONCOPYABLE(ParseContextStorage);
        AK_MAKE_NONMOVABLE(ParseContextStorage);

    public:
        ParseContextStorage(Parser&, ParseContextMode, Optional<PropertyID>);

        Vector<ValueParserFFI::FfiValueParsingContext, 2> value_contexts;
        Optional<ComputedValuesFFI::FfiLengthResolutionContext> length_resolution_context;
        ValueParserFFI::ParseContext context {};
    };

    ParseErrorOr<NonnullRefPtr<StyleValue const>> parse_css_value_from_source(PropertyID, Utf16View);
    ParseContextStorage make_parse_context(ParseContextMode, Optional<PropertyID> direct_property_context = {});

    bool in_quirks_mode() const;
    bool is_parsing_svg_presentation_attribute() const;

    GC::Ptr<DOM::Document const> m_document;
    Optional<String> m_serialized_document_url;
    Optional<String> m_serialized_document_base_url;
    ParsingMode m_parsing_mode { ParsingMode::Normal };
    IsUAStyleSheet m_is_ua_style_sheet { IsUAStyleSheet::No };

    Vector<ValueParsingContext> m_value_context;
    size_t m_random_function_index = 0;
    Vector<RuleContext> m_rule_context;
    RustNamespaceContext m_declared_namespaces;
};

Optional<RustRule> parse_keyframe_rule(ParsingParams const&, Utf16View);
Optional<RustQueryHandle> parse_style_query(Utf16View);

}

namespace Web {

NonnullRefPtr<CSS::StyleSheetState> parse_css_stylesheet(CSS::Parser::ParsingParams const&, StringView, Optional<::URL::URL> location = {}, CSS::RustMediaList media_list = {});
NonnullRefPtr<CSS::StyleSheetState> parse_css_stylesheet(CSS::Parser::ParsingParams const&, Utf16View, Optional<::URL::URL> location = {}, CSS::RustMediaList media_list = {});
CSS::RustDeclarationBlock parse_css_property_declaration_block(CSS::Parser::ParsingParams const&, Utf16View);
CSS::RustDescriptorBlock parse_css_descriptor_declaration_block(CSS::Parser::ParsingParams const&, CSS::AtRuleID, Utf16View);
RefPtr<CSS::StyleValue const> parse_css_value(CSS::Parser::ParsingParams const&, StringView, CSS::PropertyID);
RefPtr<CSS::StyleValue const> parse_css_value(CSS::Parser::ParsingParams const&, Utf16View, CSS::PropertyID);
RefPtr<CSS::StyleValue const> parse_css_type(CSS::Parser::ParsingParams const&, Utf16View, CSS::ValueType);
RefPtr<CSS::StyleValue const> parse_css_descriptor(CSS::Parser::ParsingParams const&, CSS::AtRuleID, CSS::DescriptorNameAndID const&, Utf16View);
Optional<CSS::SelectorList> parse_selector(CSS::Parser::ParsingParams const&, Utf16View);
Optional<CSS::Selector::PseudoElementSelector> parse_pseudo_element_selector(CSS::Parser::ParsingParams const&, Utf16View);
Optional<CSS::RustRule> parse_css_rule(CSS::Parser::ParsingParams const&, Utf16View, bool nested = false);
RefPtr<CSS::MediaQuery> parse_media_query(Utf16View);
Vector<NonnullRefPtr<CSS::MediaQuery>> parse_media_query_list(Utf16View);
Optional<CSS::RustQueryHandle> parse_css_supports(CSS::Parser::ParsingParams const&, Utf16View);
WEB_API ErrorOr<Utf16String> css_decode_bytes(Optional<StringView> const& environment_encoding, Optional<StringView> mime_type_charset, ReadonlyBytes encoded_string);
bool is_valid_animation_name_custom_ident(Utf16View);

}
