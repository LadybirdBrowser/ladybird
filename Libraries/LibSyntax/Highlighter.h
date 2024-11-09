/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/WeakPtr.h>
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
    virtual Optional<StringView> comment_prefix() const = 0;
    virtual Optional<StringView> comment_suffix() const = 0;
    virtual void rehighlight(Palette const&) = 0;
    virtual void highlight_matching_token_pair();

    virtual bool is_identifier(u64) const { return false; }
    virtual bool is_navigatable(u64) const { return false; }

    void attach(HighlighterClient&);
    void detach();
    void cursor_did_change();

    struct MatchingTokenPair {
        u64 open;
        u64 close;
    };
    Vector<MatchingTokenPair> matching_token_pairs() const;

    template<typename T>
    bool fast_is() const = delete;

    // FIXME: When other syntax highlighters start using a language server, we should add a common base class here.
    virtual bool is_cpp_semantic_highlighter() const { return false; }

protected:
    Highlighter() = default;

    // FIXME: This should be WeakPtr somehow
    HighlighterClient* m_client { nullptr };

    virtual Vector<MatchingTokenPair> matching_token_pairs_impl() const = 0;
    virtual bool token_types_equal(u64, u64) const = 0;
    void register_nested_token_pairs(Vector<MatchingTokenPair>);
    void clear_nested_token_pairs() { m_nested_token_pairs.clear(); }
    size_t first_free_token_kind_serial_value() const { return m_nested_token_pairs.size(); }

    struct BuddySpan {
        int index { -1 };
        TextDocumentSpan span_backup;
    };

    bool m_has_brace_buddies { false };
    BuddySpan m_brace_buddies[2];
    HashTable<MatchingTokenPair> m_nested_token_pairs;
};

class ProxyHighlighterClient final : public Syntax::HighlighterClient {
public:
    ProxyHighlighterClient(Syntax::HighlighterClient& client, TextPosition start, u64 nested_kind_start_value, StringView source)
        : m_document(client.get_document())
        , m_text(source)
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

    Vector<TextDocumentFoldingRegion> corrected_folding_regions() const
    {
        Vector<TextDocumentFoldingRegion> folding_regions { m_folding_regions };
        for (auto& entry : folding_regions) {
            entry.range.start() = {
                entry.range.start().line() + m_start.line(),
                entry.range.start().line() == 0 ? entry.range.start().column() + m_start.column() : entry.range.start().column(),
            };
            entry.range.end() = {
                entry.range.end().line() + m_start.line(),
                entry.range.end().line() == 0 ? entry.range.end().column() + m_start.column() : entry.range.end().column(),
            };
        }

        return folding_regions;
    }

    Vector<Syntax::Highlighter::MatchingTokenPair> corrected_token_pairs(Vector<Syntax::Highlighter::MatchingTokenPair> pairs) const
    {
        for (auto& pair : pairs) {
            pair.close += m_nested_kind_start_value;
            pair.open += m_nested_kind_start_value;
        }
        return pairs;
    }

private:
    virtual Vector<TextDocumentSpan> const& spans() const override { return m_spans; }
    virtual void set_span_at_index(size_t index, TextDocumentSpan span) override { m_spans.at(index) = move(span); }

    virtual Vector<TextDocumentFoldingRegion>& folding_regions() override { return m_folding_regions; }
    virtual Vector<TextDocumentFoldingRegion> const& folding_regions() const override { return m_folding_regions; }

    virtual ByteString highlighter_did_request_text() const override { return m_text; }
    virtual void highlighter_did_request_update() override { }
    virtual Document& highlighter_did_request_document() override { return m_document; }
    virtual TextPosition highlighter_did_request_cursor() const override { return {}; }
    virtual void highlighter_did_set_spans(Vector<TextDocumentSpan> spans) override { m_spans = move(spans); }
    virtual void highlighter_did_set_folding_regions(Vector<TextDocumentFoldingRegion> folding_regions) override { m_folding_regions = folding_regions; }

    Vector<TextDocumentSpan> m_spans;
    Vector<TextDocumentFoldingRegion> m_folding_regions;
    Document& m_document;
    StringView m_text;
    TextPosition m_start;
    u64 m_nested_kind_start_value { 0 };
};

}

template<>
struct AK::Traits<Syntax::Highlighter::MatchingTokenPair> : public AK::DefaultTraits<Syntax::Highlighter::MatchingTokenPair> {
    static unsigned hash(Syntax::Highlighter::MatchingTokenPair const& pair)
    {
        return pair_int_hash(u64_hash(pair.open), u64_hash(pair.close));
    }
    static bool equals(Syntax::Highlighter::MatchingTokenPair const& a, Syntax::Highlighter::MatchingTokenPair const& b)
    {
        return a.open == b.open && a.close == b.close;
    }
};
