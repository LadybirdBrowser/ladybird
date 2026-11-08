/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/TestTranslator.h>

namespace WebView {

void TestTranslator::detect_language(Web::TranslationLanguageSample const& sample, Function<void(Optional<String>)> on_complete)
{
    Core::deferred_invoke([on_complete = move(on_complete), language = sample.declared_language]() mutable {
        on_complete(move(language));
    });
}

void TestTranslator::target_languages(String const& source_language, Vector<String> const& candidates, Function<void(Vector<TargetLanguage>)> on_complete)
{
    Vector<TargetLanguage> target_languages;
    for (auto const& candidate : candidates) {
        if (is_same_language(candidate, source_language))
            continue;
        target_languages.append({ candidate, is_same_language(candidate, "xx"sv) });
    }
    Core::deferred_invoke([on_complete = move(on_complete), target_languages = move(target_languages)]() mutable {
        on_complete(move(target_languages));
    });
}

Translator::SessionId TestTranslator::create_session(String const&, String const& target_language, bool)
{
    auto session_id = ++m_last_session_id;
    m_target_languages.set(session_id, target_language);
    return session_id;
}

void TestTranslator::close_session(SessionId session_id)
{
    m_target_languages.remove(session_id);
}

void TestTranslator::translate(SessionId session_id, Web::TranslationUnit unit, Function<void(Optional<Vector<Web::TranslationRun>>)> on_complete)
{
    Vector<Web::TranslationRun> runs;
    runs.ensure_capacity(unit.runs.size());
    for (auto& run : unit.runs.in_reverse()) {
        if (!run.keep_as_is) {
            auto uppercase = run.text.to_ascii_uppercase();
            run.text = Utf16String::from_utf16(uppercase.utf16_view().trim_ascii_whitespace());
        }
        runs.unchecked_append(move(run));
    }
    if (auto target_language = m_target_languages.get(session_id); target_language.has_value() && !runs.is_empty()) {
        if (target_language->ends_with_bytes("-x-repeat"sv))
            runs.append(runs.first());
        else if (target_language->ends_with_bytes("-x-drop"sv) && runs.size() > 1)
            runs.take_last();
    }

    // Answer on a later turn of the event loop, as platform translators do, and never for a closed session.
    Core::deferred_invoke([this, session_id, on_complete = move(on_complete), runs = move(runs)]() mutable {
        if (m_target_languages.contains(session_id))
            on_complete(move(runs));
    });
}

}
