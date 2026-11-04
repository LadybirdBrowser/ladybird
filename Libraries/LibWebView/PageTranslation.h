/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibWebCommon/Page/Translation.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Translator.h>

namespace WebView {

// Translating one tab's page: working out which language the page is in, offering to translate it into a language the
// user reads, translating it with the platform translator, and showing the original again.
//
// The pages of every process that hosts a document of the tab translate their own documents. They send their text here
// one unit at a time, and get each unit's translation back.
class PageTranslation : public Weakable<PageTranslation> {
    AK_MAKE_NONCOPYABLE(PageTranslation);
    AK_MAKE_NONMOVABLE(PageTranslation);
    AK_ALLOC_WITH_KMALLOC;

public:
    // How many languages the translation menu offers at most.
    static constexpr size_t MAXIMUM_OFFERED_LANGUAGES = 5;
    // A page sends a few units at a time, of a paragraph's text each. One that sends more is answered without them being
    // translated, so a misbehaving WebContent process can't queue up unbounded work for the translator.
    static constexpr size_t MAXIMUM_UNITS_IN_FLIGHT_PER_PAGE = 8;
    static constexpr size_t MAXIMUM_UNIT_LENGTH = 65536;
    // The language sample is enough text to tell its language by, and no more is looked at.
    static constexpr size_t MAXIMUM_LANGUAGE_SAMPLE_LENGTH = 4096;

    explicit PageTranslation(ViewImplementation&);
    ~PageTranslation();

    // The language of the tab's document, once it's known.
    Optional<String> const& page_language() const { return m_page_language; }
    // The language the page is shown in, if it's translated.
    Optional<String> const& target_language() const { return m_target_language; }
    bool is_translated() const { return m_target_language.has_value(); }

    // Whether the page can be translated into a language the user reads, or already is.
    bool is_offered() const;

    Action& translate_action() { return *m_translate_action; }
    Menu& translation_menu() { return *m_translation_menu; }

    void translate_to(String const& target_language);
    // Shows the page as it was before it was translated, by loading it again.
    void show_original();

    void did_finish_loading();
    void did_change_active_document();
    // A page that hosts a document of the tab starts translating it if the tab is translated.
    void did_create_page(WebContentPage&);

    void did_get_language_sample(Badge<WebContentPage>, u64 request_id, Web::TranslationLanguageSample);
    void did_request_translation(Badge<WebContentPage>, WebContentPage&, Web::TranslationUnit);
    // For tests: Works out which language the page is in, then translates it into the target language.
    void translate_for_testing(Badge<WebContentPage>, String target_language);

private:
    struct PreviousTranslation {
        String source_language;
        String target_language;
        Optional<String> site;
    };

    // The target the n-th of the translation menu's languages stands for; only targets ready to translate get one.
    Optional<String> ready_target_language(size_t index) const;
    void stop_translating_pages();
    void detect_page_language(Function<void()> on_complete = {});
    void did_detect_page_language(u64 generation, Optional<String> language, Function<void()> on_complete);
    void close_session();
    void update_actions();
    Optional<String> current_site() const;

    ViewImplementation& m_view;

    Optional<String> m_page_language;
    Vector<Translator::TargetLanguage> m_target_languages;
    Optional<String> m_target_language;
    // The site of the page that was translated, for continuing on the next page of that site.
    Optional<String> m_translated_site;
    Optional<Translator::SessionId> m_session_id;
    HashMap<WebContentPage const*, size_t> m_units_in_flight;

    // A translation in force when the user followed a link, which the next page of the same site continues.
    Optional<PreviousTranslation> m_previous_translation;
    // Set when the original is shown again, so the reload that shows it isn't translated straight away.
    bool m_skip_continuation_for_next_document { false };

    // Answers to requests made for an earlier document are ignored.
    u64 m_generation { 0 };
    u64 m_next_sample_request_id { 0 };
    HashMap<u64, Function<void(Web::TranslationLanguageSample)>> m_pending_samples;

    RefPtr<Action> m_translate_action;
    RefPtr<Menu> m_translation_menu;
    Vector<NonnullRefPtr<Action>> m_translate_to_actions;
    RefPtr<Action> m_show_original_action;
    RefPtr<Action> m_download_languages_action;
};

}
