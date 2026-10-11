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
