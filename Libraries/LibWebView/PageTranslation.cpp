/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/PageTranslation.h>
#include <LibWebView/Settings.h>
#include <LibWebView/URL.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

// The languages the user reads, most preferred first, one per language whatever its regions.
static Vector<String> languages_the_user_reads()
{
    Vector<String> languages;
    for (auto const& language : Application::settings().languages()) {
        if (!any_of(languages, [&](auto const& existing) { return is_same_language(existing, language); }))
            languages.append(language);
    }
    return languages;
}

PageTranslation::PageTranslation(ViewImplementation& view)
    : m_view(view)
{
}

PageTranslation::~PageTranslation()
{
    close_session();
}

bool PageTranslation::is_offered() const
{
    return is_translated() || !m_target_languages.is_empty();
}

void PageTranslation::translate_to(String const& target_language)
{
    auto* translator = Application::the().translator();
    if (!translator || !m_page_language.has_value())
        return;
    // The translation picked now replaces the one a download was for, so the download's completion doesn't undo it.
    m_translate_after_download.clear();
    if (m_target_language.has_value() && is_same_language(*m_target_language, target_language))
        return;

    // A page translated into another language is shown in the original first, since its text is already translated.
    if (is_translated()) {
        m_previous_translation = PreviousTranslation { *m_page_language, target_language, current_site() };
        show_original();
        return;
    }

    m_session_id = translator->create_session(*m_page_language, target_language, Application::settings().translation_prefers_higher_quality());
    m_target_language = target_language;
    m_translated_site = current_site();

    m_view.traversable().for_each_hosting_page([&](WebContentPage& page) {
        if (page.is_open())
            page.async_start_translation(*m_page_language, target_language);
    });

    did_change_state();
}

void PageTranslation::show_original()
{
    if (!is_translated())
        return;
    // The reload brings the original back; the translation the user turned off isn't carried over to it. The page stays
    // translated until the reload replaces its document, which may not happen, e.g. if the page asks the user to stay.
    if (!m_previous_translation.has_value() || is_same_language(m_previous_translation->target_language, *m_target_language))
        m_skip_continuation_for_next_document = true;
    stop_translating_pages();
    m_view.reload();
}

bool PageTranslation::translation_can_switch_models() const
{
    if (!m_target_language.has_value())
        return false;
    auto target = m_target_languages.find_if([&](auto const& target) { return is_same_language(target.language, *m_target_language); });
    return !target.is_end() && target->higher_quality_available && !target->faster_after_download;
}

void PageTranslation::translate_again()
{
    if (!is_translated())
        return;
    m_previous_translation = PreviousTranslation { *m_page_language, *m_target_language, current_site() };
    stop_translating_pages();
    m_view.reload();
}

// The platform's settings download the languages, and the page is translated once they're ready, since that's what
// the user downloads them for.
void PageTranslation::download_languages_for(String const& target_language)
{
    auto* translator = Application::the().translator();
    if (!translator || !any_of(m_target_languages, [&](auto const& target) { return is_same_language(target.language, target_language); }))
        return;
    m_translate_after_download = target_language;
    translator->open_language_download_settings();
}

void PageTranslation::prefer_higher_quality_models(bool prefers_higher_quality)
{
    Application::settings().set_translation_prefers_higher_quality(prefers_higher_quality);
    ViewImplementation::for_each_view([](ViewImplementation& view) {
        view.page_translation().did_change_model_preference();
        return IterationDecision::Continue;
    });
    // The text translated so far stays as it is in the other tabs, but this tab's page is shown in the other models'
    // translation, since the user chose them for it. A translation with only one model installed stays as it is, since
    // a reload would lose the page's state and change nothing.
    if (translation_can_switch_models())
        translate_again();
}

