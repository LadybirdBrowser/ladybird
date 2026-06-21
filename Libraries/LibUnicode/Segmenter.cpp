/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericShorthands.h>
#include <AK/OwnPtr.h>
#include <AK/Utf16View.h>
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

SegmenterGranularity segmenter_granularity_from_string(Utf16View segmenter_granularity)
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

Utf16String segmenter_granularity_to_string(SegmenterGranularity segmenter_granularity)
{
    switch (segmenter_granularity) {
    case SegmenterGranularity::Grapheme:
        return "grapheme"_utf16;
    case SegmenterGranularity::Line:
        return "line"_utf16;
    case SegmenterGranularity::Sentence:
        return "sentence"_utf16;
    case SegmenterGranularity::Word:
        return "word"_utf16;
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

static bool can_use_ascii_line_breaking_fast_path(ReadonlyBytes bytes)
{
    return all_of(bytes, [](u8 byte) { return is_ascii_printable(byte) || is_ascii_space(byte); });
}

// UAX#14 line-break classes that occur in printable ASCII plus ASCII whitespace.
// https://www.unicode.org/reports/tr14/#Table1
enum class AsciiLineBreakClass : u8 {
    AL, // Alphabetic (alphabets and regular symbols)
    BA, // Break After (TAB, '|')
    BK, // Mandatory Break (VT, FF)
    CL, // Close Punctuation ('}')
    CP, // Close Parenthesis (')', ']')
    CR, // Carriage Return
    EX, // Exclamation/Interrogation ('!', '?')
    HY, // Hyphen ('-')
    IS, // Infix Numeric Separator (',', '.', ':', ';')
    LF, // Line Feed
    NU, // Numeric (digits)
    OP, // Open Punctuation ('(', '[', '{')
    PO, // Postfix Numeric ('%')
    PR, // Prefix Numeric ('$', '+', '\\')
    QU, // Quotation ('"', '\'')
    SP, // Space
    SY, // Symbols Allowing Break After ('/')
};

static constexpr AsciiLineBreakClass classify_ascii_byte(u8 byte)
{
    using enum AsciiLineBreakClass;
    switch (byte) {
    case '\t':
    case '|':
        return BA;
    case '\n':
        return LF;
    case '\v':
    case '\f':
        return BK;
    case '\r':
        return CR;
    case ' ':
        return SP;
    case '!':
    case '?':
        return EX;
    case '"':
    case '\'':
        return QU;
    case '$':
    case '+':
    case '\\':
        return PR;
    case '%':
        return PO;
    case '(':
    case '[':
    case '{':
        return OP;
    case ')':
    case ']':
        return CP;
    case ',':
    case '.':
    case ':':
    case ';':
        return IS;
    case '-':
        return HY;
    case '/':
        return SY;
    case '}':
        return CL;
    default:
        return is_ascii_digit(byte) ? NU : AL;
    }
}

static void mark_ascii_numeric_expression_interiors(ReadonlyBytes text, Vector<bool>& interior_of_numeric_expression)
{
    // Identify spans matching UAX#14 LB25's numeric-expression grammar:
    //     (PR | PO)? (OP | HY)? IS? NU (NU | SY | IS)* (CL | CP)? (PR | PO)?
    // Positions strictly inside such a span are kept atomic by LB25 — positions at the very start or end of a span are
    // governed by surrounding rules so adjacent numeric expressions can still break apart.

    using enum AsciiLineBreakClass;
    interior_of_numeric_expression.clear_with_capacity();
    interior_of_numeric_expression.resize(text.size() + 1);

    size_t i = 0;
    while (i < text.size()) {
        if (classify_ascii_byte(text[i]) != NU) {
            ++i;
            continue;
        }

        // Walk the prefix backwards: NU is preceded by an optional IS, then optional OP/HY, then optional PR/PO.
        size_t start = i;
        if (start > 0 && classify_ascii_byte(text[start - 1]) == IS)
            --start;
        if (start > 0 && first_is_one_of(classify_ascii_byte(text[start - 1]), OP, HY))
            --start;
        if (start > 0 && first_is_one_of(classify_ascii_byte(text[start - 1]), PR, PO))
            --start;

        // Walk the body and suffix forward: NU (NU | SY | IS)* (CL | CP)? (PR | PO)?
        size_t end = i + 1;
        while (end < text.size() && first_is_one_of(classify_ascii_byte(text[end]), NU, SY, IS))
            ++end;
        if (end < text.size() && first_is_one_of(classify_ascii_byte(text[end]), CL, CP))
            ++end;
        if (end < text.size() && first_is_one_of(classify_ascii_byte(text[end]), PR, PO))
            ++end;

        // Mark positions strictly between `start` and `end` as interior. Position `start` and position `end`
        // are left alone so adjacent expressions can still break against each other.
        for (size_t position = start + 1; position < end; ++position)
            interior_of_numeric_expression[position] = true;

        i = end;
    }
}

static void compute_ascii_line_boundaries(ReadonlyBytes text, Vector<bool>& is_boundary)
{
    // Compute soft and mandatory line-break opportunities for ASCII text following UAX#14: https://www.unicode.org/reports/tr14/)
    // Only the rules whose left- and right-hand classes can occur in printable ASCII plus ASCII whitespace are
    // implemented.
    using enum AsciiLineBreakClass;

    is_boundary.clear_with_capacity();
    is_boundary.resize(text.size() + 1);
    is_boundary[0] = true;
    if (text.is_empty())
        return;
    is_boundary[text.size()] = true;

    // LB25: precompute positions interior to numeric expressions.
    Vector<bool> interior_of_numeric_expression;
    mark_ascii_numeric_expression_interiors(text, interior_of_numeric_expression);

    // LB14 state: have we just seen `OP SP*` (with no intervening non-SP)?
    bool in_op_sp_run = false;

    for (size_t i = 1; i < text.size(); ++i) {
        auto previous_class = classify_ascii_byte(text[i - 1]);
        auto current_class = classify_ascii_byte(text[i]);

        // LB5: CR × LF; CR ! ; LF ! .
        if (previous_class == CR && current_class == LF) {
            is_boundary[i] = false;
            continue;
        }
        // LB4: BK ! .  LB5 (continued): CR ! and LF ! .
        if (first_is_one_of(previous_class, BK, CR, LF)) {
            is_boundary[i] = true;
            in_op_sp_run = false;
            continue;
        }
        // LB6: × ( BK | CR | LF ).
        if (first_is_one_of(current_class, BK, CR, LF)) {
            is_boundary[i] = false;
            continue;
        }

        // LB14 trigger: `OP` starts an `OP SP*` run that suppresses breaks until the next non-SP.
        if (previous_class == OP)
            in_op_sp_run = true;

        // LB7: × SP. The OP-SP* run is preserved across spaces.
        if (current_class == SP) {
            is_boundary[i] = false;
            continue;
        }

        // LB14: OP SP* × (no break after OP, even across intervening spaces).
        if (in_op_sp_run) {
            is_boundary[i] = false;
            in_op_sp_run = false;
            continue;
        }

        // LB13: × CL × CP × EX × SY (IS is handled by LB15c/d below).
        if (first_is_one_of(current_class, CL, CP, EX, SY)) {
            is_boundary[i] = false;
            continue;
        }

        // LB15d: do not break before IS unless preceded by SP, and even then only when SP IS is followedby NU.
        if (current_class == IS) {
            if (previous_class != SP) {
                is_boundary[i] = false;
                continue;
            }
            auto next_class = i + 1 < text.size() ? classify_ascii_byte(text[i + 1]) : AL;
            bool next_forces_break = i + 1 < text.size() && next_class == NU;
            if (!next_forces_break) {
                is_boundary[i] = false;
                continue;
            }
            // Fall through: SP × IS NU → break before IS (per LB15c "leading decimal point").
            is_boundary[i] = true;
            continue;
        }

        // LB18: SP ÷ (default break opportunity after spaces).
        if (previous_class == SP) {
            is_boundary[i] = true;
            continue;
        }

        // LB19: × QU; QU × (treat ASCII straight quotes as ambiguous QU).
        if (current_class == QU || previous_class == QU) {
            is_boundary[i] = false;
            continue;
        }

        // LB20a: do not break between a hyphen and a following letter when the hyphen comes at the start of a line -
        // that is, at the start of the text, after a space, or after a forced break.
        if (previous_class == HY && current_class == AL) {
            bool hy_starts_line = i == 1;
            if (!hy_starts_line && i >= 2)
                hy_starts_line = first_is_one_of(classify_ascii_byte(text[i - 2]), SP, BK, CR, LF);
            if (hy_starts_line) {
                is_boundary[i] = false;
                continue;
            }
        }

        // LB21: × BA; × HY (break-before suppression for BA and HY).
        if (first_is_one_of(current_class, BA, HY)) {
            is_boundary[i] = false;
            continue;
        }

        // LB23: AL × NU; NU × AL.
        if ((previous_class == AL && current_class == NU) || (previous_class == NU && current_class == AL)) {
            is_boundary[i] = false;
            continue;
        }

        // LB24: (PR | PO) × AL; AL × (PR | PO).
        if ((first_is_one_of(previous_class, PR, PO) && current_class == AL)
            || (previous_class == AL && first_is_one_of(current_class, PR, PO))) {
            is_boundary[i] = false;
            continue;
        }

        // LB25: positions strictly inside a numeric expression are kept atomic.
        if (interior_of_numeric_expression[i]) {
            is_boundary[i] = false;
            continue;
        }

        // LB28: AL × AL.
        if (previous_class == AL && current_class == AL) {
            is_boundary[i] = false;
            continue;
        }

        // LB29: IS × AL.
        if (previous_class == IS && current_class == AL) {
            is_boundary[i] = false;
            continue;
        }

        // LB30: (AL | NU) × OP; CP × (AL | NU). All ASCII OP/CP characters are non-East-Asian.
        if (current_class == OP && first_is_one_of(previous_class, AL, NU)) {
            is_boundary[i] = false;
            continue;
        }
        if (previous_class == CP && first_is_one_of(current_class, AL, NU)) {
            is_boundary[i] = false;
            continue;
        }

        // LB31: ÷ (default).
        is_boundary[i] = true;
    }
}

// Implements UAX#14 line breaking rules for ASCII text: https://www.unicode.org/reports/tr14/tr14-39.html
class AsciiLineSegmenter : public Segmenter {
public:
    AsciiLineSegmenter()
        : Segmenter(SegmenterGranularity::Line)
    {
    }

    virtual ~AsciiLineSegmenter() override = default;

    virtual NonnullOwnPtr<Segmenter> clone() const override
    {
        return make<AsciiLineSegmenter>();
    }

    virtual void set_segmented_text(String text) override
    {
        apply(text.bytes());
    }

    virtual void set_segmented_text(Utf16View const& text) override
    {
        VERIFY(text.has_ascii_storage());
        auto span = text.ascii_span();
        apply({ span.data(), span.size() });
    }

    virtual size_t current_boundary() override
    {
        return m_current;
    }

    virtual Optional<size_t> previous_boundary(size_t index, Inclusive inclusive) override
    {
        if (inclusive == Inclusive::Yes && index <= text_size() && is_boundary(index))
            return index;
        if (index == 0)
            return {};
        size_t i = min(index, text_size() + 1);
        while (i > 0) {
            --i;
            if (is_boundary(i))
                return i;
        }
        return {};
    }

    virtual Optional<size_t> next_boundary(size_t index, Inclusive inclusive) override
    {
        if (inclusive == Inclusive::Yes && index <= text_size() && is_boundary(index))
            return index;
        if (index >= text_size())
            return {};
        for (size_t i = index + 1; i <= text_size(); ++i) {
            if (is_boundary(i))
                return i;
        }
        return {};
    }

    virtual void for_each_boundary(String text, SegmentationCallback callback) override
    {
        if (text.is_empty())
            return;
        set_segmented_text(move(text));
        iterate(callback);
    }

    virtual void for_each_boundary(Utf16View const& text, SegmentationCallback callback) override
    {
        if (text.is_empty())
            return;
        set_segmented_text(text);
        iterate(callback);
    }

    virtual bool is_current_boundary_word_like() const override
    {
        return false;
    }

private:
    void apply(ReadonlyBytes bytes)
    {
        VERIFY(can_use_ascii_line_breaking_fast_path(bytes));
        compute_ascii_line_boundaries(bytes, m_is_boundary);
        m_current = 0;
    }

    size_t text_size() const { return m_is_boundary.size() - 1; }

    bool is_boundary(size_t index) const
    {
        VERIFY(index < m_is_boundary.size());
        return m_is_boundary[index];
    }

    void iterate(SegmentationCallback& callback)
    {
        for (size_t i = 0; i <= text_size(); ++i) {
            if (!m_is_boundary[i])
                continue;
            m_current = i;
            if (callback(i) == IterationDecision::Break)
                return;
        }
    }

    Vector<bool> m_is_boundary;
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
        verify_icu_success(status);

        m_segmenter->setText(&utext, status);
        verify_icu_success(status);

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

NonnullOwnPtr<Segmenter> Segmenter::create(Utf16View locale, SegmenterGranularity segmenter_granularity)
{
    UErrorCode status = U_ZERO_ERROR;

    auto locale_data = LocaleData::for_locale(locale.bytes());
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

    verify_icu_success(status);

    return make<SegmenterImpl>(segmenter.release_nonnull(), segmenter_granularity);
}

NonnullOwnPtr<Segmenter> Segmenter::create_for_ascii_grapheme(size_t length)
{
    return make<AsciiGraphemeSegmenter>(length);
}

OwnPtr<Segmenter> Segmenter::try_create_for_ascii_line(Utf16View const& text)
{
    if (!text.has_ascii_storage())
        return {};
    auto span = text.ascii_span();
    ReadonlyBytes bytes { span.data(), span.size() };
    if (!can_use_ascii_line_breaking_fast_path(bytes))
        return {};
    auto segmenter = make<AsciiLineSegmenter>();
    segmenter->set_segmented_text(text);
    return segmenter;
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
