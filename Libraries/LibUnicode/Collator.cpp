/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/CharacterTypes.h>
#include <AK/GenericShorthands.h>
#include <AK/OwnPtr.h>
#include <LibUnicode/Collator.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/Normalize.h>
#include <LibUnicode/Segmenter.h>

#include <unicode/coleitr.h>
#include <unicode/coll.h>
#include <unicode/stsearch.h>
#include <unicode/tblcoll.h>
#include <unicode/uchar.h>
#include <unicode/uniset.h>
#include <unicode/uscript.h>
#include <unicode/usearch.h>
#include <unicode/usetiter.h>

namespace Unicode {

Usage usage_from_string(Utf16View usage)
{
    if (usage == "sort"sv)
        return Usage::Sort;
    if (usage == "search"sv)
        return Usage::Search;
    VERIFY_NOT_REACHED();
}

Utf16String usage_to_string(Usage usage)
{
    switch (usage) {
    case Usage::Sort:
        return "sort"_utf16;
    case Usage::Search:
        return "search"_utf16;
    }
    VERIFY_NOT_REACHED();
}

static NonnullOwnPtr<icu::Locale> apply_usage_to_locale(icu::Locale const& locale, Usage usage, Utf16View collation)
{
    auto result = adopt_own(*locale.clone());
    UErrorCode status = U_ZERO_ERROR;

    switch (usage) {
    case Usage::Sort:
        result->setUnicodeKeywordValue("co", icu_string_piece(StringView { collation.ascii_bytes() }), status);
        break;
    case Usage::Search:
        result->setUnicodeKeywordValue("co", "search", status);
        break;
    }

    verify_icu_success(status);
    return result;
}

Sensitivity sensitivity_from_string(Utf16View sensitivity)
{
    if (sensitivity == "base"sv)
        return Sensitivity::Base;
    if (sensitivity == "accent"sv)
        return Sensitivity::Accent;
    if (sensitivity == "case"sv)
        return Sensitivity::Case;
    if (sensitivity == "variant"sv)
        return Sensitivity::Variant;
    VERIFY_NOT_REACHED();
}

Utf16String sensitivity_to_string(Sensitivity sensitivity)
{
    switch (sensitivity) {
    case Sensitivity::Base:
        return "base"_utf16;
    case Sensitivity::Accent:
        return "accent"_utf16;
    case Sensitivity::Case:
        return "case"_utf16;
    case Sensitivity::Variant:
        return "variant"_utf16;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UColAttributeValue icu_sensitivity(Sensitivity sensitivity)
{
    switch (sensitivity) {
    case Sensitivity::Base:
        return UCOL_PRIMARY;
    case Sensitivity::Accent:
        return UCOL_SECONDARY;
    case Sensitivity::Case:
        return UCOL_PRIMARY;
    case Sensitivity::Variant:
        return UCOL_TERTIARY;
    }
    VERIFY_NOT_REACHED();
}

static Sensitivity sensitivity_for_collator(icu::Collator const& collator)
{
    UErrorCode status = U_ZERO_ERROR;

    auto attribute = collator.getAttribute(UCOL_STRENGTH, status);
    verify_icu_success(status);

    switch (attribute) {
    case UCOL_PRIMARY:
        attribute = collator.getAttribute(UCOL_CASE_LEVEL, status);
        verify_icu_success(status);

        return attribute == UCOL_ON ? Sensitivity::Case : Sensitivity::Base;

    case UCOL_SECONDARY:
        return Sensitivity::Accent;

    default:
        return Sensitivity::Variant;
    }
}

CaseFirst case_first_from_string(Utf16View case_first)
{
    if (case_first == "upper"sv)
        return CaseFirst::Upper;
    if (case_first == "lower"sv)
        return CaseFirst::Lower;
    if (case_first == "false"sv)
        return CaseFirst::False;
    VERIFY_NOT_REACHED();
}

Utf16String case_first_to_string(CaseFirst case_first)
{
    switch (case_first) {
    case CaseFirst::Upper:
        return "upper"_utf16;
    case CaseFirst::Lower:
        return "lower"_utf16;
    case CaseFirst::False:
        return "false"_utf16;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UColAttributeValue icu_case_first(CaseFirst case_first)
{
    switch (case_first) {
    case CaseFirst::Upper:
        return UCOL_UPPER_FIRST;
    case CaseFirst::Lower:
        return UCOL_LOWER_FIRST;
    case CaseFirst::False:
        return UCOL_OFF;
    }
    VERIFY_NOT_REACHED();
}

static bool ignore_punctuation_for_collator(icu::Collator const& collator)
{
    UErrorCode status = U_ZERO_ERROR;

    auto attribute = collator.getAttribute(UCOL_ALTERNATE_HANDLING, status);
    verify_icu_success(status);

    return attribute == UCOL_SHIFTED;
}

class SubstringSearcherImpl final : public Collator::SubstringSearcher {
public:
    AK_ALLOC_WITH_KMALLOC;

    SubstringSearcherImpl(icu::RuleBasedCollator const& collator, Utf16View const& haystack, Utf16View const& needle)
    {
        auto to_unicode_string = [](Utf16View const& string) {
            if (string.has_latin1_storage()) {
                auto utf8 = MUST(string.to_utf8());
                return icu::UnicodeString::fromUTF8(icu_string_piece(utf8.bytes_as_string_view()));
            }
            return icu::UnicodeString { string.utf16_span().data(), static_cast<i32>(string.length_in_code_units()) };
        };

        auto* cloned_collator = dynamic_cast<icu::RuleBasedCollator*>(collator.clone());
        VERIFY(cloned_collator);
        m_collator = adopt_own(*cloned_collator);
        m_pattern = to_unicode_string(needle);
        m_text = to_unicode_string(haystack);

        UErrorCode status = U_ZERO_ERROR;
        m_search = make<icu::StringSearch>(m_pattern, m_text, m_collator.ptr(), nullptr, status);
        verify_icu_success(status);
    }

    virtual Optional<Collator::Match> find_from(size_t start_offset) override
    {
        if (m_pattern.isEmpty() || start_offset > static_cast<size_t>(m_text.length()))
            return {};

        // ICU snaps offsets inside a surrogate pair or contraction backwards, so step forward until a match doesn't.
        auto search_offset = static_cast<i32>(start_offset);
        while (search_offset < m_text.length()) {
            UErrorCode status = U_ZERO_ERROR;
            m_search->setOffset(search_offset, status);
            verify_icu_success(status);

            auto const match_start = m_search->next(status);
            verify_icu_success(status);
            if (match_start == USEARCH_DONE)
                return {};

            if (static_cast<size_t>(match_start) >= start_offset) {
                return Collator::Match {
                    .start = static_cast<size_t>(match_start),
                    .end = static_cast<size_t>(match_start + m_search->getMatchedLength()),
                };
            }

            search_offset = m_text.moveIndex32(search_offset, 1);
        }
        return {};
    }

private:
    OwnPtr<icu::RuleBasedCollator> m_collator;
    icu::UnicodeString m_pattern;
    icu::UnicodeString m_text;
    OwnPtr<icu::StringSearch> m_search;
};

class NeedleSearcherImpl final : public Collator::NeedleSearcher {
public:
    AK_ALLOC_WITH_KMALLOC;

    NeedleSearcherImpl(icu::RuleBasedCollator const& collator, Utf16View const& needle)
    {
        if (needle.is_empty())
            return;

        auto* cloned_collator = dynamic_cast<icu::RuleBasedCollator*>(collator.clone());
        VERIFY(cloned_collator);
        m_collator = adopt_own(*cloned_collator);

        // ICU keeps a pointer to the needle.
        auto unowned_needle = icu_string(needle);
        m_needle.setTo(unowned_needle.getBuffer(), unowned_needle.length());

        // ICU cannot open a search without a haystack.
        static constexpr char16_t placeholder_haystack = u'\n';

        UErrorCode status = U_ZERO_ERROR;
        m_search = usearch_openFromCollator(
            m_needle.getBuffer(), m_needle.length(), &placeholder_haystack, 1, m_collator->toUCollator(), nullptr, &status);
        verify_icu_success(status);
    }

    virtual ~NeedleSearcherImpl() override
    {
        if (m_search)
            usearch_close(m_search);
    }

    virtual void set_haystack(Utf16View const& haystack) override
    {
        m_haystack = nullptr;
        if (!m_search || haystack.is_empty())
            return;

        char16_t const* text = nullptr;
        if (haystack.has_latin1_storage()) {
            m_converted_haystack = icu_string(haystack);
            text = m_converted_haystack.getBuffer();
        } else {
            text = haystack.utf16_span().data();
        }

        UErrorCode status = U_ZERO_ERROR;
        usearch_setText(m_search, text, static_cast<i32>(haystack.length_in_code_units()), &status);
        verify_icu_success(status);
        m_haystack = text;
        m_haystack_length = static_cast<i32>(haystack.length_in_code_units());
    }

    virtual Optional<Collator::Match> find_from(size_t start_offset) override
    {
        if (!m_haystack)
            return {};

        // ICU moves an offset inside a surrogate pair or a contraction back to its start.
        auto search_offset = static_cast<i32>(min(start_offset, static_cast<size_t>(m_haystack_length)));
        if (search_offset == m_haystack_length) {
            UErrorCode status = U_ZERO_ERROR;
            usearch_setOffset(m_search, search_offset, &status);
            verify_icu_success(status);
        }
        while (search_offset < m_haystack_length) {
            UErrorCode status = U_ZERO_ERROR;
            usearch_setOffset(m_search, search_offset, &status);
            verify_icu_success(status);

            auto match = next_match();
            if (!match.has_value() || match->start >= start_offset)
                return match;

            U16_FWD_1(m_haystack, search_offset, m_haystack_length);
        }
        return {};
    }

    virtual Optional<Collator::Match> next_match() override
    {
        if (!m_haystack)
            return {};

        UErrorCode status = U_ZERO_ERROR;
        auto const match_start = usearch_next(m_search, &status);
        verify_icu_success(status);
        if (match_start == USEARCH_DONE)
            return {};

        auto const match_length = usearch_getMatchedLength(m_search);
        if (match_length == 0)
            return {};

        return Collator::Match {
            .start = static_cast<size_t>(match_start),
            .end = static_cast<size_t>(match_start + match_length),
        };
    }

private:
    icu::UnicodeString m_needle;
    OwnPtr<icu::RuleBasedCollator> m_collator;
    icu::UnicodeString m_converted_haystack;
    UStringSearch* m_search { nullptr };
    char16_t const* m_haystack { nullptr };
    i32 m_haystack_length { 0 };
};

class CollatorImpl : public Collator {
public:
    explicit CollatorImpl(NonnullOwnPtr<icu::Collator> collator)
        : m_collator(move(collator))
    {
    }

    virtual Collator::Order compare(Utf16View const& lhs, Utf16View const& rhs) const override
    {
        UErrorCode status = U_ZERO_ERROR;
        UCollationResult result;

        // NB: ICU has no iterator over Latin-1, so a string with Latin-1 storage that is not ASCII is compared as UTF-16.
        auto has_non_ascii_latin1_storage = [](Utf16View const& string) { return string.has_latin1_storage() && !string.has_ascii_storage(); };
        if (has_non_ascii_latin1_storage(lhs) || has_non_ascii_latin1_storage(rhs)) {
            result = m_collator->compare(icu_string(lhs), icu_string(rhs), status);
        } else {
            auto lhs_it = icu_string_iterator(lhs);
            auto rhs_it = icu_string_iterator(rhs);
            result = m_collator->compare(lhs_it, rhs_it, status);
        }
        verify_icu_success(status);

        switch (result) {
        case UCOL_LESS:
            return Order::Before;
        case UCOL_EQUAL:
            return Order::Equal;
        case UCOL_GREATER:
            return Order::After;
        }

        VERIFY_NOT_REACHED();
    }

    virtual NonnullOwnPtr<SubstringSearcher> create_substring_searcher(Utf16View const& haystack, Utf16View const& needle) const override
    {
        auto* rule_based_collator = dynamic_cast<icu::RuleBasedCollator*>(m_collator.ptr());
        VERIFY(rule_based_collator);
        return make<SubstringSearcherImpl>(*rule_based_collator, haystack, needle);
    }

    virtual NonnullOwnPtr<NeedleSearcher> create_needle_searcher(Utf16View const& needle) const override
    {
        auto* rule_based_collator = dynamic_cast<icu::RuleBasedCollator*>(m_collator.ptr());
        VERIFY(rule_based_collator);

        UErrorCode status = U_ZERO_ERROR;
        VERIFY(m_collator->getAttribute(UCOL_CASE_LEVEL, status) == UCOL_OFF);
        VERIFY(m_collator->getAttribute(UCOL_NUMERIC_COLLATION, status) == UCOL_OFF);
        verify_icu_success(status);
        return make<NeedleSearcherImpl>(*rule_based_collator, needle);
    }

    icu::RuleBasedCollator const& rule_based_collator() const
    {
        auto const* rule_based_collator = dynamic_cast<icu::RuleBasedCollator const*>(m_collator.ptr());
        VERIFY(rule_based_collator);
        return *rule_based_collator;
    }

    virtual Sensitivity sensitivity() const override
    {
        return sensitivity_for_collator(*m_collator);
    }

    virtual bool ignore_punctuation() const override
    {
        return ignore_punctuation_for_collator(*m_collator);
    }

private:
    NonnullOwnPtr<icu::Collator> m_collator;
};

NonnullOwnPtr<Collator> Collator::create(
    Utf16View locale,
    Usage usage,
    Utf16View collation,
    Optional<Sensitivity> sensitivity,
    CaseFirst case_first,
    bool numeric,
    Optional<bool> ignore_punctuation)
{
    UErrorCode status = U_ZERO_ERROR;

    auto& locale_data = LocaleData::for_locale_or_default(locale);

    auto locale_with_usage = apply_usage_to_locale(locale_data.locale(), usage, collation);

    auto collator = adopt_own(*icu::Collator::createInstance(*locale_with_usage, status));
    verify_icu_success(status);

    auto set_attribute = [&](UColAttribute attribute, UColAttributeValue value) {
        collator->setAttribute(attribute, value, status);
        verify_icu_success(status);
    };

    if (!sensitivity.has_value())
        sensitivity = sensitivity_for_collator(*collator);

    if (!ignore_punctuation.has_value())
        ignore_punctuation = ignore_punctuation_for_collator(*collator);

    set_attribute(UCOL_STRENGTH, icu_sensitivity(*sensitivity));
    set_attribute(UCOL_CASE_LEVEL, sensitivity == Sensitivity::Case ? UCOL_ON : UCOL_OFF);
    set_attribute(UCOL_CASE_FIRST, icu_case_first(case_first));
    set_attribute(UCOL_NUMERIC_COLLATION, numeric ? UCOL_ON : UCOL_OFF);
    set_attribute(UCOL_ALTERNATE_HANDLING, *ignore_punctuation ? UCOL_SHIFTED : UCOL_NON_IGNORABLE);
    set_attribute(UCOL_NORMALIZATION_MODE, UCOL_ON);

    return adopt_own(*new CollatorImpl(move(collator)));
}

static bool can_have_search_key(UChar32 code_point)
{
    if (U_IS_SURROGATE(code_point) || U_IS_UNICODE_NONCHAR(code_point))
        return false;
    switch (u_getIntPropertyValue(code_point, UCHAR_GRAPHEME_CLUSTER_BREAK)) {
    case U_GCB_OTHER:
    case U_GCB_CONTROL:
    case U_GCB_LF:
        return true;
    default:
        return false;
    }
}

class SearchKeyTable {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit SearchKeyTable(Sensitivity sensitivity)
        : m_collator(Collator::create({}, Usage::Search, {}, sensitivity, CaseFirst::False, false, false))
        , m_matches_case(sensitivity == Sensitivity::Variant)
    {
        auto const& collator = static_cast<CollatorImpl const&>(*m_collator).rule_based_collator();
        m_elements = adopt_own(*collator.createCollationElementIterator(icu::UnicodeString {}));

        UErrorCode status = U_ZERO_ERROR;
        icu::UnicodeSet contractions;
        ucol_getContractionsAndExpansions(collator.toUCollator(), contractions.toUSet(), nullptr, true, &status);
        verify_icu_success(status);

        // A contraction applies only where all of its code points occur.
        for (icu::UnicodeSetIterator iterator { contractions }; iterator.next();) {
            if (!iterator.isString())
                continue;
            auto const& contraction = iterator.getString();
            UChar32 last_code_point = 0;
            auto has_code_point_without_key = false;
            for (i32 index = 0; index < contraction.length(); index = contraction.moveIndex32(index, 1)) {
                last_code_point = contraction.char32At(index);
                has_code_point_without_key |= !can_have_search_key(last_code_point);
            }
            if (!has_code_point_without_key)
                m_contraction_ends.add(last_code_point);
        }
        m_contraction_ends.freeze();
    }

    Optional<u64> key(u32 code_point)
    {
        auto& page = m_pages[code_point >> 8];
        if (page.is_empty())
            page.resize_with_default_value(256, unknown_key);
        auto& key = page[code_point & 0xff];
        if (key == unknown_key)
            key = compute_key(static_cast<UChar32>(code_point)).value_or(no_key);
        if (key == no_key)
            return {};
        return key;
    }

    ALWAYS_INLINE bool code_point_has_key(u32 code_point, u64 key)
    {
        if (auto const* page = m_pages.data()[code_point >> 8].data()) {
            if (auto known_key = page[code_point & 0xff]; known_key != unknown_key)
                return known_key == key;
        }
        return this->key(code_point) == key;
    }

private:
    static constexpr u64 unknown_key = 0;
    static constexpr u64 no_key = 1;
    static constexpr u32 continuation_marker = 0xc0;

    struct Element {
        u32 weights { 0 };
        bool is_continuation { false };

        u32 primary_weight() const { return weights >> 16; }
    };

    Optional<u64> compute_key(UChar32 code_point)
    {
        if (!can_have_search_key(code_point) || m_contraction_ends.contains(code_point))
            return {};

        UErrorCode status = U_ZERO_ERROR;
        m_elements->setText(icu::UnicodeString { code_point }, status);
        verify_icu_success(status);

        Vector<Element, 3> elements;
        for (auto element = m_elements->next(status); element != icu::CollationElementIterator::NULLORDER; element = m_elements->next(status)) {
            auto weights = static_cast<u32>(element);
            auto is_continuation = (weights & continuation_marker) == continuation_marker;
            if (!m_matches_case)
                weights &= 0xffff0000;
            if (weights == 0)
                continue;
            if (elements.size() == 3)
                return {};
            elements.append({ weights, is_continuation });
        }
        verify_icu_success(status);

        if (elements.is_empty() || elements[0].primary_weight() == 0 || elements[0].is_continuation)
            return {};
        auto key = static_cast<u64>(elements[0].weights) << 32;
        if (elements.size() == 1)
            return key;
        if (elements.size() == 2 && elements[1].is_continuation)
            return key | elements[1].weights;
        if (!m_matches_case)
            return {};

        u32 accents = 0;
        for (auto const& accent : elements.span().slice(1)) {
            if (accent.primary_weight() != 0 || accent.is_continuation)
                return {};
            accents = (accents << 16) | (accent.weights & 0xffff);
        }
        return key | accents;
    }

    NonnullOwnPtr<Collator> m_collator;
    bool m_matches_case { false };
    OwnPtr<icu::CollationElementIterator> m_elements;
    icu::UnicodeSet m_contraction_ends;
    Array<Vector<u64>, 0x1100> m_pages;
};

static SearchKeyTable& search_key_table(Sensitivity sensitivity)
{
    VERIFY(first_is_one_of(sensitivity, Sensitivity::Base, Sensitivity::Variant));

    using Tables = Array<OwnPtr<SearchKeyTable>, 2>;
#ifdef AK_COMPILER_CLANG
    AK_IGNORE_DIAGNOSTIC("-Wexit-time-destructors", static thread_local Tables tables)
#else
    static thread_local Tables tables;
#endif
    auto& table = tables[sensitivity == Sensitivity::Variant ? 1 : 0];
    if (!table)
        table = make<SearchKeyTable>(sensitivity);
    return *table;
}

Optional<u64> search_key_for_code_point(u32 code_point, Sensitivity sensitivity)
{
    if (!first_is_one_of(sensitivity, Sensitivity::Base, Sensitivity::Variant))
        return {};
    return search_key_table(sensitivity).key(code_point);
}

}

extern "C" {
void* unicode_layout_text_searcher_create(u16 const*, size_t, bool);
bool unicode_layout_text_searcher_has_search_keys(void*);
bool unicode_layout_text_is_searchable_without_collator(u16 const*, size_t);
void unicode_layout_text_searcher_set_text(void*, u16 const*, size_t, size_t, bool);
bool unicode_layout_text_searcher_next_match(void*, size_t*, size_t*);
void unicode_layout_text_searcher_destroy(void*);
}

static constexpr u32 combining_voiced_sound_mark = 0x3099;
static constexpr u32 combining_semi_voiced_sound_mark = 0x309a;

static bool is_kana_letter(u32 code_point)
{
    auto icu_code_point = static_cast<UChar32>(code_point);
    auto category = u_charType(icu_code_point);
    if (category != U_OTHER_LETTER && category != U_MODIFIER_LETTER)
        return false;
    // The script extensions also cover the vertical kana repeat marks.
    return uscript_hasScript(icu_code_point, USCRIPT_HIRAGANA) || uscript_hasScript(icu_code_point, USCRIPT_KATAKANA);
}

static bool is_small_kana_letter(u32 code_point)
{
    if ((code_point >= 0x31f0 && code_point <= 0x31ff)
        || (code_point >= 0x1b150 && code_point <= 0x1b152)
        || (code_point >= 0x1b164 && code_point <= 0x1b167)
        || first_is_one_of(code_point, 0x1b132u, 0x1b155u))
        return true;
    // The katakana block lays its letters out as the hiragana block does.
    if (code_point >= 0x30a1 && code_point <= 0x30f6)
        code_point -= 0x60;
    return first_is_one_of(code_point, 0x3041u, 0x3043u, 0x3045u, 0x3047u, 0x3049u, 0x3063u, 0x3083u, 0x3085u, 0x3087u, 0x308eu, 0x3095u, 0x3096u);
}

static bool is_voiced_vertical_kana_repeat_mark(u32 code_point)
{
    return first_is_one_of(code_point, 0x3032u, 0x3034u);
}

static bool extends_grapheme_cluster(u32 code_point)
{
    switch (u_getIntPropertyValue(static_cast<UChar32>(code_point), UCHAR_GRAPHEME_CLUSTER_BREAK)) {
    case U_GCB_EXTEND:
    case U_GCB_SPACING_MARK:
    case U_GCB_ZWJ:
        return true;
    default:
        return false;
    }
}

static Utf16String decompose_kana(Utf16View const& string)
{
    return Unicode::normalize(string, Unicode::NormalizationForm::NFKD);
}

static bool kana_letters_agree(Utf16View const& decomposed_needle, Utf16View const& decomposed_match)
{
    auto next_kana_letter = [](Utf16View const& string, size_t& index) -> Optional<u32> {
        while (index < string.length_in_code_units()) {
            auto code_point = string.code_point_at(index);
            index += AK::UnicodeUtils::code_unit_length_for_code_point(code_point);
            if (is_kana_letter(code_point))
                return code_point;
        }
        return {};
    };
    auto next_voicing_mark = [](Utf16View const& string, size_t& index) -> u32 {
        while (index < string.length_in_code_units()) {
            auto code_point = string.code_point_at(index);
            if (!extends_grapheme_cluster(code_point))
                break;
            index += AK::UnicodeUtils::code_unit_length_for_code_point(code_point);
            if (first_is_one_of(code_point, combining_voiced_sound_mark, combining_semi_voiced_sound_mark))
                return code_point;
        }
        return 0;
    };

    size_t needle_index = 0;
    size_t match_index = 0;
    while (true) {
        auto needle_letter = next_kana_letter(decomposed_needle, needle_index);
        auto match_letter = next_kana_letter(decomposed_match, match_index);
        if (!needle_letter.has_value() || !match_letter.has_value())
            return needle_letter.has_value() == match_letter.has_value();

        if (is_small_kana_letter(*needle_letter) != is_small_kana_letter(*match_letter))
            return false;

        // The voiced vertical kana repeat marks have no decomposition.
        auto needle_letter_is_voiced = is_voiced_vertical_kana_repeat_mark(*needle_letter);
        auto match_letter_is_voiced = is_voiced_vertical_kana_repeat_mark(*match_letter);
        while (true) {
            auto needle_mark = exchange(needle_letter_is_voiced, false) ? combining_voiced_sound_mark : next_voicing_mark(decomposed_needle, needle_index);
            auto match_mark = exchange(match_letter_is_voiced, false) ? combining_voiced_sound_mark : next_voicing_mark(decomposed_match, match_index);
            if (needle_mark != match_mark)
                return false;
            if (needle_mark == 0)
                break;
        }
    }
}

struct UnicodeLayoutTextSearcher {
    AK_ALLOC_WITH_KMALLOC;

    NonnullOwnPtr<Unicode::Collator::NeedleSearcher> searcher;
    Optional<Utf16String> decomposed_kana_needle;
    Utf16View text;
    Unicode::Sensitivity sensitivity { Unicode::Sensitivity::Base };
    Vector<u64> needle_keys;
    bool compares_search_keys { false };
    size_t search_offset { 0 };
    OwnPtr<Unicode::Segmenter> grapheme_segmenter {};
    bool grapheme_segmenter_has_text { false };
};

static bool can_search_without_collator(Utf16View const& string)
{
    auto& ignoring_case = Unicode::search_key_table(Unicode::Sensitivity::Base);
    auto& matching_case = Unicode::search_key_table(Unicode::Sensitivity::Variant);
    for (size_t index = 0; index < string.length_in_code_units();) {
        auto code_point = string.code_point_at(index);
        if (!ignoring_case.key(code_point).has_value() || !matching_case.key(code_point).has_value())
            return false;
        index += AK::UnicodeUtils::code_unit_length_for_code_point(code_point);
    }
    return true;
}

static Optional<Unicode::Collator::Match> next_search_key_match(UnicodeLayoutTextSearcher& searcher)
{
    auto& keys = Unicode::search_key_table(searcher.sensitivity);
    auto const* code_units = searcher.text.utf16_span().data();
    auto length = searcher.text.length_in_code_units();
    auto const& needle = searcher.needle_keys;

    auto next_code_point = [&](size_t& index) -> u32 {
        u32 code_point = code_units[index++];
        if (AK::UnicodeUtils::is_utf16_high_surrogate(code_point) && index < length && AK::UnicodeUtils::is_utf16_low_surrogate(code_units[index]))
            return AK::UnicodeUtils::decode_utf16_surrogate_pair(code_point, code_units[index++]);
        return code_point;
    };

    for (auto start = searcher.search_offset; length - start >= needle.size();) {
        auto end = start;
        size_t matched_keys = 0;
        while (matched_keys < needle.size() && end < length && keys.code_point_has_key(next_code_point(end), needle[matched_keys]))
            ++matched_keys;
        if (matched_keys == needle.size())
            return Unicode::Collator::Match { .start = start, .end = end };
        if (end == length)
            return {};
        next_code_point(start);
    }
    return {};
}

static size_t end_of_grapheme_cluster(UnicodeLayoutTextSearcher& searcher, size_t end)
{
    auto const& text = searcher.text;
    if (end == 0 || end >= text.length_in_code_units())
        return end;

    // Every rule that keeps two code points in one cluster needs one of them to have a break property of its own.
    auto const* code_units = text.utf16_span().data();
    auto length = static_cast<i32>(text.length_in_code_units());
    auto offset = static_cast<i32>(end);
    UChar32 before = 0;
    UChar32 after = 0;
    U16_GET(code_units, 0, offset, length, after);
    U16_PREV(code_units, 0, offset, before);
    if (u_getIntPropertyValue(before, UCHAR_GRAPHEME_CLUSTER_BREAK) == U_GCB_OTHER
        && u_getIntPropertyValue(after, UCHAR_GRAPHEME_CLUSTER_BREAK) == U_GCB_OTHER)
        return end;

    if (!searcher.grapheme_segmenter)
        searcher.grapheme_segmenter = Unicode::Segmenter::create(Unicode::SegmenterGranularity::Grapheme);
    if (!searcher.grapheme_segmenter_has_text) {
        searcher.grapheme_segmenter->set_segmented_text(text);
        searcher.grapheme_segmenter_has_text = true;
    }
    return searcher.grapheme_segmenter->next_boundary(end, Unicode::Segmenter::Inclusive::Yes).value_or(text.length_in_code_units());
}

extern "C" void* unicode_layout_text_searcher_create(u16 const* needle, size_t length, bool case_sensitive)
{
    Utf16View needle_view { reinterpret_cast<char16_t const*>(needle), length };

    // A search that ignores case also ignores accents and character width.
    auto sensitivity = case_sensitive ? Unicode::Sensitivity::Variant : Unicode::Sensitivity::Base;
    auto collator = Unicode::Collator::create({}, Unicode::Usage::Search, {}, sensitivity, Unicode::CaseFirst::False, false, false);

    Optional<Utf16String> decomposed_kana_needle;
    if (auto decomposed_needle = decompose_kana(needle_view); any_of(decomposed_needle.utf16_view(), is_kana_letter))
        decomposed_kana_needle = move(decomposed_needle);

    Vector<u64> needle_keys;
    for (size_t index = 0; index < length;) {
        auto code_point = needle_view.code_point_at(index);
        auto key = Unicode::search_key_for_code_point(code_point, sensitivity);
        if (!key.has_value()) {
            needle_keys.clear();
            break;
        }
        needle_keys.append(*key);
        index += AK::UnicodeUtils::code_unit_length_for_code_point(code_point);
    }

    return new UnicodeLayoutTextSearcher {
        .searcher = collator->create_needle_searcher(needle_view),
        .decomposed_kana_needle = move(decomposed_kana_needle),
        .text = {},
        .sensitivity = sensitivity,
        .needle_keys = move(needle_keys),
    };
}

extern "C" bool unicode_layout_text_searcher_has_search_keys(void* handle)
{
    VERIFY(handle);
    return !static_cast<UnicodeLayoutTextSearcher*>(handle)->needle_keys.is_empty();
}

extern "C" bool unicode_layout_text_is_searchable_without_collator(u16 const* text, size_t length)
{
    return can_search_without_collator({ reinterpret_cast<char16_t const*>(text), length });
}

extern "C" void unicode_layout_text_searcher_set_text(void* handle, u16 const* text, size_t length, size_t offset, bool searchable_without_collator)
{
    VERIFY(handle);
    auto& searcher = *static_cast<UnicodeLayoutTextSearcher*>(handle);
    searcher.text = { reinterpret_cast<char16_t const*>(text), length };
    searcher.compares_search_keys = !searcher.needle_keys.is_empty() && searchable_without_collator;
    searcher.grapheme_segmenter_has_text = false;
    searcher.search_offset = min(offset, length);
    if (!searcher.compares_search_keys)
        searcher.searcher->set_haystack(searcher.text);
}

extern "C" bool unicode_layout_text_searcher_next_match(void* handle, size_t* start, size_t* length)
{
    VERIFY(handle);
    auto& searcher = *static_cast<UnicodeLayoutTextSearcher*>(handle);
    while (true) {
        auto match = searcher.compares_search_keys ? next_search_key_match(searcher) : searcher.searcher->find_from(searcher.search_offset);
        if (!match.has_value())
            return false;

        auto match_length = match->end - match->start;
        auto matched_text = searcher.text.substring_view(match->start, match_length);
        // A collator at base sensitivity ignores the size and voicing of kana letters.
        if (searcher.decomposed_kana_needle.has_value()
            && !kana_letters_agree(*searcher.decomposed_kana_needle, decompose_kana(matched_text))) {
            // A match that agrees can overlap the one that does not.
            searcher.search_offset = match->start + AK::UnicodeUtils::code_unit_length_for_code_point(searcher.text.code_point_at(match->start));
            continue;
        }

        // The collator can end a match inside a grapheme cluster.
        if (!searcher.compares_search_keys)
            match->end = end_of_grapheme_cluster(searcher, match->end);

        searcher.search_offset = match->end;
        *start = match->start;
        *length = match->end - match->start;
        return true;
    }
}

extern "C" void unicode_layout_text_searcher_destroy(void* handle)
{
    VERIFY(handle);
    delete static_cast<UnicodeLayoutTextSearcher*>(handle);
}
