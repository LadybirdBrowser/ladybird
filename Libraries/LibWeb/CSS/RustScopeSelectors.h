/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Selector.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS {

// Document-thread bindings of the immutable, worker-parsed scope selector pair.
class RustScopeSelectors final : public RefCounted<RustScopeSelectors> {
public:
    static NonnullRefPtr<RustScopeSelectors> create(Parser::ValueParserFFI::ScopeSelectors const* selectors)
    {
        return adopt_ref(*new RustScopeSelectors(selectors));
    }
    ~RustScopeSelectors() { Parser::ValueParserFFI::rust_scope_selectors_release(m_selectors); }

    Parser::ValueParserFFI::ScopeSelectors const* handle() const { return m_selectors; }
    Optional<SelectorList> const& start() const
    {
        if (!m_start.has_value()) {
            if (auto* selectors = Parser::ValueParserFFI::rust_scope_selectors_start(m_selectors))
                m_start = selector_list_from_rust(static_cast<SelectorFFI::RustParsedSelectorList const*>(selectors));
        }
        return m_start;
    }
    Optional<SelectorList> const& end() const
    {
        if (!m_end.has_value()) {
            if (auto* selectors = Parser::ValueParserFFI::rust_scope_selectors_end(m_selectors))
                m_end = selector_list_from_rust(static_cast<SelectorFFI::RustParsedSelectorList const*>(selectors));
        }
        return m_end;
    }

private:
    explicit RustScopeSelectors(Parser::ValueParserFFI::ScopeSelectors const* selectors)
        : m_selectors(Parser::ValueParserFFI::rust_scope_selectors_retain(selectors))
    {
    }

    Parser::ValueParserFFI::ScopeSelectors const* m_selectors;
    mutable Optional<SelectorList> m_start;
    mutable Optional<SelectorList> m_end;
};

}
