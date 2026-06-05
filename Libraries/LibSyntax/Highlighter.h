/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <LibGfx/Palette.h>
#include <LibSyntax/Document.h>
#include <LibSyntax/HighlighterClient.h>
#include <LibSyntax/Language.h>

namespace Syntax {

class Highlighter {
    AK_MAKE_NONCOPYABLE(Highlighter);
    AK_MAKE_NONMOVABLE(Highlighter);

public:
    virtual ~Highlighter() = default;

    virtual Language language() const = 0;
    virtual void rehighlight(Palette const&) = 0;

    void attach(HighlighterClient&);
    void detach();

    template<typename T>
    bool fast_is() const = delete;

protected:
    Highlighter() = default;

    // FIXME: This should be WeakPtr somehow
    HighlighterClient* m_client { nullptr };
};

class ProxyHighlighterClient final : public Syntax::HighlighterClient {
public:
    ProxyHighlighterClient(TextPosition start, u64 nested_kind_start_value, StringView source)
        : m_text(source)
        , m_start(start)
        , m_nested_kind_start_value(nested_kind_start_value)
    {
    }

    Vector<TextDocumentSpan> corrected_spans() const
    {
        Vector<TextDocumentSpan> spans { m_spans };
        for (auto& entry : spans) {
            entry.range.start() = {
                entry.range.start().line() + m_start.line(),
                entry.range.start().line() == 0 ? entry.range.start().column() + m_start.column() : entry.range.start().column(),
            };
            entry.range.end() = {
                entry.range.end().line() + m_start.line(),
                entry.range.end().line() == 0 ? entry.range.end().column() + m_start.column() : entry.range.end().column(),
            };
            if (entry.data != (u64)-1)
                entry.data += m_nested_kind_start_value;
        }

        return spans;
    }

private:
    virtual StringView highlighter_did_request_text() const override { return m_text; }
    virtual void highlighter_did_set_spans(Vector<TextDocumentSpan> spans) override { m_spans = move(spans); }

    Vector<TextDocumentSpan> m_spans;
    StringView m_text;
    TextPosition m_start;
    u64 m_nested_kind_start_value { 0 };
};

}
