/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSNamespaceRule.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/Dump.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSNamespaceRule);

CSSNamespaceRule::CSSNamespaceRule(RustRule rule)
    : CSSRule(move(rule))
    , m_rule(*native_rule().payload().namespace_rule)
{
}

GC::Ref<CSSNamespaceRule> CSSNamespaceRule::create(RustRule rule)
{
    return GC::Heap::the().allocate<CSSNamespaceRule>(move(rule));
}

Utf16View CSSNamespaceRule::prefix() const
{
    auto view = Parser::ValueParserFFI::rust_namespace_rule_prefix(&m_rule);
    return { reinterpret_cast<char16_t const*>(view.utf16), view.length };
}

Utf16View CSSNamespaceRule::namespace_uri() const
{
    auto view = Parser::ValueParserFFI::rust_namespace_rule_uri(&m_rule);
    return { reinterpret_cast<char16_t const*>(view.utf16), view.length };
}

// https://www.w3.org/TR/cssom/#serialize-a-css-rule
Utf16String CSSNamespaceRule::serialized() const
{
    Utf16StringBuilder builder;
    // The literal string "@namespace", followed by a single SPACE (U+0020),
    builder.append_ascii("@namespace "sv);

    // followed by the serialization as an identifier of the prefix attribute (if any),
    if (!prefix().is_empty()) {
        serialize_an_identifier(builder, prefix());
        // followed by a single SPACE (U+0020) if there is a prefix,
        builder.append_ascii(' ');
    }

    //  followed by the serialization as URL of the namespaceURI attribute,
    serialize_a_url(builder, namespace_uri());

    // followed the character ";" (U+003B).
    builder.append_ascii(';');

    return builder.to_string();
}

void CSSNamespaceRule::dump(StringBuilder& builder, int indent_levels) const
{
    Base::dump(builder, indent_levels);

    dump_indent(builder, indent_levels + 1);
    builder.appendff("Namespace: {}\n", namespace_uri());
    if (!prefix().is_empty()) {
        dump_indent(builder, indent_levels + 1);
        builder.appendff("Prefix: {}\n", prefix());
    }
}

}
