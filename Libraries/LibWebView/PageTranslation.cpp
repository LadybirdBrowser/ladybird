/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibURL/InternalURLs.h>
#include <LibURL/URL.h>
#include <LibUnicode/DisplayNames.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/Menu.h>
#include <LibWebView/PageTranslation.h>
#include <LibWebView/Settings.h>
#include <LibWebView/URL.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

// The browser's own strings are English, so languages are named in English.
static String language_display_name(StringView language)
{
    auto primary_language = primary_language_subtag(language);
    if (auto name = Unicode::language_display_name("en"sv, primary_language, Unicode::LanguageDisplay::Standard); name.has_value())
        return name->to_utf8();
    return MUST(String::from_utf8(primary_language));
}

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
    m_translate_action = Action::create("Translate"sv, ActionID::TranslatePage, [] { });
    m_translate_action->set_tooltip("Translate this page"sv);
    m_translate_action->set_visible(false);

    m_translation_menu = Menu::create("Translation"sv);
    for (size_t index = 0; index < MAXIMUM_OFFERED_LANGUAGES; ++index) {
        auto action = Action::create_checkable("Translate"sv, ActionID::TranslateTo, [this, index] {
            if (auto target_language = ready_target_language(index); target_language.has_value())
                translate_to(*target_language);
            // NB: The check mark shows the language the page is in, which activating an action doesn't change by itself.
            update_actions();
        });
        action->set_visible(false);
        m_translation_menu->add_action(action);
        m_translate_to_actions.append(move(action));
    }
    m_show_original_action = Action::create("Show Original"sv, ActionID::ShowOriginal, [this] { show_original(); });
    m_translation_menu->add_action(*m_show_original_action);
    m_translation_menu->add_separator();
    m_download_languages_action = Action::create("Download Languages…"sv, ActionID::DownloadTranslationLanguages, [] {
        if (auto* translator = Application::the().translator())
            translator->open_language_download_settings();
    });
    m_translation_menu->add_action(*m_download_languages_action);
    m_translation_menu->add_action(Action::create("Preferred Languages…"sv, ActionID::EditPreferredLanguages, [] {
        auto url = URL::about_settings();
        url.set_fragment("editLanguages"sv);
        Application::the().activate_or_open_tab_with_url(url);
    }));

    update_actions();
}

PageTranslation::~PageTranslation()
{
    close_session();
}

bool PageTranslation::is_offered() const
{
    if (is_translated())
        return true;
    return any_of(m_target_languages, [](auto const& target) { return !target.needs_download; });
}

void PageTranslation::translate_to(String const& target_language)
{
    auto* translator = Application::the().translator();
    if (!translator || !m_page_language.has_value())
        return;
    if (m_target_language.has_value() && is_same_language(*m_target_language, target_language))
        return;

    // A page translated into another language is shown in the original first, since its text is already translated.
    if (is_translated()) {
        m_previous_translation = PreviousTranslation { *m_page_language, target_language, current_site() };
        show_original();
        return;
    }

    m_session_id = translator->create_session(*m_page_language, target_language);
    m_target_language = target_language;
    m_translated_site = current_site();

    m_view.traversable().for_each_hosting_page([&](WebContentPage& page) {
        if (page.is_open())
            page.async_start_translation(*m_page_language, target_language);
    });

    update_actions();
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
    m_page_language.clear();
    m_target_languages.clear();
    m_target_language.clear();
    update_actions();
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
    update_actions();

    // A page in a language the user reads isn't offered for translation.
    auto languages = languages_the_user_reads();
    if (!m_page_language.has_value() || any_of(languages, [&](auto const& language) { return is_same_language(language, *m_page_language); })) {
        if (on_complete)
            on_complete();
        return;
    }
    if (languages.size() > MAXIMUM_OFFERED_LANGUAGES)
        languages.shrink(MAXIMUM_OFFERED_LANGUAGES);

    translator->target_languages(*m_page_language, languages, [weak_this = make_weak_ptr(), generation, on_complete = move(on_complete)](Vector<Translator::TargetLanguage> target_languages) {
        if (!weak_this || generation != weak_this->m_generation)
            return;
        weak_this->m_target_languages = move(target_languages);
        weak_this->update_actions();
        if (on_complete)
            on_complete();
    });
}

Optional<String> PageTranslation::ready_target_language(size_t index) const
{
    for (auto const& target : m_target_languages) {
        if (target.needs_download)
            continue;
        if (index-- == 0)
            return target.language;
    }
    return {};
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

void PageTranslation::update_actions()
{
    m_translate_action->set_visible(is_offered());
    m_translate_action->set_engaged(is_translated());
    m_translate_action->set_tooltip(is_translated() ? "Showing translation"sv : "Translate this page"sv);

    size_t slot = 0;
    for (auto const& target : m_target_languages) {
        if (target.needs_download || slot == m_translate_to_actions.size())
            continue;
        auto& action = m_translate_to_actions[slot++];
        action->set_text(MUST(String::formatted("Translate to {}", language_display_name(target.language))));
        action->set_checked(m_target_language.has_value() && is_same_language(*m_target_language, target.language));
        action->set_visible(true);
    }
    for (; slot < m_translate_to_actions.size(); ++slot)
        m_translate_to_actions[slot]->set_visible(false);

    m_show_original_action->set_enabled(is_translated());
    m_download_languages_action->set_visible(any_of(m_target_languages, [](auto const& target) { return target.needs_download; }));
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