void PageTranslation::did_finish_loading()
{
    if (is_translated() || !Application::the().translator())
        return;

    detect_page_language([this] {
        // NB: This runs from detect_page_language()'s callbacks, which check that the translation is still alive.
        auto previous_translation = exchange(m_previous_translation, {});
        if (!previous_translation.has_value() || !m_page_language.has_value())
            return;
        // A translated page's next page is translated too, if it's in the same language on the same site.
        if (!is_same_language(previous_translation->source_language, *m_page_language) || previous_translation->site != current_site())
            return;
        auto target = m_target_languages.find_if([&](auto const& target) { return !target.needs_download && is_same_language(target.language, previous_translation->target_language); });
        if (!target.is_end())
            translate_to(target->language);
    });
}

void PageTranslation::did_change_active_document()
{
    ++m_generation;
    m_pending_samples.clear();

    // The next page continues the translation, unless the user showed the original. A translation into another language
    // that the user asked for while the page was translated is recorded already, and is kept.
    if (exchange(m_skip_continuation_for_next_document, false))
        m_previous_translation.clear();
    else if (is_translated() && !m_previous_translation.has_value())
        m_previous_translation = PreviousTranslation { *m_page_language, *m_target_language, m_translated_site };

    if (is_translated())
        stop_translating_pages();
    close_session();
    m_translate_after_download.clear();
    m_page_language.clear();
    m_target_languages.clear();
    m_target_language.clear();
    did_change_state();
}

void PageTranslation::did_change_installed_languages()
{
    if (!m_page_language.has_value())
        return;

    update_target_languages([this] {
        // This runs from update_target_languages()'s callback, which checks that the translation is still alive.
        auto* translator = Application::the().translator();
        // The units not yet translated get the faster models as soon as they're installed.
        if (m_session_id.has_value())
            translator->choose_session_model_again(*m_session_id, Application::settings().translation_prefers_higher_quality());

        if (!m_translate_after_download.has_value())
            return;
        auto target = m_target_languages.find_if([&](auto const& target) { return !target.needs_download && !target.faster_after_download && is_same_language(target.language, *m_translate_after_download); });
        if (target.is_end())
            return;
        m_translate_after_download.clear();
        translate_to(target->language);
    });
}

void PageTranslation::did_change_model_preference()
{
    if (auto* translator = Application::the().translator(); translator && m_session_id.has_value())
        translator->choose_session_model_again(*m_session_id, Application::settings().translation_prefers_higher_quality());
    did_change_state();
}

void PageTranslation::did_create_page(WebContentPage& page)
{
    if (is_translated())
        page.async_start_translation(*m_page_language, *m_target_language);
}

void PageTranslation::did_get_language_sample(Badge<WebContentPage>, u64 request_id, Web::TranslationLanguageSample sample)
{
    if (sample.text.length_in_code_units() > MAXIMUM_LANGUAGE_SAMPLE_LENGTH)
        sample.text = Utf16String::from_utf16(sample.text.substring_view(0, MAXIMUM_LANGUAGE_SAMPLE_LENGTH));
    if (auto callback = m_pending_samples.take(request_id); callback.has_value())
        (*callback)(move(sample));
}

void PageTranslation::did_request_translation(Badge<WebContentPage>, WebContentPage& requesting_page, Web::TranslationUnit unit)
{
    auto* translator = Application::the().translator();
    if (!translator || !m_session_id.has_value()) {
        requesting_page.async_did_translate({ unit.id, {} });
        return;
    }

    size_t unit_length = 0;
    for (auto const& run : unit.runs)
        unit_length += run.text.length_in_code_units();
    auto& units_in_flight = m_units_in_flight.ensure(&requesting_page);
    if (units_in_flight >= MAXIMUM_UNITS_IN_FLIGHT_PER_PAGE || unit_length > MAXIMUM_UNIT_LENGTH) {
        requesting_page.async_did_translate({ unit.id, {} });
        return;
    }
    ++units_in_flight;

    auto unit_id = unit.id;
    // NB: Closing the session drops this callback, and the session is closed before the translation is destroyed.
    translator->translate(*m_session_id, move(unit), [this, page = NonnullRefPtr { requesting_page }, unit_id](Optional<Vector<Web::TranslationRun>> runs) {
        if (auto it = m_units_in_flight.find(page.ptr()); it != m_units_in_flight.end() && --it->value == 0)
            m_units_in_flight.remove(it);
        if (page->is_open())
            page->async_did_translate({ unit_id, move(runs) });
    });
}

