/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>

namespace Web::HTML {

Utf16String HTMLToken::to_string() const
{
    Utf16StringBuilder builder;

    switch (type()) {
    case HTMLToken::Type::DOCTYPE:
        builder.append_ascii("DOCTYPE"sv);
        builder.append_ascii(" { name: '"sv);
        builder.append(doctype_data().name.view());
        builder.append_ascii("' }"sv);
        break;
    case HTMLToken::Type::StartTag:
        builder.append_ascii("StartTag"sv);
        break;
    case HTMLToken::Type::EndTag:
        builder.append_ascii("EndTag"sv);
        break;
    case HTMLToken::Type::Comment:
        builder.append_ascii("Comment"sv);
        break;
    case HTMLToken::Type::Character:
        builder.append_ascii("Character"sv);
        break;
    case HTMLToken::Type::EndOfFile:
        builder.append_ascii("EndOfFile"sv);
        break;
    case HTMLToken::Type::Invalid:
        VERIFY_NOT_REACHED();
    }

    if (type() == HTMLToken::Type::StartTag || type() == HTMLToken::Type::EndTag) {
        builder.append_ascii(" { name: '"sv);
        builder.append(tag_name().view());
        builder.append_ascii("', { "sv);
        for_each_attribute([&](auto& attribute) {
            builder.append(attribute.local_name.view());
            builder.append_ascii("=\""sv);
            builder.append(attribute.value);
            builder.append_ascii("\" "sv);
            return IterationDecision::Continue;
        });
        builder.append_ascii("} }"sv);
    }

    if (is_comment()) {
        builder.append_ascii(" { data: '"sv);
        builder.append(comment());
        builder.append_ascii("' }"sv);
    }

    if (is_character()) {
        builder.append_ascii(" { data: '"sv);
        builder.append_code_point(code_point());
        builder.append_ascii("' }"sv);
    }

    if (type() == HTMLToken::Type::Character) {
        builder.appendff("@{}:{}", m_start_position.line, m_start_position.column);
    } else {
        builder.appendff("@{}:{}-{}:{}", m_start_position.line, m_start_position.column, m_end_position.line, m_end_position.column);
    }

    return builder.to_string();
}

void HTMLToken::normalize_attributes()
{
    // From AttributeNameState: https://html.spec.whatwg.org/multipage/parsing.html#attribute-name-state
    //
    // When the user agent leaves the attribute name state (and before emitting the tag token, if appropriate),
    // the complete attribute's name must be compared to the other attributes on the same token;
    // if there is already an attribute on the token with the exact same name, then this is a duplicate-attribute
    // parse error and the new attribute must be removed from the token.

    // NOTE: If an attribute is so removed from a token, it, and the value that gets associated with it, if any,
    // are never subsequently used by the parser, and are therefore effectively discarded. Removing the attribute
    // in this way does not change its status as the "current attribute" for the purposes of the tokenizer, however.

    HashTable<Utf16FlyString> seen_attributes;
    auto* ptr = tag_attributes();
    if (!ptr)
        return;
    auto& tag_attributes = *ptr;
    for (size_t i = 0; i < tag_attributes.size(); ++i) {
        auto& attribute = tag_attributes[i];
        if (seen_attributes.set(attribute.local_name, AK::HashSetExistingEntryBehavior::Keep) == AK::HashSetResult::KeptExistingEntry) {
            // This is a duplicate attribute, remove it.
            tag_attributes.remove(i);
            --i;
            m_had_duplicate_attribute = true;
        }
    }
}

}
