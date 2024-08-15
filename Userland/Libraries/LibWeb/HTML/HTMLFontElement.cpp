/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <LibWeb/Bindings/HTMLFontElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/ParsingContext.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/HTML/HTMLFontElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLFontElement);

enum class Mode {
    RelativePlus,
    RelativeMinus,
    Absolute,
};

// https://html.spec.whatwg.org/multipage/rendering.html#rules-for-parsing-a-legacy-font-size
static Optional<CSS::Keyword> parse_legacy_font_size(StringView string)
{
    // 1. Let input be the attribute's value.
    // 2. Let position be a pointer into input, initially pointing at the start of the string.
    GenericLexer lexer { string };

    // 3. Skip ASCII whitespace within input given position.
    lexer.ignore_while(Web::Infra::is_ascii_whitespace);

    // 4. If position is past the end of input, there is no presentational hint. Return.
    if (lexer.is_eof())
        return {};

    // 5. If the character at position is a U+002B PLUS SIGN character (+), then let mode be relative-plus, and advance position to the next character. Otherwise, if the character at position is a U+002D HYPHEN-MINUS character (-), then let mode be relative-minus, and advance position to the next character. Otherwise, let mode be absolute.
    Mode mode { Mode::Absolute };

    if (lexer.peek() == '+') {
        mode = Mode::RelativePlus;
        lexer.consume();
    } else if (lexer.peek() == '-') {
        mode = Mode::RelativeMinus;
        lexer.consume();
    }

    // 6. Collect a sequence of code points that are ASCII digits from input given position, and let the resulting sequence be digits.
    size_t start_index = lexer.tell();
    lexer.consume_while(is_ascii_digit);
    size_t end_index = lexer.tell();
    auto digits = lexer.input().substring_view(start_index, end_index - start_index);
    auto value_or_empty = AK::StringUtils::convert_to_int<i32>(digits);

    // 7. If digits is the empty string, there is no presentational hint. Return.
    if (!value_or_empty.has_value())
        return {};

    // 8. Interpret digits as a base-ten integer. Let value be the resulting number.
    auto value = value_or_empty.release_value();

    // 9. If mode is relative-plus, then increment value by 3. If mode is relative-minus, then let value be the result of subtracting value from 3.
    if (mode == Mode::RelativePlus)
        value += 3;
    else if (mode == Mode::RelativeMinus)
        value = 3 - value;

    // 10. If value is greater than 7, let it be 7.
    if (value > 7)
        value = 7;

    // 11. If value is less than 1, let it be 1.
    if (value < 1)
        value = 1;

    // 12. Set 'font-size' to the keyword corresponding to the value of value according to the following table:
    switch (value) {
    case 1:
        return CSS::Keyword::XSmall;
    case 2:
        return CSS::Keyword::Small;
    case 3:
        return CSS::Keyword::Medium;
    case 4:
        return CSS::Keyword::Large;
    case 5:
        return CSS::Keyword::XLarge;
    case 6:
        return CSS::Keyword::XxLarge;
    case 7:
        return CSS::Keyword::XxxLarge;
    default:
        VERIFY_NOT_REACHED();
    }
}

HTMLFontElement::HTMLFontElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLFontElement::~HTMLFontElement() = default;

void HTMLFontElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLFontElement);
}

void HTMLFontElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name.equals_ignoring_ascii_case("color"sv)) {
            // https://html.spec.whatwg.org/multipage/rendering.html#phrasing-content-3:rules-for-parsing-a-legacy-colour-value
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                style.set_property(CSS::PropertyID::Color, CSS::CSSColorValue::create_from_color(color.value()));
        } else if (name.equals_ignoring_ascii_case("size"sv)) {
            // When a font element has a size attribute, the user agent is expected to use the following steps, known as the rules for parsing a legacy font size, to treat the attribute as a presentational hint setting the element's 'font-size' property:
            auto font_size_or_empty = parse_legacy_font_size(value);
            if (font_size_or_empty.has_value()) {
                auto font_size = string_from_keyword(font_size_or_empty.release_value());
                if (auto parsed_value = parse_css_value(CSS::Parser::ParsingContext { document() }, font_size, CSS::PropertyID::FontSize))
                    style.set_property(CSS::PropertyID::FontSize, parsed_value.release_nonnull());
            }
        }
    });
}

}
