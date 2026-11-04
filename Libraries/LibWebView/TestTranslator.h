/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWebView/Translator.h>

namespace WebView {

// A translator that gives the same answers on every platform, so tests can check what translating a page does to it.
// It "translates" a unit by putting its runs in reverse order, and by uppercasing the ASCII letters of every run it may
// translate and trimming the white space at its ends, as a real translation does. Reversing the runs means every test
// exercises the reordering a real translation can bring. A target language with the private use subtag "x-repeat" also
// gets the first run of each translation a second time, at its end, and one with "x-drop" loses its last run, as real
// translations sometimes do.
class WEBVIEW_API TestTranslator final : public Translator {
public:
    // The page's declared language, or nothing.
    virtual void detect_language(Web::TranslationLanguageSample const&, Function<void(Optional<String>)> on_complete) override;
    // Every candidate in another language; one whose primary language subtag is "xx" needs downloading first.
    virtual void target_languages(String const& source_language, Vector<String> const& candidates, Function<void(Vector<TargetLanguage>)> on_complete) override;
    virtual SessionId create_session(String const& source_language, String const& target_language) override;
    virtual void close_session(SessionId) override;
    virtual void translate(SessionId, Web::TranslationUnit, Function<void(Optional<Vector<Web::TranslationRun>>)> on_complete) override;
    virtual void open_language_download_settings() override { }

private:
    SessionId m_last_session_id { 0 };
    HashMap<SessionId, String> m_target_languages;
};

}
