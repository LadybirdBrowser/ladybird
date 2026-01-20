/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <AK/Utf32View.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/Segmenter.h>

#include <unicode/brkiter.h>
#include <unicode/utext.h>
#include <unicode/utf8.h>

namespace Unicode {

SegmenterGranularity segmenter_granularity_from_string(StringView segmenter_granularity)
{
    if (segmenter_granularity == "grapheme"sv)
        return SegmenterGranularity::Grapheme;
    if (segmenter_granularity == "line"sv)
        return SegmenterGranularity::Line;
    if (segmenter_granularity == "sentence"sv)
        return SegmenterGranularity::Sentence;
    if (segmenter_granularity == "word"sv)
        return SegmenterGranularity::Word;
    VERIFY_NOT_REACHED();
}

StringView segmenter_granularity_to_string(SegmenterGranularity segmenter_granularity)
{
    switch (segmenter_granularity) {
    case SegmenterGranularity::Grapheme:
        return "grapheme"sv;
    case SegmenterGranularity::Line:
        return "line"sv;
    case SegmenterGranularity::Sentence:
        return "sentence"sv;
    case SegmenterGranularity::Word:
        return "word"sv;
    }
    VERIFY_NOT_REACHED();
}

// Fast path segmenter for ASCII text where every character is its own grapheme.
// This avoids all ICU overhead for the common case of ASCII-only text.
class AsciiGraphemeSegmenter : public Segmenter {
public:
    explicit AsciiGraphemeSegmenter(size_t length)
        : Segmenter(SegmenterGranularity::Grapheme)
        , m_length(length)
    {
    }

    virtual ~AsciiGraphemeSegmenter() override = default;

    virtual NonnullOwnPtr<Segmenter> clone() const override
    {
        return make<AsciiGraphemeSegmenter>(m_length);
    }

    virtual void set_segmented_text(String text) override
    {
        m_length = text.byte_count();
    }

    virtual void set_segmented_text(Utf16View const& text) override
    {
        m_length = text.length_in_code_units();
    }

    virtual size_t current_boundary() override
    {
        return m_current;
    }

    virtual Optional<size_t> previous_boundary(size_t index, Inclusive inclusive) override
    {
        if (inclusive == Inclusive::Yes && index <= m_length)
            return index;
        if (index == 0)
            return {};
        return index - 1;
    }

    virtual Optional<size_t> next_boundary(size_t index, Inclusive inclusive) override
    {
        if (inclusive == Inclusive::Yes && index <= m_length)
            return index;
        if (index >= m_length)
            return {};
        return index + 1;
    }

    virtual void for_each_boundary(String text, SegmentationCallback callback) override
    {
        set_segmented_text(move(text));
        for_each_boundary_impl(callback);
    }

    virtual void for_each_boundary(Utf16View const& text, SegmentationCallback callback) override
    {
        set_segmented_text(text);
        for_each_boundary_impl(callback);
    }

    virtual void for_each_boundary(Utf32View const& text, SegmentationCallback callback) override
    {
        m_length = text.length();
        for_each_boundary_impl(callback);
    }

    virtual bool is_current_boundary_word_like() const override
    {
        return false;
    }

private:
    void for_each_boundary_impl(SegmentationCallback& callback)
    {
        for (size_t i = 0; i <= m_length; ++i) {
            if (callback(i) == IterationDecision::Break)
                return;
        }
    }

    size_t m_length { 0 };
    size_t m_current { 0 };
};

class SegmenterImpl : public Segmenter {
public:
    SegmenterImpl(NonnullOwnPtr<icu::BreakIterator> segmenter, SegmenterGranularity segmenter_granularity)
        : Segmenter(segmenter_granularity)
        , m_segmenter(move(segmenter))
    {
    }

    virtual ~SegmenterImpl() override = default;

    virtual NonnullOwnPtr<Segmenter> clone() const override
    {
        return make<SegmenterImpl>(adopt_own(*m_segmenter->clone()), m_segmenter_granularity);
    }

    virtual void set_segmented_text(String text) override
    {
        UErrorCode status = U_ZERO_ERROR;

        m_segmented_text = move(text);
        auto view = m_segmented_text.get<String>().bytes_as_string_view();

        UText utext = UTEXT_INITIALIZER;
        utext_openUTF8(&utext, view.characters_without_null_termination(), static_cast<i64>(view.length()), &status);
        VERIFY(icu_success(status));

        m_segmenter->setText(&utext, status);
        VERIFY(icu_success(status));

        utext_close(&utext);
    }

    virtual void set_segmented_text(Utf16View const& text) override
    {
        if (text.has_ascii_storage()) {
            set_segmented_text(MUST(text.to_utf8()));
            return;
        }

        m_segmented_text = icu::UnicodeString { text.utf16_span().data(), static_cast<i32>(text.length_in_code_units()) };
        m_segmenter->setText(m_segmented_text.get<icu::UnicodeString>());
    }

    virtual size_t current_boundary() override
    {
        return m_segmenter->current();
    }

