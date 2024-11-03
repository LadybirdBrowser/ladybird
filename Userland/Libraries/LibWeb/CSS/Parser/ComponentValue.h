/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibWeb/CSS/Parser/Token.h>
#include <LibWeb/CSS/Parser/Types.h>

namespace Web::CSS::Parser {

// https://drafts.csswg.org/css-syntax/#component-value
class ComponentValue {
public:
    ComponentValue(Token);
    explicit ComponentValue(Function&&);
    explicit ComponentValue(SimpleBlock&&);
    ~ComponentValue();

    bool is_block() const { return m_value.has<SimpleBlock>(); }
    SimpleBlock& block() { return m_value.get<SimpleBlock>(); }
    SimpleBlock const& block() const { return m_value.get<SimpleBlock>(); }

    bool is_function() const { return m_value.has<Function>(); }
    bool is_function(StringView name) const;
    Function& function() { return m_value.get<Function>(); }
    Function const& function() const { return m_value.get<Function>(); }

    bool is_token() const { return m_value.has<Token>(); }
    bool is(Token::Type type) const { return is_token() && token().is(type); }
    bool is_delim(u32 delim) const { return is(Token::Type::Delim) && token().delim() == delim; }
    bool is_ident(StringView ident) const;
    Token const& token() const { return m_value.get<Token>(); }
    operator Token() const { return m_value.get<Token>(); }

    String to_string() const;
    String to_debug_string() const;
    String original_source_text() const;

private:
    Variant<Token, Function, SimpleBlock> m_value;
};
}

template<>
struct AK::Formatter<Web::CSS::Parser::ComponentValue> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Parser::ComponentValue const& component_value)
    {
        return Formatter<StringView>::format(builder, component_value.to_string());
    }
};
