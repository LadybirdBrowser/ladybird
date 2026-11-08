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
        // Only the platform's higher-quality models are installed for the pair, and they're much slower than the ones a
        // download would add.
        bool faster_after_download { false };
        // The platform's higher-quality models are installed for the pair, so the user can choose them.
        bool higher_quality_available { false };
        // A language the pair needs is being downloaded.
        bool downloading { false };
        // The languages of the pair the platform knows aren't installed, which may be the page's own as well as the
        // target. Empty if the platform doesn't say.
        Vector<String> languages_to_download {};
    };

    // Which of the candidate languages text in the source language can be translated into, in the candidates' order.
    // Asynchronous; on_complete runs on the main thread.
    virtual void target_languages(String const& source_language, Vector<String> const& candidates, Function<void(Vector<TargetLanguage>)> on_complete) = 0;

    // A session translates from one language to another, one unit at a time, in the order the units are given. It uses
    // the fastest of the pair's installed models, or the higher-quality ones if they're preferred and installed.
    virtual SessionId create_session(String const& source_language, String const& target_language, bool prefer_higher_quality) = 0;
    // Units the session hasn't started on yet are translated with the model the session would pick now, e.g. once the
    // download of a faster one completes.
    virtual void choose_session_model_again(SessionId, bool prefer_higher_quality) = 0;
    // Drops the units the session hasn't translated yet; their callbacks never run.
    virtual void close_session(SessionId) = 0;
    // Asynchronous; on_complete runs on the main thread, with nothing if the unit couldn't be translated.
    virtual void translate(SessionId, Web::TranslationUnit, Function<void(Optional<Vector<Web::TranslationRun>>)> on_complete) = 0;

    // Opens the platform's settings where the user downloads the languages a translation needs.
    virtual void open_language_download_settings() = 0;

    // What the user knows the platform's higher-quality models as, if it has any.
    virtual Optional<StringView> higher_quality_models_name() const { return {}; }

    // Runs on the main thread whenever a language's download starts or finishes, so the answers target_languages()
    // gives may have changed.
    Function<void()> on_languages_changed;
};

}
