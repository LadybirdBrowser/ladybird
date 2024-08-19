/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/StringView.h>
#include <AK/Utf8View.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <ctype.h>

namespace Web::HTML {

bool prescan_should_abort(ByteBuffer const& input, size_t const& position)
{
    return position >= input.size() || position >= 1024;
}

bool prescan_is_whitespace_or_slash(u8 const& byte)
{
    return byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r' || byte == ' ' || byte == '/';
}

bool prescan_skip_whitespace_and_slashes(ByteBuffer const& input, size_t& position)
{
    while (!prescan_should_abort(input, position) && (input[position] == '\t' || input[position] == '\n' || input[position] == '\f' || input[position] == '\r' || input[position] == ' ' || input[position] == '/'))
        ++position;
    return !prescan_should_abort(input, position);
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#algorithm-for-extracting-a-character-encoding-from-a-meta-element
Optional<StringView> extract_character_encoding_from_meta_element(ByteString const& string)
{
    // Checking for "charset" is case insensitive, as is getting an encoding.
    // Therefore, stick to lowercase from the start for simplicity.
    auto lowercase_string = string.to_lowercase();
    GenericLexer lexer(lowercase_string);

    for (;;) {
        auto charset_index = lexer.remaining().find("charset"sv);
        if (!charset_index.has_value())
            return {};

        // 7 is the length of "charset".
        lexer.ignore(charset_index.value() + 7);

        lexer.ignore_while([](char c) {
            return Infra::is_ascii_whitespace(c);
        });

        if (lexer.peek() != '=')
            continue;

        break;
    }

    // Ignore the '='.
    lexer.ignore();

    lexer.ignore_while([](char c) {
        return Infra::is_ascii_whitespace(c);
    });

    if (lexer.is_eof())
        return {};

    if (lexer.consume_specific('"')) {
        auto matching_double_quote = lexer.remaining().find('"');
        if (!matching_double_quote.has_value())
            return {};

        auto encoding = lexer.remaining().substring_view(0, matching_double_quote.value());
        return TextCodec::get_standardized_encoding(encoding);
    }

    if (lexer.consume_specific('\'')) {
        auto matching_single_quote = lexer.remaining().find('\'');
        if (!matching_single_quote.has_value())
            return {};

        auto encoding = lexer.remaining().substring_view(0, matching_single_quote.value());
        return TextCodec::get_standardized_encoding(encoding);
    }

    auto encoding = lexer.consume_until([](char c) {
        return Infra::is_ascii_whitespace(c) || c == ';';
    });
    return TextCodec::get_standardized_encoding(encoding);
}

// https://html.spec.whatwg.org/multipage/parsing.html#concept-get-attributes-when-sniffing
GC::Ptr<DOM::Attr> prescan_get_attribute(DOM::Document& document, ByteBuffer const& input, size_t& position)
{
    // 1. If the byte at position is one of 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), 0x20 (SP), or 0x2F (/) then advance position to the next byte and redo this step.
    if (!prescan_skip_whitespace_and_slashes(input, position))
        return {};

    // 2. If the byte at position is 0x3E (>), then abort the get an attribute algorithm. There isn't one.
    if (input[position] == '>')
        return {};

    // 3. Otherwise, the byte at position is the start of the attribute name. Let attribute name and attribute value be the empty string.
    // 4. Process the byte at position as follows:
    StringBuilder attribute_name;
    while (true) {
        // -> If it is 0x3D (=), and the attribute name is longer than the empty string
        if (input[position] == '=' && !attribute_name.is_empty()) {
            // Advance position to the next byte and jump to the step below labeled value.
            ++position;
            goto value;
        }
        // -> If it is 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), or 0x20 (SP)
        if (input[position] == '\t' || input[position] == '\n' || input[position] == '\f' || input[position] == '\r' || input[position] == ' ') {
            // Jump to the step below labeled spaces.
            goto spaces;
        }
        // -> If it is 0x2F (/) or 0x3E (>)
        if (input[position] == '/' || input[position] == '>') {
            // Abort the get an attribute algorithm. The attribute's name is the value of attribute name, its value is the empty string.
            return DOM::Attr::create(document, MUST(attribute_name.to_string()), String {});
        }
        // -> If it is in the range 0x41 (A) to 0x5A (Z)
        if (input[position] >= 'A' && input[position] <= 'Z') {
            // Append the code point b+0x20 to attribute name (where b is the value of the byte at position). (This converts the input to lowercase.)
            attribute_name.append_code_point(input[position] + 0x20);
        }
        // -> Anything else
        else {
            // Append the code point with the same value as the byte at position to attribute name.
            // (It doesn't actually matter how bytes outside the ASCII range are handled here,
            // since only ASCII bytes can contribute to the detection of a character encoding.)
            attribute_name.append_code_point(input[position]);
        }

        // 5. Advance position to the next byte and return to the previous step.
        ++position;
        if (prescan_should_abort(input, position))
            return {};
    }

spaces:
    // 6. Spaces: If the byte at position is one of 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), or 0x20 (SP)
    //    then advance position to the next byte, then, repeat this step.
    if (!prescan_skip_whitespace_and_slashes(input, position))
        return {};

    // 7. If the byte at position is not 0x3D (=), abort the get an attribute algorithm.
    //    The attribute's name is the value of attribute name, its value is the empty string.
    if (input[position] != '=')
        return DOM::Attr::create(document, MUST(attribute_name.to_string()), String {});

    // 8. Advance position past the 0x3D (=) byte.
    ++position;

value:
    // 9. Value: If the byte at position is one of 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), or 0x20 (SP)
    //    then advance position to the next byte, then, repeat this step.
    if (!prescan_skip_whitespace_and_slashes(input, position))
        return {};

    StringBuilder attribute_value;
    // 10. Process the byte at position as follows:

    // -> If it is 0x22 (") or 0x27 (')
    if (input[position] == '"' || input[position] == '\'') {
        // 1. Let b be the value of the byte at position.
        u8 quote_character = input[position];

        // 2. Quote loop: Advance position to the next byte.
        ++position;

        for (; !prescan_should_abort(input, position); ++position) {
            // 3. If the value of the byte at position is the value of b, then advance position to the next byte
            //    and abort the "get an attribute" algorithm.
            //    The attribute's name is the value of attribute name, and its value is the value of attribute value.
            if (input[position] == quote_character)
                return DOM::Attr::create(document, MUST(attribute_name.to_string()), MUST(attribute_value.to_string()));

            // 4. Otherwise, if the value of the byte at position is in the range 0x41 (A) to 0x5A (Z),
            //    then append a code point to attribute value whose value is 0x20 more than the value of the byte at position.
            if (input[position] >= 'A' && input[position] <= 'Z') {
                attribute_value.append_code_point(input[position] + 0x20);
            }
            // 5. Otherwise, append a code point to attribute value whose value is the same as the value of the byte at position.
            else {
                attribute_value.append_code_point(input[position]);
            }

            // 6. Return to the step above labeled quote loop.
        }
        return {};
    }

    // -> If it is 0x3E (>)
    if (input[position] == '>') {
        // Abort the get an attribute algorithm. The attribute's name is the value of attribute name, its value is the empty string.
        return DOM::Attr::create(document, MUST(attribute_name.to_string()), String {});
    }

    // -> If it is in the range 0x41 (A) to 0x5A (Z)
    if (input[position] >= 'A' && input[position] <= 'Z') {
        // Append a code point b+0x20 to attribute value (where b is the value of the byte at position).
        attribute_value.append_code_point(input[position] + 0x20);
        // Advance position to the next byte.
        ++position;
    }
    // -> Anything else
    else {
        // Append a code point with the same value as the byte at position to attribute value.
        attribute_value.append_code_point(input[position]);
        // Advance position to the next byte.
        ++position;
    }

    if (prescan_should_abort(input, position))
        return {};

    // 11. Process the byte at position as follows:
    for (; !prescan_should_abort(input, position); ++position) {
        // -> If it is 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), 0x20 (SP), or 0x3E (>)
        if (input[position] == '\t' || input[position] == '\n' || input[position] == '\f' || input[position] == '\r' || input[position] == ' ' || input[position] == '>') {
            // Abort the get an attribute algorithm. The attribute's name is the value of attribute name and its value is the value of attribute value.
            return DOM::Attr::create(document, MUST(attribute_name.to_string()), MUST(attribute_value.to_string()));
        }

        // -> If it is in the range 0x41 (A) to 0x5A (Z)
        if (input[position] >= 'A' && input[position] <= 'Z') {
            // Append a code point b+0x20 to attribute value (where b is the value of the byte at position).
            attribute_value.append_code_point(input[position] + 0x20);
        }
        // -> Anything else
        else {
            // Append a code point with the same value as the byte at position to attribute value.
            attribute_value.append_code_point(input[position]);
        }

        // 12. Advance position to the next byte and return to the previous step.
    }
    return {};
}

// https://html.spec.whatwg.org/multipage/parsing.html#prescan-a-byte-stream-to-determine-its-encoding
Optional<ByteString> run_prescan_byte_stream_algorithm(DOM::Document& document, ByteBuffer const& input)
{
    // https://html.spec.whatwg.org/multipage/parsing.html#prescan-a-byte-stream-to-determine-its-encoding

    // Detects '<?x'
    if (!prescan_should_abort(input, 5)) {
        // A sequence of bytes starting with: 0x3C, 0x0, 0x3F, 0x0, 0x78, 0x0
        if (input[0] == 0x3C && input[1] == 0x00 && input[2] == 0x3F && input[3] == 0x00 && input[4] == 0x78 && input[5] == 0x00)
            return "utf-16le";
        // A sequence of bytes starting with: 0x0, 0x3C, 0x0, 0x3F, 0x0, 0x78
        if (input[0] == 0x00 && input[1] == 0x3C && input[2] == 0x00 && input[3] == 0x3F && input[4] == 0x00 && input[5] == 0x78)
            return "utf-16be";
    }

    for (size_t position = 0; !prescan_should_abort(input, position); ++position) {
        if (!prescan_should_abort(input, position + 5) && input[position] == '<' && input[position + 1] == '!'
            && input[position + 2] == '-' && input[position + 3] == '-') {
            position += 2;
            for (; !prescan_should_abort(input, position + 3); ++position) {
                if (input[position] == '-' && input[position + 1] == '-' && input[position + 2] == '>') {
                    position += 2;
                    break;
                }
            }
        } else if (!prescan_should_abort(input, position + 6)
            && input[position] == '<'
            && (input[position + 1] == 'M' || input[position + 1] == 'm')
            && (input[position + 2] == 'E' || input[position + 2] == 'e')
            && (input[position + 3] == 'T' || input[position + 3] == 't')
            && (input[position + 4] == 'A' || input[position + 4] == 'a')
            && prescan_is_whitespace_or_slash(input[position + 5])) {
            position += 6;
            Vector<FlyString> attribute_list {};
            bool got_pragma = false;
            Optional<bool> need_pragma {};
            Optional<ByteString> charset {};

            while (true) {
                auto attribute = prescan_get_attribute(document, input, position);
                if (!attribute)
                    break;
                if (attribute_list.contains_slow(attribute->name()))
                    continue;
                auto const& attribute_name = attribute->name();
                attribute_list.append(attribute->name());

                if (attribute_name == "http-equiv") {
                    got_pragma = attribute->value() == "content-type";
                } else if (attribute_name == "content") {
                    auto encoding = extract_character_encoding_from_meta_element(attribute->value().to_byte_string());
                    if (encoding.has_value() && !charset.has_value()) {
                        charset = encoding.value();
                        need_pragma = true;
                    }
                } else if (attribute_name == "charset") {
                    auto maybe_charset = TextCodec::get_standardized_encoding(attribute->value());
                    if (maybe_charset.has_value()) {
                        charset = Optional<ByteString> { maybe_charset };
                        need_pragma = { false };
                    }
                }
            }

            if (!need_pragma.has_value() || (need_pragma.value() && !got_pragma) || !charset.has_value())
                continue;
            if (charset.value() == "UTF-16BE/LE")
                return "UTF-8";
            else if (charset.value() == "x-user-defined")
                return "windows-1252";
            else
                return charset.value();
        } else if (!prescan_should_abort(input, position + 3) && input[position] == '<'
            && ((input[position + 1] == '/' && isalpha(input[position + 2])) || isalpha(input[position + 1]))) {
            position += 2;
            prescan_skip_whitespace_and_slashes(input, position);
            while (prescan_get_attribute(document, input, position)) { };
        } else if (!prescan_should_abort(input, position + 1) && input[position] == '<' && (input[position + 1] == '!' || input[position + 1] == '/' || input[position + 1] == '?')) {
            position += 1;
            do {
                position += 1;
                if (prescan_should_abort(input, position))
                    return {};
            } while (input[position] != '>');
        } else {
            // Do nothing.
        }
    }
    return {};
}

// https://html.spec.whatwg.org/multipage/parsing.html#determining-the-character-encoding
ByteString run_encoding_sniffing_algorithm(DOM::Document& document, ByteBuffer const& input)
{
    if (input.size() >= 2) {
        if (input[0] == 0xFE && input[1] == 0xFF) {
            return "UTF-16BE";
        } else if (input[0] == 0xFF && input[1] == 0xFE) {
            return "UTF-16LE";
        } else if (input.size() >= 3 && input[0] == 0xEF && input[1] == 0xBB && input[2] == 0xBF) {
            return "UTF-8";
        }
    }

    // FIXME: If the user has explicitly instructed the user agent to override the document's character
    //        encoding with a specific encoding.
    // FIXME: The user agent may wait for more bytes of the resource to be available, either in this step or
    //        at any later step in this algorithm.
    // FIXME: If the transport layer specifies a character encoding, and it is supported.

    auto optional_encoding = run_prescan_byte_stream_algorithm(document, input);
    if (optional_encoding.has_value()) {
        return optional_encoding.value();
    }

    // FIXME: If the HTML parser for which this algorithm is being run is associated with a Document whose browsing context
    //        is non-null and a child browsing context.
    // FIXME: If the user agent has information on the likely encoding for this page, e.g. based on the encoding of the page
    //        when it was last visited.

    if (!Utf8View(StringView(input)).validate()) {
        // FIXME: As soon as Locale is supported, this should sometimes return a different encoding based on the locale.
        return "windows-1252";
    }

    // NOTE: This is the authoritative place to actually decide on using the default encoding as per the HTML specification.
    //       "Otherwise, return an implementation-defined or user-specified default character encoding, [...]."
    return "UTF-8";
}

}
