/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Object.h>
#include <LibUnicode/ListFormat.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

class ListFormat final : public Object {
    JS_OBJECT(ListFormat, Object);
    JS_DECLARE_ALLOCATOR(ListFormat);

public:
    enum class Type {
        Invalid,
        Conjunction,
        Disjunction,
        Unit,
    };

    virtual ~ListFormat() override = default;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

    Unicode::ListFormatType type() const { return m_type; }
    void set_type(StringView type) { m_type = Unicode::list_format_type_from_string(type); }
    StringView type_string() const { return Unicode::list_format_type_to_string(m_type); }

    Unicode::Style style() const { return m_style; }
    void set_style(StringView style) { m_style = Unicode::style_from_string(style); }
    StringView style_string() const { return Unicode::style_to_string(m_style); }

    Unicode::ListFormat const& formatter() const { return *m_formatter; }
    void set_formatter(NonnullOwnPtr<Unicode::ListFormat> formatter) { m_formatter = move(formatter); }

private:
    explicit ListFormat(Object& prototype);

    String m_locale;                                                         // [[Locale]]
    Unicode::ListFormatType m_type { Unicode::ListFormatType::Conjunction }; // [[Type]]
    Unicode::Style m_style { Unicode::Style::Long };                         // [[Style]]

    // Non-standard. Stores the ICU list formatter for the Intl object's formatting options.
    OwnPtr<Unicode::ListFormat> m_formatter;
};

Vector<Unicode::ListFormat::Partition> create_parts_from_list(ListFormat const&, ReadonlySpan<String> list);
String format_list(ListFormat const&, ReadonlySpan<String> list);
NonnullGCPtr<Array> format_list_to_parts(VM&, ListFormat const&, ReadonlySpan<String> list);
ThrowCompletionOr<Vector<String>> string_list_from_iterable(VM&, Value iterable);

}
