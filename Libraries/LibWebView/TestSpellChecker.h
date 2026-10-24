/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <LibWebView/SpellChecker.h>

namespace WebView {

// A spellchecker with a fixed word list, so tests give the same answers on every platform.
class WEBVIEW_API TestSpellChecker final : public SpellChecker {
public:
    virtual DocumentTag create_document_tag() override;
    virtual void close_document_tag(DocumentTag) override;
    virtual void check(DocumentTag, Utf16String const& text, Function<void(Vector<Web::SpellingError>)> on_complete) override;
    virtual Vector<Utf16String> guesses(DocumentTag, Utf16String const& word) override;
    virtual void ignore(DocumentTag, Utf16String const& word) override;
    virtual void learn(DocumentTag, Utf16String const& word) override;
    virtual bool learns_for_every_tag() const override { return false; }
    virtual bool can_suggest() const override { return true; }
    virtual void suggest(DocumentTag, Utf16String const& text, u32 caret_offset, Function<void(Optional<Web::WritingSuggestion>)> on_complete) override;

private:
    bool is_misspelled(DocumentTag, Utf16View word) const;

    DocumentTag m_next_document_tag { 1 };
    // AD-HOC: A platform spellchecker learns a word for every app. Learned words stay with the tab's tag here, so
    //         views that run tests side by side never see each other's.
    HashMap<DocumentTag, HashTable<Utf16String>> m_accepted_words;
};

}
