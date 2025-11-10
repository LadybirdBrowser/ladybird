/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/TemporaryChange.h>
#include <AK/Utf16String.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/MathMLStringBox.h>
#include <LibWeb/MathML/AttributeNames.h>
#include <LibWeb/MathML/MathMLStringElement.h>

namespace Web::MathML {

GC_DEFINE_ALLOCATOR(MathMLStringElement);

MathMLStringElement::MathMLStringElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : MathMLElement(document, move(qualified_name))
{
}

GC::Ptr<Layout::Node> MathMLStringElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    ensure_quotes();
    return heap().allocate<Layout::MathMLStringBox>(document(), *this, move(style));
}

void MathMLStringElement::children_changed(DOM::Node::ChildrenChangedMetadata const* metadata)
{
    MathMLElement::children_changed(metadata);
    if (m_is_generating_quotes)
        return;
    ensure_quotes();
}

void MathMLStringElement::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    MathMLElement::attribute_changed(local_name, old_value, value, namespace_);
    if (local_name == AttributeNames::lquote || local_name == AttributeNames::rquote)
        ensure_quotes();
}

void MathMLStringElement::inserted()
{
    MathMLElement::inserted();
    ensure_quotes();
}

String MathMLStringElement::resolved_left_quote() const
{
    static constexpr StringView fallback { "\"", 1 };
    return attribute(AttributeNames::lquote).value_or(String::from_utf8_without_validation(fallback.bytes()));
}

String MathMLStringElement::resolved_right_quote() const
{
    static constexpr StringView fallback { "\"", 1 };
    return attribute(AttributeNames::rquote).value_or(String::from_utf8_without_validation(fallback.bytes()));
}

void MathMLStringElement::ensure_quotes()
{
    TemporaryChange guard(m_is_generating_quotes, true);

    if (m_left_quote_text_node) {
        m_left_quote_text_node->remove();
        m_left_quote_text_node = nullptr;
    }
    if (m_right_quote_text_node) {
        m_right_quote_text_node->remove();
        m_right_quote_text_node = nullptr;
    }

    auto left_text = document().create_text_node(Utf16String::from_utf8(resolved_left_quote()));
    if (auto* first_child = this->first_child())
        insert_before(left_text, first_child);
    else
        MUST(append_child(left_text));
    m_left_quote_text_node = left_text.ptr();

    auto right_text = document().create_text_node(Utf16String::from_utf8(resolved_right_quote()));
    MUST(append_child(right_text));
    m_right_quote_text_node = right_text.ptr();
}

}
