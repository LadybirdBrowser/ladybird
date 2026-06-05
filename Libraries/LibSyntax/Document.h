/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Utf32View.h>
#include <AK/Vector.h>
#include <LibGfx/TextAttributes.h>
#include <LibSyntax/Forward.h>
#include <LibSyntax/TextRange.h>

namespace Syntax {

struct TextDocumentSpan {
    TextRange range;
    Gfx::TextAttributes attributes;
    u64 data { 0 };
    bool is_skippable { false };
};

class TextDocumentLine {
public:
    explicit TextDocumentLine(Document&);
    explicit TextDocumentLine(Document&, StringView);

    Utf32View view() const LIFETIME_BOUND { return { code_points(), length() }; }
    u32 const* code_points() const { return m_text.data(); }
    bool is_empty() const { return length() == 0; }
    size_t length() const { return m_text.size(); }
    bool set_text(Document&, StringView);
    void clear(Document&);

private:
    // NOTE: This vector is null terminated.
    Vector<u32> m_text;
};

class Document : public RefCounted<Document> {
public:
    Document() = default;
    virtual ~Document() = default;

    void set_spans(Vector<TextDocumentSpan> spans);
    Vector<TextDocumentSpan>& spans() { return m_spans; }

    virtual TextDocumentLine const& line(size_t line_index) const = 0;
    virtual TextDocumentLine& line(size_t line_index) = 0;

    virtual void update_views(Badge<TextDocumentLine>) = 0;

protected:
    Vector<TextDocumentSpan> m_spans;
};

}
