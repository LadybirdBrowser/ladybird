/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/HTML/TextDirective.h>

namespace Web::HTML {

// https://wicg.github.io/scroll-to-text-fragment/#extracting-the-fragment-directive
Optional<String> remove_the_fragment_directive(URL::URL& url)
{
    // 1. Let raw fragment be equal to url's fragment.
    auto const& raw_fragment = url.fragment();

    // 2. Let fragment directive be null.
    Optional<String> fragment_directive;

    // 3. If raw fragment is non-null and contains the fragment directive delimiter as a substring:
    if (raw_fragment.has_value()) {
        // 1. Let position be the string position variable pointing to the first code point of the first instance, if
        //    one exists, of the fragment directive delimiter in raw fragment, or past the end of raw fragment otherwise.
        auto position = raw_fragment->find_byte_offset(":~:"sv);
        if (position.has_value()) {
            // 2. Let new fragment be the code point substring by positions of raw fragment from the start of raw
            //    fragment to position.
            auto new_fragment = MUST(raw_fragment->substring_from_byte_offset_with_shared_superstring(0, *position));

            // 3. Advance position by the code point length of the fragment directive delimiter.
            *position += 3;

            // 4. If position does not point past the end of raw fragment:
            if (*position < raw_fragment->byte_count()) {
                // 1. Set fragment directive to the code point substring to the end of the string raw fragment starting
                //    from position.
                fragment_directive = MUST(raw_fragment->substring_from_byte_offset_with_shared_superstring(*position));
            }

            // 5. Set url's fragment to new fragment.
            url.set_fragment(move(new_fragment));
        }
    }

    // 4. Return fragment directive.
    return fragment_directive;
}

// https://wicg.github.io/scroll-to-text-fragment/#text-directives
Optional<Utf16String> percent_decode_a_text_directive_term(Optional<StringView> term)
{
    // 1. If term is null, return null.
    if (!term.has_value())
        return {};

    // 2. Assert: term is an ASCII string.
    VERIFY(term->is_ascii());

    // 3. Let decoded bytes be the result of percent-decoding term.
    auto decoded_bytes = URL::percent_decode(*term);

    // 4. Return the result of running UTF-8 decode without BOM on decoded bytes.
    return Utf16String::from_utf8_with_replacement_character(decoded_bytes, Utf16String::WithBOMHandling::No);
}

// https://wicg.github.io/scroll-to-text-fragment/#text-directives
Optional<TextDirective> parse_a_text_directive(StringView text_directive_value)
{
    // 1. Let prefix, suffix, start, end, each be null.
    Optional<StringView> prefix;
    Optional<StringView> suffix;
    Optional<StringView> start;
    Optional<StringView> end;

    // 2. Assert: text directive value is an ASCII string with no code points in the fragment percent-encode set and no
    //    instances of U+0026 (&).
    VERIFY(text_directive_value.is_ascii());
    VERIFY(!text_directive_value.contains('&'));

    // 3. Let tokens be a list of strings that result from strictly splitting text directive value on U+002C (,).
    auto tokens = text_directive_value.split_view(',', SplitBehavior::KeepEmpty);

    // 4. If tokens has size less than 1 or greater than 4, return null.
    if (tokens.is_empty() || tokens.size() > 4)
        return {};

    // 5. If the first item of tokens ends with U+002D (-):
    if (tokens.first().ends_with('-')) {
        // 1. Set prefix to the substring of tokens[0] from 0 with length tokens[0]'s length - 1.
        prefix = tokens.first().substring_view(0, tokens.first().length() - 1);

        // 2. Remove the first item of tokens.
        tokens.remove(0);

        // 3. If prefix is the empty string or contains any instances of U+002D (-), return null.
        if (prefix->is_empty() || prefix->contains('-'))
            return {};

        // 4. If tokens is empty, return null.
        if (tokens.is_empty())
            return {};
    }

    // 6. If the last item of tokens starts with U+002D (-):
    if (tokens.last().starts_with('-')) {
        // 1. Set suffix to the substring of the last item of tokens from 1 to the end of the string.
        suffix = tokens.last().substring_view(1);

        // 2. Remove the last item of tokens.
        tokens.take_last();

        // 3. If suffix is the empty string or contains any instances of U+002D (-), return null.
        if (suffix->is_empty() || suffix->contains('-'))
            return {};

        // 4. If tokens is empty, return null.
        if (tokens.is_empty())
            return {};
    }

    // 7. If tokens has size greater than 2, return null.
    if (tokens.size() > 2)
        return {};

    // 8. Assert: tokens has size 1 or 2.
    VERIFY(tokens.size() == 1 || tokens.size() == 2);

    // 9. Set start to the first item in tokens.
    start = tokens.first();

    // 10. Remove the first item in tokens.
    tokens.remove(0);

    // 11. If start is the empty string or contains any instances of U+002D (-), return null.
    if (start->is_empty() || start->contains('-'))
        return {};

    // 12. If tokens is not empty:
    if (!tokens.is_empty()) {
        // 1. Set end to the first item in tokens.
        end = tokens.first();

        // 2. If end is the empty string or contains any instances of U+002D (-), return null.
        if (end->is_empty() || end->contains('-'))
            return {};
    }

    // 13. Return a new text directive, with
    //       prefix: The percent-decoding of prefix
    //       start: The percent-decoding of start
    //       end: The percent-decoding of end
    //       suffix: The percent-decoding of suffix
    return TextDirective {
        .prefix = percent_decode_a_text_directive_term(prefix),
        .start = percent_decode_a_text_directive_term(start).release_value(),
        .end = percent_decode_a_text_directive_term(end),
        .suffix = percent_decode_a_text_directive_term(suffix),
    };
}

// https://wicg.github.io/scroll-to-text-fragment/#text-directives
Vector<TextDirective> parse_the_fragment_directive(StringView fragment_directive)
{
    // AD-HOC: A fragment directive extracted from a URL is always ASCII. Session history may be restored from an
    //         untrusted serialized representation, so reject invalid restored values before parse_a_text_directive()
    //         reaches its specification assertion.
    if (!fragment_directive.is_ascii())
        return {};

    // 1. Let directives be the result of strictly splitting fragment directive on U+0026 (&).
    auto directives = fragment_directive.split_view('&', SplitBehavior::KeepEmpty);

    // 2. Let output be an initially empty list of text directives.
    Vector<TextDirective> output;

    // 3. For each string directive in directives:
    for (auto const& directive : directives) {
        // 1. If directive does not start with "text=", then continue.
        if (!directive.starts_with("text="sv))
            continue;

        // 2. Let text directive value be the code point substring from 5 to the end of directive.
        //    Note: this may be the empty string.
        auto text_directive_value = directive.substring_view(5);

        // 3. Let parsed text directive be the result of parsing text directive value.
        auto parsed_text_directive = parse_a_text_directive(text_directive_value);

        // 4. If parsed text directive is non-null, append it to output.
        if (parsed_text_directive.has_value())
            output.append(parsed_text_directive.release_value());
    }

    // 4. Return output.
    return output;
}

}
