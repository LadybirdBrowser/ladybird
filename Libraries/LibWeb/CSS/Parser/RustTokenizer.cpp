/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FFIHelpers.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/CSS/CharacterTypes.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/CSS/Parser/RustTokenizer.h>
#include <LibWeb/RustFFI.h>

namespace Web::CSS::Parser {

// U+FFFD REPLACEMENT CHARACTER (�)
static constexpr u32 REPLACEMENT_CHARACTER = 0xFFFD;

Utf16String RustTokenizer::normalize_input(StringView input, StringView encoding, TokenizerInput tokenizer_input)
{
    // https://www.w3.org/TR/css-syntax-3/#css-filter-code-points
    auto standardized_encoding = TextCodec::get_standardized_encoding(encoding);
    VERIFY(standardized_encoding.has_value());
    auto decoder = TextCodec::decoder_for(encoding);
    VERIFY(decoder.has_value());

    auto decoded_input = [&] {
        if (tokenizer_input == TokenizerInput::DecodedText) {
            VERIFY(Utf8View { input }.validate());
            return String::from_utf8_without_validation(input.bytes());
        }
        if (standardized_encoding->equals_ignoring_ascii_case("utf-8"sv) && Utf8View { input }.validate(AllowLonelySurrogates::No)) {
            if (input.bytes().starts_with({ { 0xef, 0xbb, 0xbf } }))
                input = input.substring_view(3);
            return String::from_utf8_without_validation(input.bytes());
        }
        return MUST(decoder->to_utf8(input, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement));
    }();

    return normalize_input(Utf16String::from_utf8_without_validation(decoded_input));
}

bool RustTokenizer::input_needs_normalization(Utf16View input)
{
    for (auto code_point : input) {
        if (code_point == '\r' || code_point == '\f' || code_point == 0x00 || is_unicode_surrogate(code_point))
            return true;
    }
    return false;
}

Utf16String RustTokenizer::normalize_input(Utf16View input)
{
    // OPTIMIZATION: If the input doesn't contain any filterable characters, we can skip the filtering.
    if (!input_needs_normalization(input))
        return Utf16String::from_utf16(input);

    Utf16StringBuilder builder { input.length_in_code_units() };
    bool last_was_carriage_return = false;

    // To filter code points from a stream of (unfiltered) code points input:
    for (auto code_point : input) {
        // Replace any U+000D CARRIAGE RETURN (CR) code points,
        // U+000C FORM FEED (FF) code points,
        // or pairs of U+000D CARRIAGE RETURN (CR) followed by U+000A LINE FEED (LF)
        // in input by a single U+000A LINE FEED (LF) code point.
        if (code_point == '\r') {
            if (last_was_carriage_return) {
                builder.append_code_point('\n');
            } else {
                last_was_carriage_return = true;
            }
        } else {
            if (last_was_carriage_return)
                builder.append_code_point('\n');

            if (code_point == '\n') {
                if (!last_was_carriage_return)
                    builder.append_code_point('\n');

            } else if (code_point == '\f') {
                builder.append_code_point('\n');
                // Replace any U+0000 NULL or surrogate code points in input with U+FFFD REPLACEMENT CHARACTER (�).
            } else if (code_point == 0x00 || is_unicode_surrogate(code_point)) {
                builder.append_code_point(REPLACEMENT_CHARACTER);
            } else {
                builder.append_code_point(code_point);
            }

            last_was_carriage_return = false;
        }
    }

    return builder.to_string();
}

static Number::Type css_number_type_from_ffi(FFI::CssNumberType number_type)
{
    switch (number_type) {
    case FFI::CssNumberType::Number:
        return Number::Type::Number;
    case FFI::CssNumberType::IntegerWithExplicitSign:
        return Number::Type::IntegerWithExplicitSign;
    case FFI::CssNumberType::Integer:
        return Number::Type::Integer;
    }
    VERIFY_NOT_REACHED();
}

static SourcePosition position_from_ffi(size_t line, size_t column)
{
    VERIFY(line <= NumericLimits<u32>::max());
    VERIFY(column <= NumericLimits<u32>::max());
    return { static_cast<u32>(line), static_cast<u32>(column) };
}

Token RustTokenizer::token_from_ffi(FFI::CssToken const& ffi_token)
{
    auto original_source_text = [&] {
        if (ffi_token.original_source_len == 0)
            return Utf16String {};
        if (ffi_token.original_source_ascii_ptr)
            return Utf16String::from_utf8_without_validation(StringView { ffi_token.original_source_ascii_ptr, ffi_token.original_source_len });
        return Utf16String::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(ffi_token.original_source_utf16_ptr), ffi_token.original_source_len });
    }();
    auto payload = ffi_token.value_len == 0
        ? Utf16FlyString {}
        : Utf16FlyString::from_utf16(Utf16View { reinterpret_cast<char16_t const*>(ffi_token.value_ptr), ffi_token.value_len });

    Token token;
    switch (ffi_token.token_type) {
    case FFI::CssTokenType::Invalid:
        VERIFY_NOT_REACHED();
    case FFI::CssTokenType::EndOfFile:
        token = Token::create(Token::Type::EndOfFile, move(original_source_text));
        break;
    case FFI::CssTokenType::Ident:
        token = Token::create_ident(move(payload), move(original_source_text));
        break;
    case FFI::CssTokenType::Function:
        token = Token::create_function(move(payload), move(original_source_text));
        break;
    case FFI::CssTokenType::AtKeyword:
        token = Token::create_at_keyword(move(payload), move(original_source_text));
        break;
    case FFI::CssTokenType::Hash:
        token = Token::create_hash(
            move(payload),
            ffi_token.hash_type == FFI::CssHashType::Id ? Token::HashType::Id : Token::HashType::Unrestricted,
            move(original_source_text));
        break;
    case FFI::CssTokenType::String:
        token = Token::create_string(move(payload), move(original_source_text));
        break;
    case FFI::CssTokenType::BadString:
        token = Token::create(Token::Type::BadString, move(original_source_text));
        break;
    case FFI::CssTokenType::Url:
        token = Token::create_url(move(payload), move(original_source_text));
        break;
    case FFI::CssTokenType::BadUrl:
        token = Token::create(Token::Type::BadUrl, move(original_source_text));
        break;
    case FFI::CssTokenType::Delim:
        token = Token::create_delim(ffi_token.delim, move(original_source_text));
        break;
    case FFI::CssTokenType::Number:
        token = Token::create_number(Number { css_number_type_from_ffi(ffi_token.number_type), ffi_token.number_value }, move(original_source_text));
        break;
    case FFI::CssTokenType::Percentage:
        token = Token::create_percentage(Number { css_number_type_from_ffi(ffi_token.number_type), ffi_token.number_value }, move(original_source_text));
        break;
    case FFI::CssTokenType::Dimension:
        token = Token::create_dimension(
            Number { css_number_type_from_ffi(ffi_token.number_type), ffi_token.number_value },
            move(payload),
            move(original_source_text));
        break;
    case FFI::CssTokenType::Whitespace:
        token = Token::create_whitespace(move(original_source_text));
        break;
    case FFI::CssTokenType::CDO:
        token = Token::create(Token::Type::CDO, move(original_source_text));
        break;
    case FFI::CssTokenType::CDC:
        token = Token::create(Token::Type::CDC, move(original_source_text));
        break;
    case FFI::CssTokenType::Colon:
        token = Token::create(Token::Type::Colon, move(original_source_text));
        break;
    case FFI::CssTokenType::Semicolon:
        token = Token::create(Token::Type::Semicolon, move(original_source_text));
        break;
    case FFI::CssTokenType::Comma:
        token = Token::create(Token::Type::Comma, move(original_source_text));
        break;
    case FFI::CssTokenType::OpenSquare:
        token = Token::create(Token::Type::OpenSquare, move(original_source_text));
        break;
    case FFI::CssTokenType::CloseSquare:
        token = Token::create(Token::Type::CloseSquare, move(original_source_text));
        break;
    case FFI::CssTokenType::OpenParen:
        token = Token::create(Token::Type::OpenParen, move(original_source_text));
        break;
    case FFI::CssTokenType::CloseParen:
        token = Token::create(Token::Type::CloseParen, move(original_source_text));
        break;
    case FFI::CssTokenType::OpenCurly:
        token = Token::create(Token::Type::OpenCurly, move(original_source_text));
        break;
    case FFI::CssTokenType::CloseCurly:
        token = Token::create(Token::Type::CloseCurly, move(original_source_text));
        break;
    }

    token.set_position_range(Badge<RustTokenizer> {}, position_from_ffi(ffi_token.start_line, ffi_token.start_column), position_from_ffi(ffi_token.end_line, ffi_token.end_column));
    return token;
}