void PageTranslation::translate_for_testing(Badge<WebContentPage>, String target_language)
{
    detect_page_language([this, target_language = move(target_language)] {
        translate_to(target_language);
    });
}

void PageTranslation::detect_page_language(Function<void()> on_complete)
{
    auto request_id = m_next_sample_request_id++;
    auto generation = m_generation;
    m_pending_samples.set(request_id, [this, generation, on_complete = move(on_complete)](Web::TranslationLanguageSample sample) mutable {
        auto* translator = Application::the().translator();
        if (!translator || generation != m_generation)
            return;
        // The tab may be closed before the translator answers.
        translator->detect_language(sample, [weak_this = make_weak_ptr(), generation, on_complete = move(on_complete)](Optional<String> language) mutable {
            if (weak_this)
                weak_this->did_detect_page_language(generation, move(language), move(on_complete));
        });
    });
    m_view.page().async_request_translation_language_sample(request_id);
}

void PageTranslation::did_detect_page_language(u64 generation, Optional<String> language, Function<void()> on_complete)
{
    auto* translator = Application::the().translator();
    if (!translator || generation != m_generation)
        return;

    m_page_language = move(language);
    m_target_languages.clear();
    did_change_state();

    update_target_languages(move(on_complete));
}

void PageTranslation::update_target_languages(Function<void()> on_complete)
{
    auto* translator = Application::the().translator();
    if (!translator)
        return;

    // A page in a language the user reads isn't offered for translation.
    auto languages = languages_the_user_reads();
    if (!m_page_language.has_value() || any_of(languages, [&](auto const& language) { return is_same_language(language, *m_page_language); })) {
        if (on_complete)
            on_complete();
        return;
    }
    if (languages.size() > MAXIMUM_OFFERED_LANGUAGES)
        languages.shrink(MAXIMUM_OFFERED_LANGUAGES);

    auto request = ++m_target_languages_request;
    translator->target_languages(*m_page_language, languages, [weak_this = make_weak_ptr(), generation = m_generation, request, on_complete = move(on_complete)](Vector<Translator::TargetLanguage> target_languages) {
        if (!weak_this || generation != weak_this->m_generation)
            return;
        // Lookups can finish out of order. A result older than the one applied already is dropped, so it can't bring
        // back the offer of a download that's done; its callback still runs, with the newer languages.
        if (request > weak_this->m_applied_target_languages_request) {
            weak_this->m_applied_target_languages_request = request;
            weak_this->m_target_languages = move(target_languages);
            weak_this->did_change_state();
        }
        if (on_complete)
            on_complete();
    });
}

// The pages forget the translation, so a document they load next isn't translated unless it's asked for.
void PageTranslation::stop_translating_pages()
{
    m_view.traversable().for_each_hosting_page([](WebContentPage& page) {
        if (page.is_open())
            page.async_stop_translation();
    });
}

void PageTranslation::close_session()
{
    m_units_in_flight.clear();
    if (!m_session_id.has_value())
        return;
    if (auto* translator = Application::the().translator())
        translator->close_session(*m_session_id);
    m_session_id.clear();
}

void PageTranslation::did_change_state()
{
    m_view.did_change_state();
}

Optional<String> PageTranslation::current_site() const
{
    auto url = m_view.url();
    if (!url.host().has_value())
        return {};
    if (auto domain = url.host()->registrable_domain(); domain.has_value())
        return domain;
    return url.host()->serialize();
}

}
