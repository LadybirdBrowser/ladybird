/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Page/SpellingError.h>
#include <LibWebCommon/Page/WritingSuggestion.h>
#include <LibWebView/Export.h>

namespace WebView {

// The platform spellchecker the UI process asks on behalf of every page. A document tag scopes what ignore() hides to
// one tab.
class WEBVIEW_API SpellChecker {
    AK_ALLOC_WITH_KMALLOC;

public:
    using DocumentTag = i64;

    virtual ~SpellChecker() = default;

    virtual DocumentTag create_document_tag() = 0;
    virtual void close_document_tag(DocumentTag) = 0;

    // Asynchronous; on_complete runs on the main thread.
    virtual void check(DocumentTag, Utf16String const& text, Function<void(Vector<Web::SpellingError>)> on_complete) = 0;

    virtual Vector<Utf16String> guesses(DocumentTag, Utf16String const& word) = 0;
    virtual void ignore(DocumentTag, Utf16String const& word) = 0;
    virtual void learn(DocumentTag, Utf16String const& word) = 0;

    // Whether a learned word holds for every document tag, as it does in a user dictionary that every app shares.
    virtual bool learns_for_every_tag() const { return true; }

    // Whether suggest() can ever answer, i.e. the platform predicts text as the user types.
    virtual bool can_suggest() const { return false; }

    // Predicts how the text could continue at caret_offset (in UTF-16 code units). Asynchronous; on_complete runs on
    // the main thread, with nothing if there's no prediction.
    virtual void suggest(DocumentTag, [[maybe_unused]] Utf16String const& text, [[maybe_unused]] u32 caret_offset, Function<void(Optional<Web::WritingSuggestion>)> on_complete)
    {
        on_complete({});
    }
};

}
