/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/CharacterTypes.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/TestSpellChecker.h>

namespace WebView {

static constexpr Array s_misspelled_words {
    u"txet"sv,
    u"Txet"sv,
    u"quikc"sv,
    u"wrod"sv,
    u"speling"sv,
    u"lernword"sv,
    u"lernagain"sv,
};

static Vector<Utf16String> guesses_for(Utf16View word)
{
    auto guesses = [](std::initializer_list<StringView> words) {
        Vector<Utf16String> result;
        for (auto word : words)
            result.append(Utf16String::from_utf8(word));
        return result;
    };
    if (word == u"txet"sv)
        return guesses({ "text"sv, "teat"sv, "tet"sv });
    if (word == u"Txet"sv)
        return guesses({ "Text"sv, "Teat"sv, "Tet"sv });
    if (word == u"wrod"sv)
        return guesses({ "word"sv, "rod"sv, "wrong"sv, "wood"sv, "ford"sv, "prod"sv });
    if (word == u"speling"sv)
        return guesses({ "spelling"sv, "spieling"sv });
    if (word == u"lernword"sv)
        return guesses({ "learnword"sv });
    return {};
}

static bool is_word_code_unit(char16_t code_unit)
{
    return is_ascii_alpha(code_unit) || code_unit == '\'';
}

// What each word at the caret is predicted to become. The last one corrects what's typed rather than continuing it.
// Every test-web test has these predictions, so a test that types one of these words and then waits before pressing Tab
// accepts the prediction rather than moving focus.
static Optional<StringView> prediction_for(Utf16View partial_word)
{
    if (partial_word == u"predi"sv)
        return "prediction"sv;
    if (partial_word == u"Lady"sv)
        return "Ladybird browser"sv;
    if (partial_word == u"teh"sv)
        return "the"sv;
    return {};
}

SpellChecker::DocumentTag TestSpellChecker::create_document_tag()
{
    return m_next_document_tag++;
}

void TestSpellChecker::close_document_tag(DocumentTag tag)
{
    m_accepted_words.remove(tag);
}

bool TestSpellChecker::is_misspelled(DocumentTag tag, Utf16View word) const
{
    if (!any_of(s_misspelled_words, [&](auto misspelled) { return misspelled == word; }))
        return false;
    auto accepted = m_accepted_words.get(tag);
    return !accepted.has_value() || !accepted->contains(Utf16String::from_utf16(word));
}

void TestSpellChecker::check(DocumentTag tag, Utf16String const& text, Function<void(Vector<Web::SpellingError>)> on_complete)
{
    Vector<Web::SpellingError> errors;
    auto view = text.utf16_view();
    size_t index = 0;
    while (index < view.length_in_code_units()) {
        if (!is_word_code_unit(view.code_unit_at(index))) {
            ++index;
            continue;
        }
        auto start = index;
        while (index < view.length_in_code_units() && is_word_code_unit(view.code_unit_at(index)))
            ++index;
        if (is_misspelled(tag, view.substring_view(start, index - start)))
            errors.append({ static_cast<u32>(start), static_cast<u32>(index - start) });
    }

    // Answer on a later turn of the event loop, as platform spellcheckers do.
    Core::deferred_invoke([on_complete = move(on_complete), errors = move(errors)]() mutable {
        on_complete(move(errors));
    });
}

void TestSpellChecker::suggest(DocumentTag, Utf16String const& text, u32 caret_offset, Function<void(Optional<Web::WritingSuggestion>)> on_complete)
{
    Optional<Web::WritingSuggestion> suggestion;
    auto view = text.utf16_view();
    if (caret_offset <= view.length_in_code_units()) {
        // The candidate replaces the whole word the caret is in, as platform spellcheckers' candidates do.
        auto start = caret_offset;
        while (start > 0 && is_word_code_unit(view.code_unit_at(start - 1)))
            --start;
        auto end = caret_offset;
        while (end < view.length_in_code_units() && is_word_code_unit(view.code_unit_at(end)))
            ++end;
        if (start < caret_offset) {
            if (auto prediction = prediction_for(view.substring_view(start, end - start)); prediction.has_value())
                suggestion = Web::WritingSuggestion { start, end - start, Utf16String::from_utf8(*prediction) };
        }
    }

    // Answer on a later turn of the event loop, as platform spellcheckers do.
    Core::deferred_invoke([on_complete = move(on_complete), suggestion = move(suggestion)]() mutable {
        on_complete(move(suggestion));
    });
}

Vector<Utf16String> TestSpellChecker::guesses(DocumentTag, Utf16String const& word)
{
    return guesses_for(word.utf16_view());
}

void TestSpellChecker::ignore(DocumentTag tag, Utf16String const& word)
{
    m_accepted_words.ensure(tag).set(word);
}

void TestSpellChecker::learn(DocumentTag tag, Utf16String const& word)
{
    m_accepted_words.ensure(tag).set(word);
}

}
