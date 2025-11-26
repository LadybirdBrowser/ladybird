/*
 * Copyright (c) 2021, Max Wipfli <mail@maxwipfli.ch>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
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
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::HTML {

static bool prescan_should_abort(ReadonlyBytes input, size_t const& position)
{
    return position >= input.size() || position >= 1024;
}

static constexpr bool is_whitespace(u8 byte)
{
    return byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r' || byte == ' ';
}

static constexpr bool is_whitespace_or_slash(u8 byte)
{
    return is_whitespace(byte) || byte == '/';
}

static constexpr bool is_whitespace_or_end_chevron(u8 byte)
{
    return is_whitespace(byte) || byte == '>';
}

static bool prescan_skip_whitespace_and_slashes(ReadonlyBytes input, size_t& position)
{
    while (!prescan_should_abort(input, position) && is_whitespace_or_slash(input[position]))
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
GC::Ptr<DOM::Attr> prescan_get_attribute(DOM::Document& document, ReadonlyBytes input, size_t& position)
{
    // 1. If the byte at position is one of 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), 0x20 (SP), or 0x2F (/),
    //    then advance position to the next byte and redo this step.
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
        if (is_whitespace(input[position])) {
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
    // 6. Spaces: If the byte at position is one of 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), or 0x20 (SP),
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
    // 9. Value: If the byte at position is one of 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), or 0x20 (SP),
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
            if (input[position] == quote_character) {
                ++position;
                return DOM::Attr::create(document, MUST(attribute_name.to_string()), MUST(attribute_value.to_string()));
            }

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
        if (is_whitespace_or_end_chevron(input[position])) {
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
Optional<ByteString> run_prescan_byte_stream_algorithm(DOM::Document& document, ReadonlyBytes input)
{
    // 1. Let position be a pointer to a byte in the input byte stream, initially pointing at the first byte.
    size_t position = 0;

    // 2. Prescan for UTF-16 XML declarations: If position points to:
    if (!prescan_should_abort(input, position + 5)) {
        // * A sequence of bytes starting with: 0x3C, 0x0, 0x3F, 0x0, 0x78, 0x0 (case-sensitive UTF-16 little-endian '<?x')
        //       Return UTF-16LE.
        if (input.slice(position, 6) == Array<u8, 6> { 0x3C, 0x0, 0x3F, 0x0, 0x78, 0x0 })
            return "utf-16le";

        // * A sequence of bytes starting with: 0x0, 0x3C, 0x0, 0x3F, 0x0, 0x78 (case-sensitive UTF-16 big-endian '<?x')
        //       Return UTF-16BE.
        if (input.slice(position, 6) == Array<u8, 6> { 0x0, 0x3C, 0x0, 0x3F, 0x0, 0x78 })
            return "utf-16be";
    }

    // NOTE: For historical reasons, the prefix is two bytes longer than in Appendix F of XML and the encoding name is
    //       not checked.

    // 3. Loop: If position points to:
    while (!prescan_should_abort(input, position)) {
        // * A sequence of bytes starting with: 0x3C 0x21 0x2D 0x2D (`<!--`)
        if (!prescan_should_abort(input, position + 5) && input[position] == '<' && input[position + 1] == '!'
            && input[position + 2] == '-' && input[position + 3] == '-') {
            // Advance the position pointer so that it points at the first 0x3E byte which is preceded by two 0x2D bytes
            // (i.e. at the end of an ASCII '-->' sequence) and comes after the 0x3C byte that was found. (The two 0x2D
            // bytes can be the same as those in the '<!--' sequence.)
            position += 2;
            for (; !prescan_should_abort(input, position + 3); ++position) {
                if (input[position] == '-' && input[position + 1] == '-' && input[position + 2] == '>') {
                    position += 2;
                    break;
                }
            }
        }

        // * A sequence of bytes starting with: 0x3C, 0x4D or 0x6D, 0x45 or 0x65, 0x54 or 0x74, 0x41 or 0x61, and one of
        //   0x09, 0x0A, 0x0C, 0x0D, 0x20, 0x2F (case-insensitive ASCII '<meta' followed by a space or slash)
        else if (!prescan_should_abort(input, position + 6)
            && input[position] == '<'
            && (input[position + 1] == 'M' || input[position + 1] == 'm')
            && (input[position + 2] == 'E' || input[position + 2] == 'e')
            && (input[position + 3] == 'T' || input[position + 3] == 't')
            && (input[position + 4] == 'A' || input[position + 4] == 'a')
            && is_whitespace_or_slash(input[position + 5])) {
            // 1. Advance the position pointer so that it points at the next 0x09, 0x0A, 0x0C, 0x0D, 0x20, or 0x2F byte
            //    (the one in sequence of characters matched above).
            position += 6;

            // 2. Let attribute list be an empty list of strings.
            Vector<FlyString> attribute_list {};

            // 3. Let got pragma be false.
            bool got_pragma = false;

            // 4. Let need pragma be null.
            Optional<bool> need_pragma {};

            // 5. Let charset be the null value (which, for the purposes of this algorithm, is distinct from an
            //    unrecognized encoding or the empty string).
            Optional<ByteString> charset {};

            while (true) {
                // 6. Attributes: Get an attribute and its value. If no attribute was sniffed, then jump to the
                //    processing step below.
                auto attribute = prescan_get_attribute(document, input, position);
                if (!attribute)
                    break;

                // 7. If the attribute's name is already in attribute list, then return to the step labeled attributes.
                if (attribute_list.contains_slow(attribute->name()))
                    continue;

                // 8. Add the attribute's name to attribute list.
                auto const& attribute_name = attribute->name();
                attribute_list.append(attribute->name());

                // 9. Run the appropriate step from the following list, if one applies:
                //    * If the attribute's name is "http-equiv"
                if (attribute_name == AttributeNames::http_equiv) {
                    // If the attribute's value is "content-type", then set got pragma to true.
                    got_pragma = attribute->value() == "content-type";
                }

                //    * If the attribute's name is "content"
                else if (attribute_name == AttributeNames::content) {
                    // Apply the algorithm for extracting a character encoding from a meta element, giving the
                    // attribute's value as the string to parse. If a character encoding is returned, and if charset is
                    // still set to null, let charset be the encoding returned, and set need pragma to true.
                    auto encoding = extract_character_encoding_from_meta_element(attribute->value().to_byte_string());
                    if (encoding.has_value() && !charset.has_value()) {
                        charset = encoding.value();
                        need_pragma = true;
                    }
                }

                //    * If the attribute's name is "charset"
                else if (attribute_name == AttributeNames::charset) {
                    // Let charset be the result of getting an encoding from the attribute's value, and set need pragma
                    // to false.
                    auto maybe_charset = TextCodec::get_standardized_encoding(attribute->value());
                    if (maybe_charset.has_value()) {
                        charset = Optional<ByteString> { maybe_charset };
                        need_pragma = { false };
                    }
                }

                // 10. Return to the step labeled attributes.
            }

            // 11. Processing: If need pragma is null, then jump to the step below labeled next byte.
            if (!need_pragma.has_value())
                continue;

            // 12. If need pragma is true but got pragma is false, then jump to the step below labeled next byte.
            if (need_pragma.value() && !got_pragma)
                continue;

            // 13. If charset is failure, then jump to the step below labeled next byte.
            if (!charset.has_value())
                continue;

            // 14. If charset is UTF-16BE/LE, then set charset to UTF-8.
            // NB: https://encoding.spec.whatwg.org/#common-infrastructure-for-utf-16be-and-utf-16le
            if (charset.value() == "UTF-16BE" || charset.value() == "UTF-16LE")
                return "UTF-8";

            // 15. If charset is x-user-defined, then set charset to windows-1252.
            if (charset.value() == "x-user-defined")
                return "windows-1252";

            // 16. Return charset.
            return charset.value();
        }

        // * A sequence of bytes starting with a 0x3C byte (<), optionally a 0x2F byte (/), and finally a byte in the
        //   range 0x41-0x5A or 0x61-0x7A (A-Z or a-z)
        else if (!prescan_should_abort(input, position + 3) && input[position] == '<'
            && ((input[position + 1] == '/' && is_ascii_alpha(input[position + 2])) || is_ascii_alpha(input[position + 1]))) {
            // 1. Advance the position pointer so that it points at the next 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR),
            //    0x20 (SP), or 0x3E (>) byte.
            while (!prescan_should_abort(input, position) && !is_whitespace_or_end_chevron(input[position]))
                ++position;

            // 2. Repeatedly get an attribute until no further attributes can be found, then jump to the step below
            //    labeled next byte.
            while (prescan_get_attribute(document, input, position)) { }
            continue;
        }

        // * A sequence of bytes starting with: 0x3C 0x21 (`<!`)
        // * A sequence of bytes starting with: 0x3C 0x2F (`</`)
        // * A sequence of bytes starting with: 0x3C 0x3F (`<?`)
        else if (!prescan_should_abort(input, position + 1) && input[position] == '<'
            && (input[position + 1] == '!' || input[position + 1] == '/' || input[position + 1] == '?')) {
            // Advance the position pointer so that it points at the first 0x3E byte (>) that comes after the 0x3C byte
            // that was found.
            position += 2;
            while (!prescan_should_abort(input, position) && input[position] != '>')
                ++position;
        }

        // * Any other byte
        else {
            // Do nothing with that byte.
        }

        // 4. Next byte: Move position so it points at the next byte in the input byte stream, and return to the step
        //    above labeled loop.
        ++position;
    }
    return {};
}

// https://encoding.spec.whatwg.org/#bom-sniff
Optional<ByteString> run_bom_sniff(ReadonlyBytes input)
{
    if (input.size() >= 3) {
        // 1. Let BOM be the result of peeking 3 bytes from ioQueue, converted to a byte sequence.
        // 2. For each of the rows in the table below, starting with the first one and going down, if BOM starts with the bytes given in the first column, then return the encoding given in the cell in the second column of that row. Otherwise, return null.
        // Byte order mark  Encoding
        // 0xEF 0xBB 0xBF   UTF-8
        // 0xFE 0xFF        UTF-16BE
        // 0xFF 0xFE        UTF-16LE
        if (input[0] == 0xEF && input[1] == 0xBB && input[2] == 0xBF) {
            return "UTF-8";
        }
        if (input[0] == 0xFE && input[1] == 0xFF) {
            return "UTF-16BE";
        }
        if (input[0] == 0xFF && input[1] == 0xFE) {
            return "UTF-16LE";
        }
    }
    return {};
}

// https://html.spec.whatwg.org/multipage/parsing.html#determining-the-character-encoding
ByteString run_encoding_sniffing_algorithm(DOM::Document& document, ReadonlyBytes input, Optional<MimeSniff::MimeType> maybe_mime_type)
{
    // 1. If the result of BOM sniffing is an encoding, return that encoding with confidence certain.
    // FIXME: There is no concept of decoding certainty yet.
    auto bom = run_bom_sniff(input);
    if (bom.has_value())
        return bom.value();
    // 2. FIXME: If the user has explicitly instructed the user agent to override the document's character encoding with a specific encoding,
    //    optionally return that encoding with the confidence certain.

    // 3. FIXME: The user agent may wait for more bytes of the resource to be available, either in this step or at any later step in this algorithm.
    //    For instance, a user agent might wait 500ms or 1024 bytes, whichever came first. In general preparsing the source to find the encoding improves performance,
    //    as it reduces the need to throw away the data structures used when parsing upon finding the encoding information. However, if the user agent delays too long
    //    to obtain data to determine the encoding, then the cost of the delay could outweigh any performance improvements from the preparse.

    // 4. If the transport layer specifies a character encoding, and it is supported, return that encoding with the confidence certain.
    if (maybe_mime_type.has_value()) {
        // FIXME: This is awkward because lecacy_extract_an_encoding can not fail
        auto maybe_transport_encoding = Fetch::Infrastructure::legacy_extract_an_encoding(maybe_mime_type, "invalid"sv);
        if (maybe_transport_encoding != "invalid"sv)
            return maybe_transport_encoding;
    }

    // 5. Optionally, prescan the byte stream to determine its encoding, with the end condition being when the user agent decides that scanning further bytes would not
    //    be efficient. User agents are encouraged to only prescan the first 1024 bytes. User agents may decide that scanning any bytes is not efficient, in which case
    //    these substeps are entirely skipped.
    //    The aforementioned algorithm returns either a character encoding or failure. If it returns a character encoding, then return the same encoding, with confidence tentative.
    auto prescan = run_prescan_byte_stream_algorithm(document, input);
    if (prescan.has_value())
        return prescan.value();

    // 6. FIXME: If the HTML parser for which this algorithm is being run is associated with a Document d whose container document is non-null, then:
    // 1. Let parentDocument be d's container document.
    // 2. If parentDocument's origin is same origin with d's origin and parentDocument's character encoding is not UTF-16BE/LE, then return parentDocument's character
    //    encoding, with the confidence tentative.

    // 7. Otherwise, if the user agent has information on the likely encoding for this page, e.g. based on the encoding of the page when it was last visited, then return
    //    that encoding, with the confidence tentative.

    // 8. FIXME: The user agent may attempt to autodetect the character encoding from applying frequency analysis or other algorithms to the data stream. Such algorithms
    //    may use information about the resource other than the resource's contents, including the address of the resource. If autodetection succeeds in determining a
    //    character encoding, and that encoding is a supported encoding, then return that encoding, with the confidence tentative. [UNIVCHARDET]
    if (!Utf8View(StringView(input)).validate()) {
        // FIXME: As soon as Locale is supported, this should sometimes return a different encoding based on the locale.
        return "windows-1252";
    }

    // 9. Otherwise, return an implementation-defined or user-specified default character encoding, with the confidence tentative.
    //    In controlled environments or in environments where the encoding of documents can be prescribed (for example, for user agents intended for dedicated use in new
    //    networks), the comprehensive UTF-8 encoding is suggested.
    return "UTF-8";
}

}