    virtual Optional<size_t> previous_boundary(size_t boundary, Inclusive inclusive) override
    {
        auto icu_boundary = align_boundary(boundary);

        if (inclusive == Inclusive::Yes) {
            if (static_cast<bool>(m_segmenter->isBoundary(icu_boundary)))
                return static_cast<size_t>(icu_boundary);
        }

        if (auto index = m_segmenter->preceding(icu_boundary); index != icu::BreakIterator::DONE)
            return static_cast<size_t>(index);

        return {};
    }

    virtual Optional<size_t> next_boundary(size_t boundary, Inclusive inclusive) override
    {
        auto icu_boundary = align_boundary(boundary);

        if (inclusive == Inclusive::Yes) {
            if (static_cast<bool>(m_segmenter->isBoundary(icu_boundary)))
                return static_cast<size_t>(icu_boundary);
        }

        if (auto index = m_segmenter->following(icu_boundary); index != icu::BreakIterator::DONE)
            return static_cast<size_t>(index);

        return {};
    }

    virtual void for_each_boundary(String text, SegmentationCallback callback) override
    {
        if (text.is_empty())
            return;

        set_segmented_text(move(text));
        for_each_boundary(move(callback));
    }

    virtual void for_each_boundary(Utf16View const& text, SegmentationCallback callback) override
    {
        if (text.is_empty())
            return;

        set_segmented_text(text);
        for_each_boundary(move(callback));
    }

    virtual void for_each_boundary(Utf32View const& text, SegmentationCallback callback) override
    {
        if (text.is_empty())
            return;

        // FIXME: We should be able to create a custom UText provider to avoid converting to UTF-8 here.
        set_segmented_text(MUST(String::formatted("{}", text)));

        auto code_points = m_segmented_text.get<String>().code_points();
        auto current = code_points.begin();
        size_t code_point_index = 0;

        for_each_boundary([&](auto index) {
            auto it = code_points.iterator_at_byte_offset(index);

            while (current != it) {
                ++code_point_index;
                ++current;
            }

            return callback(code_point_index);
        });
    }

    virtual bool is_current_boundary_word_like() const override
    {
        auto status = m_segmenter->getRuleStatus();

        if (status >= UBRK_WORD_NUMBER && status < UBRK_WORD_NUMBER_LIMIT)
            return true;
        if (status >= UBRK_WORD_LETTER && status < UBRK_WORD_LETTER_LIMIT)
            return true;
        if (status >= UBRK_WORD_KANA && status < UBRK_WORD_KANA_LIMIT)
            return true;
        if (status >= UBRK_WORD_IDEO && status < UBRK_WORD_IDEO_LIMIT)
            return true;

        return false;
    }

private:
    i32 align_boundary(size_t boundary)
    {
        auto icu_boundary = static_cast<i32>(boundary);

        return m_segmented_text.visit(
            [&](String const& text) {
                if (boundary >= text.byte_count())
                    return static_cast<i32>(text.byte_count());

                U8_SET_CP_START(text.bytes().data(), 0, icu_boundary);
                return icu_boundary;
            },
            [&](icu::UnicodeString const& text) {
                if (icu_boundary >= text.length())
                    return text.length();

                return text.getChar32Start(icu_boundary);
            },
            [](Empty) -> i32 { VERIFY_NOT_REACHED(); });
    }

    void for_each_boundary(SegmentationCallback callback)
    {
        if (callback(static_cast<size_t>(m_segmenter->first())) == IterationDecision::Break)
            return;

        while (true) {
            auto index = m_segmenter->next();
            if (index == icu::BreakIterator::DONE)
                return;

            if (callback(static_cast<size_t>(index)) == IterationDecision::Break)
                return;
        }
    }

    NonnullOwnPtr<icu::BreakIterator> m_segmenter;
    Variant<Empty, String, icu::UnicodeString> m_segmented_text;
};

NonnullOwnPtr<Segmenter> Segmenter::create(SegmenterGranularity segmenter_granularity)
{
    return Segmenter::create(default_locale(), segmenter_granularity);
}

NonnullOwnPtr<Segmenter> Segmenter::create(StringView locale, SegmenterGranularity segmenter_granularity)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale);
    VERIFY(locale_data.has_value());

    auto segmenter = adopt_own_if_nonnull([&]() {
        switch (segmenter_granularity) {
        case SegmenterGranularity::Grapheme:
            return icu::BreakIterator::createCharacterInstance(locale_data->locale(), status);
        case SegmenterGranularity::Line:
            return icu::BreakIterator::createLineInstance(locale_data->locale(), status);
        case SegmenterGranularity::Sentence:
            return icu::BreakIterator::createSentenceInstance(locale_data->locale(), status);
        case SegmenterGranularity::Word:
            return icu::BreakIterator::createWordInstance(locale_data->locale(), status);
        }
        VERIFY_NOT_REACHED();
    }());

    VERIFY(icu_success(status));

    return make<SegmenterImpl>(segmenter.release_nonnull(), segmenter_granularity);
}

NonnullOwnPtr<Segmenter> Segmenter::create_for_ascii_grapheme(size_t length)
{
    return make<AsciiGraphemeSegmenter>(length);
}

bool Segmenter::should_continue_beyond_word(Utf16View const& word)
{
    for (auto code_point : word) {
        if (!code_point_has_punctuation_general_category(code_point) && !code_point_has_separator_general_category(code_point))
            return false;
    }
    return true;
}

}
