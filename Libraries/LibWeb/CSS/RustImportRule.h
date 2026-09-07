/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/CSS/RustQueryHandle.h>

namespace Web::CSS {

class RustImportRule {
public:
    explicit RustImportRule(Parser::ValueParserFFI::ImportRuleData const* rule)
        : m_rule(Parser::ValueParserFFI::rust_import_rule_retain(rule))
    {
        VERIFY(m_rule);
    }
    RustImportRule(RustImportRule const& other)
        : m_rule(Parser::ValueParserFFI::rust_import_rule_retain(other.m_rule))
    {
    }
    RustImportRule(RustImportRule&& other)
        : m_rule(exchange(other.m_rule, nullptr))
    {
    }
    RustImportRule& operator=(RustImportRule const& other)
    {
        RustImportRule copy(other);
        swap(m_rule, copy.m_rule);
        return *this;
    }
    RustImportRule& operator=(RustImportRule&& other)
    {
        RustImportRule moved(move(other));
        swap(m_rule, moved.m_rule);
        return *this;
    }
    ~RustImportRule() { Parser::ValueParserFFI::rust_import_rule_release(m_rule); }
    Parser::ValueParserFFI::ImportRuleData const* handle() const { return m_rule; }
    void const* url_data() const { return data().url; }
    Optional<Utf16View> layer() const
    {
        auto view = data();
        if (!view.has_layer)
            return {};
        return Utf16View { reinterpret_cast<char16_t const*>(view.layer.utf16), view.layer.length };
    }
    Optional<RustQueryHandle> supports() const
    {
        if (auto* query = data().supports)
            return RustQueryHandle::retained(query);
        return {};
    }

private:
    Parser::ValueParserFFI::FfiImportRuleView data() const { return Parser::ValueParserFFI::rust_import_rule_view(m_rule); }
    Parser::ValueParserFFI::ImportRuleData const* m_rule;
};

}