static_assert(static_cast<u8>(FFI::CssTokenType::Invalid) == static_cast<u8>(Token::Type::Invalid));
static_assert(static_cast<u8>(FFI::CssTokenType::EndOfFile) == static_cast<u8>(Token::Type::EndOfFile));
static_assert(static_cast<u8>(FFI::CssTokenType::Ident) == static_cast<u8>(Token::Type::Ident));
static_assert(static_cast<u8>(FFI::CssTokenType::Function) == static_cast<u8>(Token::Type::Function));
static_assert(static_cast<u8>(FFI::CssTokenType::AtKeyword) == static_cast<u8>(Token::Type::AtKeyword));
static_assert(static_cast<u8>(FFI::CssTokenType::Hash) == static_cast<u8>(Token::Type::Hash));
static_assert(static_cast<u8>(FFI::CssTokenType::String) == static_cast<u8>(Token::Type::String));
static_assert(static_cast<u8>(FFI::CssTokenType::BadString) == static_cast<u8>(Token::Type::BadString));
static_assert(static_cast<u8>(FFI::CssTokenType::Url) == static_cast<u8>(Token::Type::Url));
static_assert(static_cast<u8>(FFI::CssTokenType::BadUrl) == static_cast<u8>(Token::Type::BadUrl));
static_assert(static_cast<u8>(FFI::CssTokenType::Delim) == static_cast<u8>(Token::Type::Delim));
static_assert(static_cast<u8>(FFI::CssTokenType::Number) == static_cast<u8>(Token::Type::Number));
static_assert(static_cast<u8>(FFI::CssTokenType::Percentage) == static_cast<u8>(Token::Type::Percentage));
static_assert(static_cast<u8>(FFI::CssTokenType::Dimension) == static_cast<u8>(Token::Type::Dimension));
static_assert(static_cast<u8>(FFI::CssTokenType::Whitespace) == static_cast<u8>(Token::Type::Whitespace));
static_assert(static_cast<u8>(FFI::CssTokenType::CDO) == static_cast<u8>(Token::Type::CDO));
static_assert(static_cast<u8>(FFI::CssTokenType::CDC) == static_cast<u8>(Token::Type::CDC));
static_assert(static_cast<u8>(FFI::CssTokenType::Colon) == static_cast<u8>(Token::Type::Colon));
static_assert(static_cast<u8>(FFI::CssTokenType::Semicolon) == static_cast<u8>(Token::Type::Semicolon));
static_assert(static_cast<u8>(FFI::CssTokenType::Comma) == static_cast<u8>(Token::Type::Comma));
static_assert(static_cast<u8>(FFI::CssTokenType::OpenSquare) == static_cast<u8>(Token::Type::OpenSquare));
static_assert(static_cast<u8>(FFI::CssTokenType::CloseSquare) == static_cast<u8>(Token::Type::CloseSquare));
static_assert(static_cast<u8>(FFI::CssTokenType::OpenParen) == static_cast<u8>(Token::Type::OpenParen));
static_assert(static_cast<u8>(FFI::CssTokenType::CloseParen) == static_cast<u8>(Token::Type::CloseParen));
static_assert(static_cast<u8>(FFI::CssTokenType::OpenCurly) == static_cast<u8>(Token::Type::OpenCurly));
static_assert(static_cast<u8>(FFI::CssTokenType::CloseCurly) == static_cast<u8>(Token::Type::CloseCurly));
static_assert(static_cast<u8>(FFI::CssHashType::Id) == static_cast<u8>(Token::HashType::Id));
static_assert(static_cast<u8>(FFI::CssHashType::Unrestricted) == static_cast<u8>(Token::HashType::Unrestricted));
static_assert(static_cast<u8>(FFI::CssNumberType::Number) == static_cast<u8>(Number::Type::Number));
static_assert(static_cast<u8>(FFI::CssNumberType::IntegerWithExplicitSign) == static_cast<u8>(Number::Type::IntegerWithExplicitSign));
static_assert(static_cast<u8>(FFI::CssNumberType::Integer) == static_cast<u8>(Number::Type::Integer));

Vector<Token> RustTokenizer::tokenize(StringView input, StringView encoding, TokenizerInput tokenizer_input)
{
    auto filtered_input = normalize_input(input, encoding, tokenizer_input);
    return tokenize(filtered_input);
}

Vector<Token> RustTokenizer::tokenize(Utf16View filtered_input)
{
    struct CallbackContext {
        Vector<Token> tokens;
    };

    CallbackContext context;
    context.tokens.ensure_capacity((filtered_input.length_in_code_units() / 2) + 1);
    FFI::rust_css_tokenize(
        filtered_input.has_ascii_storage() ? reinterpret_cast<u8 const*>(filtered_input.ascii_span().data()) : nullptr,
        filtered_input.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(filtered_input.utf16_span().data()),
        filtered_input.length_in_code_units(),
        &context,
        [](void* raw_context, FFI::CssToken const* ffi_token) {
            auto& context = *static_cast<CallbackContext*>(raw_context);
            context.tokens.append(token_from_ffi(*ffi_token));
        });

    return move(context.tokens);
}

}
