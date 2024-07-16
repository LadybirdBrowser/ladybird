/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/GenericLexer.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>

namespace AK {

class SourceGenerator {
    AK_MAKE_NONCOPYABLE(SourceGenerator);

public:
    using MappingType = HashMap<StringView, String>;

    explicit SourceGenerator(StringBuilder& builder, char opening = '@', char closing = '@', char escape = '\\')
        : m_builder(builder)
        , m_opening(opening)
        , m_closing(closing)
        , m_escape(escape)
    {
    }
    explicit SourceGenerator(StringBuilder& builder, MappingType&& mapping, char opening = '@', char closing = '@', char escape = '\\')
        : m_builder(builder)
        , m_mapping(move(mapping))
        , m_opening(opening)
        , m_closing(closing)
        , m_escape(escape)
    {
    }

    SourceGenerator(SourceGenerator&&) = default;
    // Move-assign is undefinable due to 'StringBuilder& m_builder;'
    SourceGenerator& operator=(SourceGenerator&&) = delete;

    [[nodiscard]] SourceGenerator fork()
    {
        return SourceGenerator { m_builder, MUST(m_mapping.clone()), m_opening, m_closing };
    }

    void set(StringView key, String value)
    {
        if (key.contains(m_opening) || key.contains(m_closing)) {
            warnln("SourceGenerator keys cannot contain the opening/closing delimiters `{}` and `{}`. (Keys are only wrapped in these when using them, not when setting them.)", m_opening, m_closing);
            VERIFY_NOT_REACHED();
        }
        m_mapping.set(key, move(value));
    }

    String get(StringView key) const
    {
        auto result = m_mapping.get(key);
        if (!result.has_value()) {
            warnln("No key named `{}` set on SourceGenerator", key);
            VERIFY_NOT_REACHED();
        }
        return result.release_value();
    }

    StringView as_string_view() const { return m_builder.string_view(); }

    void append(StringView pattern)
    {
        GenericLexer lexer { pattern };

        while (!lexer.is_eof()) {
            m_builder.append(lexer.consume_until([&](char ch) { return ch == m_opening || ch == m_escape; }));
            if (lexer.consume_specific(m_escape)) {
                if (!(lexer.next_is(m_opening) || lexer.next_is(m_escape))) {
                    if (lexer.is_eof())
                        warnln("Unexpected EOF while parsing escape sequence on SourceGenerator");
                    else
                        warnln("Invalid escape sequence found `{}{}` on SourceGenerator", m_escape, lexer.peek());

                    VERIFY_NOT_REACHED();
                }

                m_builder.append(lexer.consume());
                continue;
            }

            if (lexer.consume_specific(m_opening)) {
                auto const placeholder = lexer.consume_until(m_closing);

                if (!lexer.consume_specific(m_closing))
                    VERIFY_NOT_REACHED();

                m_builder.append(get(placeholder));
            } else {
                VERIFY(lexer.is_eof());
            }
        }
    }

    void appendln(StringView pattern)
    {
        append(pattern);
        m_builder.append('\n');
    }

    template<size_t N>
    String get(char const (&key)[N])
    {
        return get(StringView { key, N - 1 });
    }

    template<size_t N>
    void set(char const (&key)[N], String value)
    {
        set(StringView { key, N - 1 }, value);
    }

    template<size_t N>
    void append(char const (&pattern)[N])
    {
        append(StringView { pattern, N - 1 });
    }

    template<size_t N>
    void appendln(char const (&pattern)[N])
    {
        appendln(StringView { pattern, N - 1 });
    }

    // FIXME: These are deprecated.
    void set(StringView key, ByteString value)
    {
        set(key, MUST(String::from_byte_string(value)));
    }
    template<size_t N>
    void set(char const (&key)[N], ByteString value)
    {
        set(StringView { key, N - 1 }, value);
    }

private:
    StringBuilder& m_builder;
    MappingType m_mapping;
    char m_opening { '@' };
    char m_closing { '@' };
    char m_escape { '\\' };
};

}

#if USING_AK_GLOBALLY
using AK::SourceGenerator;
#endif
