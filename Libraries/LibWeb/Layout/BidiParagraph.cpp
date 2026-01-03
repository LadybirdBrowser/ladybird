/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BidiParagraph.h>

namespace Web::Layout {

static bool is_strong_ltr(Unicode::BidiClass bc)
{
    return bc == Unicode::BidiClass::LeftToRight;
}

static bool is_strong_rtl(Unicode::BidiClass bc)
{
    return bc == Unicode::BidiClass::RightToLeft || bc == Unicode::BidiClass::RightToLeftArabic;
}

static bool is_neutral(Unicode::BidiClass bc)
{
    return bc == Unicode::BidiClass::OtherNeutral || bc == Unicode::BidiClass::WhiteSpaceNeutral || bc == Unicode::BidiClass::SegmentSeparator || bc == Unicode::BidiClass::BlockSeparator;
}

BidiParagraph::BidiParagraph(CSS::Direction paragraph_direction, CSS::UnicodeBidi unicode_bidi)
    : m_paragraph_direction(paragraph_direction)
    , m_paragraph_unicode_bidi(unicode_bidi)
{
    m_paragraph_embedding_level = (paragraph_direction == CSS::Direction::Rtl) ? 1 : 0;
}

void BidiParagraph::add_fragment(size_t fragment_index, Utf16View text, CSS::Direction direction, CSS::UnicodeBidi unicode_bidi)
{
    BidiRun run;
    run.fragment_index = fragment_index;
    run.original_class = (m_paragraph_direction == CSS::Direction::Rtl)
        ? Unicode::BidiClass::RightToLeft
        : Unicode::BidiClass::LeftToRight;

    for (size_t i = 0; i < text.length_in_code_units();) {
        auto code_point = text.code_point_at(i);
        auto bc = Unicode::bidirectional_class(code_point);

        if (is_strong_ltr(bc) || is_strong_rtl(bc)) {
            run.original_class = bc;
            break;
        }
        i += (code_point > 0xFFFF) ? 2 : 1;
    }

    run.resolved_class = run.original_class;

    // NOTE: For text fragments, unicode-bidi: isolate should NOT create isolate initiators.
    // The isolate boundary is at the element level, not the fragment level.
    // Text fragments keep their intrinsic bidi class (AL, R, L) from the actual text content.
    // Only Embed, BidiOverride, IsolateOverride, and Plaintext should override the intrinsic class.
    if (unicode_bidi == CSS::UnicodeBidi::Embed) {
        run.original_class = (direction == CSS::Direction::Ltr)
            ? Unicode::BidiClass::LeftToRightEmbedding
            : Unicode::BidiClass::RightToLeftEmbedding;
    } else if (unicode_bidi == CSS::UnicodeBidi::BidiOverride) {
        run.original_class = (direction == CSS::Direction::Ltr)
            ? Unicode::BidiClass::LeftToRightOverride
            : Unicode::BidiClass::RightToLeftOverride;
    } else if (unicode_bidi == CSS::UnicodeBidi::IsolateOverride) {
        run.original_class = Unicode::BidiClass::FirstStrongIsolate;
        run.is_isolate_initiator = true;
    } else if (unicode_bidi == CSS::UnicodeBidi::Plaintext) {
        run.original_class = Unicode::BidiClass::FirstStrongIsolate;
        run.is_isolate_initiator = true;
    }

    m_fragment_to_run.set(fragment_index, m_runs.size());
    m_runs.append(move(run));
}

void BidiParagraph::add_atomic_inline(size_t fragment_index, CSS::Direction direction, CSS::UnicodeBidi unicode_bidi)
{
    BidiRun run;
    run.fragment_index = fragment_index;
    run.original_class = Unicode::BidiClass::OtherNeutral;
    run.resolved_class = run.original_class;

    if (unicode_bidi == CSS::UnicodeBidi::Embed) {
        run.original_class = (direction == CSS::Direction::Ltr)
            ? Unicode::BidiClass::LeftToRightEmbedding
            : Unicode::BidiClass::RightToLeftEmbedding;
    } else if (unicode_bidi == CSS::UnicodeBidi::Isolate) {
        run.original_class = (direction == CSS::Direction::Ltr)
            ? Unicode::BidiClass::LeftToRightIsolate
            : Unicode::BidiClass::RightToLeftIsolate;
        run.is_isolate_initiator = true;
    }

    m_fragment_to_run.set(fragment_index, m_runs.size());
    m_runs.append(move(run));
}

void BidiParagraph::resolve_levels()
{
    if (m_runs.is_empty())
        return;

    resolve_explicit_embedding_levels();
    resolve_weak_types();
    resolve_neutral_types();
    resolve_implicit_levels();
    reset_levels_for_line_end_whitespace();
}

u8 BidiParagraph::determine_paragraph_level() const
{
    if (m_paragraph_unicode_bidi == CSS::UnicodeBidi::Plaintext) {
        for (auto const& run : m_runs) {
            if (is_strong_ltr(run.original_class))
                return 0;
            if (is_strong_rtl(run.original_class))
                return 1;
        }
    }
    return (m_paragraph_direction == CSS::Direction::Rtl) ? 1 : 0;
}

void BidiParagraph::resolve_explicit_embedding_levels()
{
    m_directional_status_stack.clear();
    m_directional_status_stack.append({
        .embedding_level = m_paragraph_embedding_level,
        .direction = m_paragraph_direction,
        .is_override = false,
        .is_isolate = false,
    });

    u8 overflow_isolate_count = 0;
    u8 overflow_embedding_count = 0;
    u8 valid_isolate_count = 0;

    for (auto& run : m_runs) {
        auto const& current_status = m_directional_status_stack.last();
        auto bc = run.original_class;

        auto compute_next_odd_level = [](u8 level) -> u8 {
            return (level + 1) | 1;
        };
        auto compute_next_even_level = [](u8 level) -> u8 {
            return (level + 2) & ~1;
        };

        switch (bc) {
        case Unicode::BidiClass::RightToLeftEmbedding: {
            auto new_level = compute_next_odd_level(current_status.embedding_level);
            if (new_level <= MAX_DEPTH && overflow_isolate_count == 0 && overflow_embedding_count == 0) {
                m_directional_status_stack.append({
                    .embedding_level = new_level,
                    .direction = CSS::Direction::Rtl,
                    .is_override = false,
                    .is_isolate = false,
                });
            } else if (overflow_isolate_count == 0) {
                ++overflow_embedding_count;
            }
            run.embedding_level = current_status.embedding_level;
            break;
        }

        case Unicode::BidiClass::LeftToRightEmbedding: {
            auto new_level = compute_next_even_level(current_status.embedding_level);
            if (new_level <= MAX_DEPTH && overflow_isolate_count == 0 && overflow_embedding_count == 0) {
                m_directional_status_stack.append({
                    .embedding_level = new_level,
                    .direction = CSS::Direction::Ltr,
                    .is_override = false,
                    .is_isolate = false,
                });
            } else if (overflow_isolate_count == 0) {
                ++overflow_embedding_count;
            }
            run.embedding_level = current_status.embedding_level;
            break;
        }

        case Unicode::BidiClass::RightToLeftOverride: {
            auto new_level = compute_next_odd_level(current_status.embedding_level);
            if (new_level <= MAX_DEPTH && overflow_isolate_count == 0 && overflow_embedding_count == 0) {
                m_directional_status_stack.append({
                    .embedding_level = new_level,
                    .direction = CSS::Direction::Rtl,
                    .is_override = true,
                    .is_isolate = false,
                });
            } else if (overflow_isolate_count == 0) {
                ++overflow_embedding_count;
            }
            run.embedding_level = current_status.embedding_level;
            break;
        }

        case Unicode::BidiClass::LeftToRightOverride: {
            auto new_level = compute_next_even_level(current_status.embedding_level);
            if (new_level <= MAX_DEPTH && overflow_isolate_count == 0 && overflow_embedding_count == 0) {
                m_directional_status_stack.append({
                    .embedding_level = new_level,
                    .direction = CSS::Direction::Ltr,
                    .is_override = true,
                    .is_isolate = false,
                });
            } else if (overflow_isolate_count == 0) {
                ++overflow_embedding_count;
            }
            run.embedding_level = current_status.embedding_level;
            break;
        }

        case Unicode::BidiClass::RightToLeftIsolate: {
            run.embedding_level = current_status.embedding_level;
            if (current_status.is_override)
                run.resolved_class = (current_status.direction == CSS::Direction::Ltr)
                    ? Unicode::BidiClass::LeftToRight
                    : Unicode::BidiClass::RightToLeft;

            auto new_level = compute_next_odd_level(current_status.embedding_level);
            if (new_level <= MAX_DEPTH && overflow_isolate_count == 0 && overflow_embedding_count == 0) {
                ++valid_isolate_count;
                m_directional_status_stack.append({
                    .embedding_level = new_level,
                    .direction = CSS::Direction::Rtl,
                    .is_override = false,
                    .is_isolate = true,
                });
            } else {
                ++overflow_isolate_count;
            }
            break;
        }

        case Unicode::BidiClass::LeftToRightIsolate: {
            run.embedding_level = current_status.embedding_level;
            if (current_status.is_override)
                run.resolved_class = (current_status.direction == CSS::Direction::Ltr)
                    ? Unicode::BidiClass::LeftToRight
                    : Unicode::BidiClass::RightToLeft;

            auto new_level = compute_next_even_level(current_status.embedding_level);
            if (new_level <= MAX_DEPTH && overflow_isolate_count == 0 && overflow_embedding_count == 0) {
                ++valid_isolate_count;
                m_directional_status_stack.append({
                    .embedding_level = new_level,
                    .direction = CSS::Direction::Ltr,
                    .is_override = false,
                    .is_isolate = true,
                });
            } else {
                ++overflow_isolate_count;
            }
            break;
        }

        case Unicode::BidiClass::FirstStrongIsolate: {
            run.embedding_level = current_status.embedding_level;
            if (current_status.is_override)
                run.resolved_class = (current_status.direction == CSS::Direction::Ltr)
                    ? Unicode::BidiClass::LeftToRight
                    : Unicode::BidiClass::RightToLeft;

            u8 new_level;
            CSS::Direction new_direction;
            bool found_strong = false;
            size_t current_index = static_cast<size_t>(&run - m_runs.data());
            for (size_t j = current_index + 1; j < m_runs.size(); ++j) {
                if (is_strong_ltr(m_runs[j].original_class)) {
                    new_level = compute_next_even_level(current_status.embedding_level);
                    new_direction = CSS::Direction::Ltr;
                    found_strong = true;
                    break;
                }
                if (is_strong_rtl(m_runs[j].original_class)) {
                    new_level = compute_next_odd_level(current_status.embedding_level);
                    new_direction = CSS::Direction::Rtl;
                    found_strong = true;
                    break;
                }
            }
            if (!found_strong) {
                new_level = compute_next_even_level(current_status.embedding_level);
                new_direction = CSS::Direction::Ltr;
            }

            if (new_level <= MAX_DEPTH && overflow_isolate_count == 0 && overflow_embedding_count == 0) {
                ++valid_isolate_count;
                m_directional_status_stack.append({
                    .embedding_level = new_level,
                    .direction = new_direction,
                    .is_override = false,
                    .is_isolate = true,
                });
            } else {
                ++overflow_isolate_count;
            }
            break;
        }

        case Unicode::BidiClass::PopDirectionalFormat: {
            if (overflow_isolate_count > 0) {
            } else if (overflow_embedding_count > 0) {
                --overflow_embedding_count;
            } else if (!current_status.is_isolate && m_directional_status_stack.size() >= 2) {
                m_directional_status_stack.take_last();
            }
            run.embedding_level = m_directional_status_stack.last().embedding_level;
            break;
        }

        case Unicode::BidiClass::PopDirectionalIsolate: {
            run.is_isolate_terminator = true;
            if (overflow_isolate_count > 0) {
                --overflow_isolate_count;
            } else if (valid_isolate_count > 0) {
                overflow_embedding_count = 0;
                while (m_directional_status_stack.size() > 1 && !m_directional_status_stack.last().is_isolate) {
                    m_directional_status_stack.take_last();
                }
                if (m_directional_status_stack.size() > 1) {
                    m_directional_status_stack.take_last();
                }
                --valid_isolate_count;
            }
            run.embedding_level = m_directional_status_stack.last().embedding_level;
            if (m_directional_status_stack.last().is_override)
                run.resolved_class = (m_directional_status_stack.last().direction == CSS::Direction::Ltr)
                    ? Unicode::BidiClass::LeftToRight
                    : Unicode::BidiClass::RightToLeft;
            break;
        }

        case Unicode::BidiClass::BoundaryNeutral:
            run.embedding_level = current_status.embedding_level;
            break;

        default:
            run.embedding_level = current_status.embedding_level;
            if (current_status.is_override) {
                run.resolved_class = (current_status.direction == CSS::Direction::Ltr)
                    ? Unicode::BidiClass::LeftToRight
                    : Unicode::BidiClass::RightToLeft;
            }
            break;
        }
    }
}

void BidiParagraph::resolve_weak_types()
{
    Optional<Unicode::BidiClass> prev_strong_class;
    for (size_t i = 0; i < m_runs.size(); ++i) {
        auto& run = m_runs[i];

        if (is_strong_ltr(run.resolved_class) || is_strong_rtl(run.resolved_class)) {
            prev_strong_class = run.resolved_class;
            continue;
        }

        if (run.resolved_class == Unicode::BidiClass::DirNonSpacingMark) {
            if (i > 0) {
                run.resolved_class = m_runs[i - 1].resolved_class;
            } else {
                run.resolved_class = (run.embedding_level % 2 == 0)
                    ? Unicode::BidiClass::LeftToRight
                    : Unicode::BidiClass::RightToLeft;
            }
        }

        if (run.resolved_class == Unicode::BidiClass::EuropeanNumber) {
            if (prev_strong_class.has_value() && prev_strong_class.value() == Unicode::BidiClass::RightToLeftArabic) {
                run.resolved_class = Unicode::BidiClass::ArabicNumber;
            }
        }

        if (run.resolved_class == Unicode::BidiClass::EuropeanNumberSeparator || run.resolved_class == Unicode::BidiClass::CommonNumberSeparator) {
            bool is_between_numbers = false;
            if (i > 0 && i + 1 < m_runs.size()) {
                auto prev_class = m_runs[i - 1].resolved_class;
                auto next_class = m_runs[i + 1].resolved_class;
                if ((prev_class == Unicode::BidiClass::EuropeanNumber && next_class == Unicode::BidiClass::EuropeanNumber) || (prev_class == Unicode::BidiClass::ArabicNumber && next_class == Unicode::BidiClass::ArabicNumber && run.resolved_class == Unicode::BidiClass::CommonNumberSeparator)) {
                    is_between_numbers = true;
                    run.resolved_class = prev_class;
                }
            }
            if (!is_between_numbers) {
                run.resolved_class = Unicode::BidiClass::OtherNeutral;
            }
        }

        if (run.resolved_class == Unicode::BidiClass::EuropeanNumberTerminator) {
            bool adjacent_to_en = false;
            if (i > 0 && m_runs[i - 1].resolved_class == Unicode::BidiClass::EuropeanNumber) {
                adjacent_to_en = true;
            } else if (i + 1 < m_runs.size() && m_runs[i + 1].resolved_class == Unicode::BidiClass::EuropeanNumber) {
                adjacent_to_en = true;
            }
            if (adjacent_to_en) {
                run.resolved_class = Unicode::BidiClass::EuropeanNumber;
            } else {
                run.resolved_class = Unicode::BidiClass::OtherNeutral;
            }
        }
    }

    if (prev_strong_class.has_value() && prev_strong_class.value() == Unicode::BidiClass::LeftToRight) {
        for (auto& run : m_runs) {
            if (run.resolved_class == Unicode::BidiClass::EuropeanNumber) {
                run.resolved_class = Unicode::BidiClass::LeftToRight;
            }
        }
    }
}

void BidiParagraph::resolve_neutral_types()
{
    for (size_t i = 0; i < m_runs.size(); ++i) {
        auto& run = m_runs[i];

        if (!is_neutral(run.resolved_class))
            continue;

        Optional<Unicode::BidiClass> prev_strong;
        for (ssize_t j = static_cast<ssize_t>(i) - 1; j >= 0; --j) {
            if (is_strong_ltr(m_runs[j].resolved_class) || is_strong_rtl(m_runs[j].resolved_class) || m_runs[j].resolved_class == Unicode::BidiClass::EuropeanNumber || m_runs[j].resolved_class == Unicode::BidiClass::ArabicNumber) {
                prev_strong = m_runs[j].resolved_class;
                break;
            }
        }

        Optional<Unicode::BidiClass> next_strong;
        for (size_t j = i + 1; j < m_runs.size(); ++j) {
            if (is_strong_ltr(m_runs[j].resolved_class) || is_strong_rtl(m_runs[j].resolved_class) || m_runs[j].resolved_class == Unicode::BidiClass::EuropeanNumber || m_runs[j].resolved_class == Unicode::BidiClass::ArabicNumber) {
                next_strong = m_runs[j].resolved_class;
                break;
            }
        }

        auto effective_prev = prev_strong.value_or(
            (run.embedding_level % 2 == 0) ? Unicode::BidiClass::LeftToRight : Unicode::BidiClass::RightToLeft);
        auto effective_next = next_strong.value_or(
            (run.embedding_level % 2 == 0) ? Unicode::BidiClass::LeftToRight : Unicode::BidiClass::RightToLeft);

        bool prev_is_ltr = is_strong_ltr(effective_prev) || effective_prev == Unicode::BidiClass::EuropeanNumber;
        bool next_is_ltr = is_strong_ltr(effective_next) || effective_next == Unicode::BidiClass::EuropeanNumber;
        bool prev_is_rtl = is_strong_rtl(effective_prev) || effective_prev == Unicode::BidiClass::ArabicNumber;
        bool next_is_rtl = is_strong_rtl(effective_next) || effective_next == Unicode::BidiClass::ArabicNumber;

        if (prev_is_ltr && next_is_ltr) {
            run.resolved_class = Unicode::BidiClass::LeftToRight;
        } else if (prev_is_rtl && next_is_rtl) {
            run.resolved_class = Unicode::BidiClass::RightToLeft;
        } else {
            run.resolved_class = (run.embedding_level % 2 == 0)
                ? Unicode::BidiClass::LeftToRight
                : Unicode::BidiClass::RightToLeft;
        }
    }
}

void BidiParagraph::resolve_implicit_levels()
{
    for (auto& run : m_runs) {
        if (run.embedding_level % 2 == 0) {
            if (run.resolved_class == Unicode::BidiClass::RightToLeft) {
                run.embedding_level += 1;
            } else if (run.resolved_class == Unicode::BidiClass::ArabicNumber || run.resolved_class == Unicode::BidiClass::EuropeanNumber) {
                run.embedding_level += 2;
            }
        } else {
            if (run.resolved_class == Unicode::BidiClass::LeftToRight || run.resolved_class == Unicode::BidiClass::EuropeanNumber || run.resolved_class == Unicode::BidiClass::ArabicNumber) {
                run.embedding_level += 1;
            }
        }
    }
}

void BidiParagraph::reset_levels_for_line_end_whitespace()
{
    // https://www.unicode.org/reports/tr9/#L1
    // On each line, reset the embedding level of the following characters to the paragraph embedding level:
    // 1. Segment separators,
    // 2. Paragraph separators,
    // 3. Any sequence of whitespace characters and/or isolate formatting characters preceding a segment separator or paragraph separator, and
    // 4. Any sequence of whitespace characters and/or isolate formatting characters at the end of the line.

    if (m_runs.is_empty())
        return;

    // For a paragraph (which is treated as a single line), scan from the end
    // and reset levels of trailing whitespace/separators to the paragraph level
    ssize_t i = static_cast<ssize_t>(m_runs.size()) - 1;

    while (i >= 0) {
        auto bc = m_runs[i].resolved_class;

        // Check if this is whitespace, separator, or isolate formatting character
        bool is_resettable = (bc == Unicode::BidiClass::WhiteSpaceNeutral
            || bc == Unicode::BidiClass::SegmentSeparator
            || bc == Unicode::BidiClass::BlockSeparator
            || bc == Unicode::BidiClass::LeftToRightIsolate
            || bc == Unicode::BidiClass::RightToLeftIsolate
            || bc == Unicode::BidiClass::FirstStrongIsolate
            || bc == Unicode::BidiClass::PopDirectionalIsolate);

        if (is_resettable) {
            m_runs[i].embedding_level = m_paragraph_embedding_level;
            --i;
        } else {
            break;
        }
    }
}

Vector<size_t> BidiParagraph::reordered_fragment_indices() const
{
    return reorder_runs();
}

Vector<size_t> BidiParagraph::reorder_runs() const
{
    if (m_runs.is_empty())
        return {};

    u8 max_level = m_paragraph_embedding_level;
    for (auto const& run : m_runs) {
        max_level = max(max_level, run.embedding_level);
    }

    Vector<size_t> run_order;
    run_order.ensure_capacity(m_runs.size());
    for (size_t i = 0; i < m_runs.size(); ++i) {
        run_order.unchecked_append(i);
    }

    // https://www.unicode.org/reports/tr9/#L2
    // From the highest level found in the text to the lowest odd level on each line, including intermediate levels not
    // actually present in the text, reverse any contiguous sequence of characters that are at that level or higher.
    for (u8 level = max_level; level >= 1; --level) {
        size_t run_index = 0;
        while (run_index < run_order.size()) {
            if (m_runs[run_order[run_index]].embedding_level >= level) {
                size_t segment_start = run_index;
                while (run_index < run_order.size() && m_runs[run_order[run_index]].embedding_level >= level) {
                    ++run_index;
                }
                size_t segment_end = run_index;

                for (size_t offset = 0; offset < (segment_end - segment_start) / 2; ++offset) {
                    swap(run_order[segment_start + offset], run_order[segment_end - 1 - offset]);
                }
            } else {
                ++run_index;
            }
        }
    }

    Vector<size_t> result;
    result.ensure_capacity(run_order.size());
    for (auto run_index : run_order) {
        result.unchecked_append(m_runs[run_index].fragment_index);
    }

    return result;
}

void BidiParagraph::dump_runs() const
{
    dbgln("[BIDI] Runs after resolve_levels() - paragraph_level={}:", m_paragraph_embedding_level);
    for (size_t i = 0; i < m_runs.size(); ++i) {
        auto const& run = m_runs[i];
        dbgln("[BIDI]   Run[{}]: frag_idx={}, level={}, orig_class={}, resolved_class={}",
            i, run.fragment_index, run.embedding_level,
            Unicode::bidi_class_to_string_view(run.original_class), Unicode::bidi_class_to_string_view(run.resolved_class));
    }
}

}
