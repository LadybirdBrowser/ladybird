/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibSyntax/Document.h>
#include <LibSyntax/HighlighterClient.h>
#include <LibSyntax/Language.h>
#include <LibURL/Forward.h>

namespace WebView {

enum class HighlightOutputMode {
    FullDocument, // Include HTML header, title, style sheet, etc
    SourceOnly,   // Just the highlighted source
};

class SourceDocument final : public Syntax::Document {
public:
    static NonnullRefPtr<SourceDocument> create(StringView source)
    {
        return adopt_ref(*new (nothrow) SourceDocument(source));
    }
    virtual ~SourceDocument() = default;

    StringView text() const { return m_source; }
    size_t line_count() const { return m_lines.size(); }

    // ^ Syntax::Document
    virtual Syntax::TextDocumentLine const& line(size_t line_index) const override;
    virtual Syntax::TextDocumentLine& line(size_t line_index) override;

private:
    SourceDocument(StringView source);

    // ^ Syntax::Document
    virtual void update_views(Badge<Syntax::TextDocumentLine>) override { }

    StringView m_source;
    Vector<Syntax::TextDocumentLine> m_lines;
};

class SourceHighlighterClient final : public Syntax::HighlighterClient {
public:
    SourceHighlighterClient(StringView source, Syntax::Language);
    virtual ~SourceHighlighterClient() = default;

    String to_html_string(URL::URL const& url, URL::URL const& base_url, HighlightOutputMode) const;

private:
    // ^ Syntax::HighlighterClient
    virtual Vector<Syntax::TextDocumentSpan> const& spans() const override;
    virtual void set_span_at_index(size_t index, Syntax::TextDocumentSpan span) override;
    virtual Vector<Syntax::TextDocumentFoldingRegion>& folding_regions() override;
    virtual Vector<Syntax::TextDocumentFoldingRegion> const& folding_regions() const override;
    virtual ByteString highlighter_did_request_text() const override;
    virtual void highlighter_did_request_update() override;
    virtual Syntax::Document& highlighter_did_request_document() override;
    virtual Syntax::TextPosition highlighter_did_request_cursor() const override;
    virtual void highlighter_did_set_spans(Vector<Syntax::TextDocumentSpan>) override;
    virtual void highlighter_did_set_folding_regions(Vector<Syntax::TextDocumentFoldingRegion>) override;

    StringView class_for_token(u64 token_type) const;

    SourceDocument& document() const { return *m_document; }

    NonnullRefPtr<SourceDocument> m_document;
    OwnPtr<Syntax::Highlighter> m_highlighter;
};

String highlight_source(URL::URL const& url, URL::URL const& base_url, StringView, Syntax::Language, HighlightOutputMode);

constexpr inline StringView HTML_HIGHLIGHTER_STYLE = R"~~~(
    @media (prefers-color-scheme: dark) {
        /* FIXME: We should be able to remove the HTML style when "color-scheme" is supported */
        html {
            background-color: rgb(30, 30, 30);
            color: white;
            counter-reset: line;
        }

        :root {
            --comment-color: lightgreen;
            --keyword-color: orangered;
            --name-color: orange;
            --value-color: deepskyblue;
            --internal-color: darkgrey;
            --string-color: goldenrod;
            --error-color: red;
            --line-number-color: darkgrey;
        }
    }

    @media (prefers-color-scheme: light) {
        :root {
            --comment-color: green;
            --keyword-color: red;
            --name-color: darkorange;
            --value-color: blue;
            --internal-color: dimgrey;
            --string-color: darkgoldenrod;
            --error-color: darkred;
            --line-number-color: dimgrey;
        }
    }

    .html {
        font-size: 10pt;
        font-family: Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
    }

    .line {
        counter-increment: line;
        white-space: pre;
    }

    .line::before {
        content: counter(line) " ";

        display: inline-block;
        width: 2.5em;

        padding-right: 0.5em;
        text-align: right;

        color: var(--line-number-color);
    }

    .tag {
        font-weight: 600;
        color: var(--keyword-color);
    }
    .comment {
        color: var(--comment-color);
    }
    .attribute-name {
        color: var(--name-color);
    }
    .attribute-value {
        color: var(--value-color);
    }
    .internal {
        color: var(--internal-color);
    }
    .invalid {
        color: var(--error-color);
        text-decoration: currentColor wavy underline;
    }
    .at-keyword, .function, .keyword, .control-keyword, .url {
        color: var(--keyword-color);
    }
    .number, .hash {
        color: var(--value-color);
    }
    .string {
        color: var(--string-color);
    }
)~~~"sv;

}
