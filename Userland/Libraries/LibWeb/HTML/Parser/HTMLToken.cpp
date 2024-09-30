/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>

namespace Web::HTML {

String HTMLToken::to_string() const
{
    StringBuilder builder;

    switch (type()) {
    case HTMLToken::Type::DOCTYPE:
        builder.append("DOCTYPE"sv);
        builder.append(" { name: '"sv);
        builder.append(doctype_data().name);
        builder.append("' }"sv);
        break;
    case HTMLToken::Type::StartTag:
        builder.append("StartTag"sv);
        break;
    case HTMLToken::Type::EndTag:
        builder.append("EndTag"sv);
        break;
    case HTMLToken::Type::Comment:
        builder.append("Comment"sv);
        break;
    case HTMLToken::Type::Character:
        builder.append("Character"sv);
        break;
    case HTMLToken::Type::EndOfFile:
        builder.append("EndOfFile"sv);
        break;
    case HTMLToken::Type::Invalid:
        VERIFY_NOT_REACHED();
    }

    if (type() == HTMLToken::Type::StartTag || type() == HTMLToken::Type::EndTag) {
        builder.append(" { name: '"sv);
        builder.append(tag_name());
        builder.append("', { "sv);
        for_each_attribute([&](auto& attribute) {
            builder.append(attribute.local_name);
            builder.append("=\""sv);
            builder.append(attribute.value);
            builder.append("\" "sv);
            return IterationDecision::Continue;
        });
        builder.append("} }"sv);
    }

    if (is_comment()) {
        builder.append(" { data: '"sv);
        builder.append(comment());
        builder.append("' }"sv);
    }

    if (is_character()) {
        builder.append(" { data: '"sv);
        builder.append_code_point(code_point());
        builder.append("' }"sv);
    }

    if (type() == HTMLToken::Type::Character) {
        builder.appendff("@{}:{}", m_start_position.line, m_start_position.column);
    } else {
        builder.appendff("@{}:{}-{}:{}", m_start_position.line, m_start_position.column, m_end_position.line, m_end_position.column);
    }

    return MUST(builder.to_string());
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

    HashTable<FlyString> seen_attributes;
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
        }
    }
}

}
