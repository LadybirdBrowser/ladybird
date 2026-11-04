/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Page/Translation.h>
#include <LibWebView/Export.h>

namespace WebView {

// The primary language subtag of a BCP 47 language tag, e.g. "pt" for "pt-BR".
WEBVIEW_API StringView primary_language_subtag(StringView language);
// Whether two language tags name the same language, whatever their regions or scripts.
WEBVIEW_API bool is_same_language(StringView, StringView);

// The platform translator the UI process asks on behalf of every page. Languages are BCP 47 tags.
class WEBVIEW_API Translator {
    AK_ALLOC_WITH_KMALLOC;

public:
    using SessionId = u64;

    virtual ~Translator() = default;

    // Works out which language a page is in. Asynchronous; on_complete runs on the main thread, with nothing if the
    // sample doesn't say.
    virtual void detect_language(Web::TranslationLanguageSample const&, Function<void(Optional<String>)> on_complete) = 0;

    struct TargetLanguage {
        String language;
        // The platform supports the pair, but the user has to download its languages before it can be translated.
        bool needs_download { false };
    };

    // Which of the candidate languages text in the source language can be translated into, in the candidates' order.
    // Asynchronous; on_complete runs on the main thread.
    virtual void target_languages(String const& source_language, Vector<String> const& candidates, Function<void(Vector<TargetLanguage>)> on_complete) = 0;

    // A session translates from one language to another, one unit at a time, in the order the units are given.
    virtual SessionId create_session(String const& source_language, String const& target_language) = 0;
    // Drops the units the session hasn't translated yet; their callbacks never run.
    virtual void close_session(SessionId) = 0;
    // Asynchronous; on_complete runs on the main thread, with nothing if the unit couldn't be translated.
    virtual void translate(SessionId, Web::TranslationUnit, Function<void(Optional<Vector<Web::TranslationRun>>)> on_complete) = 0;

    // Opens the platform's settings where the user downloads the languages a translation needs.
    virtual void open_language_download_settings() = 0;
};

}
