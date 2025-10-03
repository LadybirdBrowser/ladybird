/*
 * Pascal-backed CSS tokenizer entry
 * Provides Tokenizer::tokenize() implemented via the FreePascal tokenizer.
 */

#include <AK/Vector.h>
#include <AK/String.h>
#include <AK/Debug.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/CSS/CharacterTypes.h>
#include <LibWeb/CSS/Parser/Tokenizer.h>
#include <LibWeb/CSS/Parser/PascalTokenizerBridge.h>
#include <LibWeb/CSS/Number.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::CSS::Parser {

// U+FFFD REPLACEMENT CHARACTER (�)
#define REPLACEMENT_CHARACTER 0xFFFD


Vector<Token> Tokenizer::tokenize(StringView input, StringView encoding)
{
    // https://www.w3.org/TR/css-syntax-3/#css-filter-code-points
    auto filter_code_points = [](StringView input, auto encoding) -> String {
        auto decoder = TextCodec::decoder_for(encoding);
        VERIFY(decoder.has_value());

        auto decoded_input = MUST(decoder->to_utf8(input));

        bool const contains_filterable = [&] {
            for (auto code_point : decoded_input.code_points()) {
                if (code_point == '\r' || code_point == '\f' || code_point == 0x00 || is_unicode_surrogate(code_point))
                    return true;
            }
            return false;
        }();
        if (!contains_filterable)
            return decoded_input;

        StringBuilder builder { input.length() };
        bool last_was_carriage_return = false;

        for (auto code_point : decoded_input.code_points()) {
            if (code_point == '\r') {
                if (last_was_carriage_return) {
                    builder.append('\n');
                } else {
                    last_was_carriage_return = true;
                }
            } else {
                if (last_was_carriage_return)
                    builder.append('\n');

                if (code_point == '\n') {
                    if (!last_was_carriage_return)
                        builder.append('\n');
                } else if (code_point == '\f') {
                    builder.append('\n');
                } else if (code_point == 0x00 || is_unicode_surrogate(code_point)) {
                    builder.append_code_point(REPLACEMENT_CHARACTER);
                } else {
                    builder.append_code_point(code_point);
                }

                last_was_carriage_return = false;
            }
        }
        return builder.to_string_without_validation();
    };

    auto decoded = filter_code_points(input, encoding);

    // Build a line-start index so we can convert Pascal line/column
    // positions to byte offsets for original_source_text slices.
    Vector<size_t> line_starts;
    line_starts.append(0);
    auto bytes_view = decoded.bytes();
    for (size_t i = 0; i < bytes_view.size(); ++i) {
        if (bytes_view[i] == '\n')
            line_starts.append(i + 1);
    }
    auto decoded_sv = decoded.bytes_as_string_view();

    // (no-op) retained placeholder removed; slicing done in emit() using userdata

    Vector<Token> tokens;
    struct CallbackUserdata {
        Vector<Token>* tokens;
        StringView* decoded_sv;
        Vector<size_t>* line_starts;
    } userdata { &tokens, &decoded_sv, &line_starts };

    auto emit = [](void* ud, const LB_CssTokenLite* token_lite, const char* str1, size_t str1_len, const char* str2, size_t str2_len) {
        (void)str2;
        (void)str2_len;
        auto* data = reinterpret_cast<CallbackUserdata*>(ud);
        auto& out = *data->tokens;

        auto pos_start = Token::Position { token_lite->start_line, token_lite->start_col };
        auto pos_end = Token::Position { token_lite->end_line, token_lite->end_col };
        auto& line_starts = *data->line_starts;
        auto& decoded_sv_local = *data->decoded_sv;
        auto clamp_line = [&](u32 line) -> size_t {
            if (line >= line_starts.size())
                return line_starts.size() - 1;
            return line;
        };
        size_t sl = clamp_line(token_lite->start_line);
        size_t el = clamp_line(token_lite->end_line);
        size_t start_off = line_starts[sl] + token_lite->start_col;
        size_t end_off = line_starts[el] + token_lite->end_col;
        String original;
        if (start_off < end_off && start_off < decoded_sv_local.length()) {
            end_off = min(end_off, (size_t)decoded_sv_local.length());
            auto sv2 = decoded_sv_local.substring_view(start_off, end_off - start_off);
            original = MUST(String::from_utf8(sv2));
        }

        auto make_fly = [&](const char* s, size_t len) -> FlyString {
            if (!s || len == 0)
                return {};
            auto str = MUST(String::from_utf8({ s, len }));
            return FlyString { str };
        };

        Token token;
        switch (static_cast<LB_TokenType>(token_lite->token_type)) {
        case LB_EndOfFile:
            // C++ tokenizer uses empty original_source_text for EOF.
            token = Token::create(Token::Type::EndOfFile, {});
            break;
        case LB_Ident:
            token = Token::create_ident(make_fly(str1, str1_len), original);
            break;
        case LB_Function:
            token = Token::create_function(make_fly(str1, str1_len), original);
            break;
        case LB_AtKeyword:
            token = Token::create_at_keyword(make_fly(str1, str1_len), original);
            break;
        case LB_Hash: {
            auto hash_type = static_cast<LB_HashType>(token_lite->hash_type) == LB_Hash_Id ? Token::HashType::Id : Token::HashType::Unrestricted;
            token = Token::create_hash(make_fly(str1, str1_len), hash_type, original);
            break;
        }
        case LB_String:
            token = Token::create_string(make_fly(str1, str1_len), original);
            break;
        case LB_BadString:
            token = Token::create(Token::Type::BadString, move(original));
            break;
        case LB_Url:
            token = Token::create_url(make_fly(str1, str1_len), original);
            break;
        case LB_BadUrl:
            token = Token::create(Token::Type::BadUrl, move(original));
            break;
        case LB_Delim:
            token = Token::create_delim(token_lite->delim, move(original));
            break;
        case LB_Number: {
            auto number_type = static_cast<LB_NumberType>(token_lite->number_type);
            CSS::Number::Type t = CSS::Number::Type::Number;
            if (number_type == LB_Number_Integer)
                t = CSS::Number::Type::Integer;
            else if (number_type == LB_Number_IntegerWithExplicitSign)
                t = CSS::Number::Type::IntegerWithExplicitSign;
            token = Token::create_number(CSS::Number { t, token_lite->number_value }, original);
            break;
        }
        case LB_Percentage: {
            auto number_type = static_cast<LB_NumberType>(token_lite->number_type);
            CSS::Number::Type t = CSS::Number::Type::Number;
            if (number_type == LB_Number_Integer)
                t = CSS::Number::Type::Integer;
            else if (number_type == LB_Number_IntegerWithExplicitSign)
                t = CSS::Number::Type::IntegerWithExplicitSign;
            token = Token::create_percentage(CSS::Number { t, token_lite->number_value }, original);
            break;
        }
        case LB_Dimension: {
            auto unit = make_fly(str1, str1_len);
            auto number_type = static_cast<LB_NumberType>(token_lite->number_type);
            CSS::Number::Type t = CSS::Number::Type::Number;
            if (number_type == LB_Number_Integer)
                t = CSS::Number::Type::Integer;
            else if (number_type == LB_Number_IntegerWithExplicitSign)
                t = CSS::Number::Type::IntegerWithExplicitSign;
            token = Token::create_dimension(CSS::Number { t, token_lite->number_value }, move(unit), original);
            break;
        }
        case LB_Whitespace:
            token = Token::create_whitespace(move(original));
            break;
        case LB_CDO:
            token = Token::create(Token::Type::CDO, move(original));
            break;
        case LB_CDC:
            token = Token::create(Token::Type::CDC, move(original));
            break;
        case LB_Colon:
            token = Token::create(Token::Type::Colon, move(original));
            break;
        case LB_Semicolon:
            token = Token::create(Token::Type::Semicolon, move(original));
            break;
        case LB_Comma:
            token = Token::create(Token::Type::Comma, move(original));
            break;
        case LB_OpenSquare:
            token = Token::create(Token::Type::OpenSquare, move(original));
            break;
        case LB_CloseSquare:
            token = Token::create(Token::Type::CloseSquare, move(original));
            break;
        case LB_OpenParen:
            token = Token::create(Token::Type::OpenParen, move(original));
            break;
        case LB_CloseParen:
            token = Token::create(Token::Type::CloseParen, move(original));
            break;
        case LB_OpenCurly:
            token = Token::create(Token::Type::OpenCurly, move(original));
            break;
        case LB_CloseCurly:
            token = Token::create(Token::Type::CloseCurly, move(original));
            break;
        default:
            token = Token::create(Token::Type::Invalid);
            break;
        }

        token.set_position_range({}, pos_start, pos_end);
        out.append(move(token));
    };

    auto bytes = decoded.bytes();
    (void)lb_css_tokenize_stream(reinterpret_cast<char const*>(bytes.data()), bytes.size(), emit, &userdata);

    return tokens;
}

Token Tokenizer::create_eof_token()
{
    return Token::create(Token::Type::EndOfFile);
}

}
