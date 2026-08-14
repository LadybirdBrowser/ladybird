/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/StringBuilder.h>
#include <AK/UnicodeUtils.h>
#include <AK/Utf16StringBuilder.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Collator.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/TextDirective.h>
#include <LibWeb/HTML/TextDirectiveGenerator.h>
#include <LibWeb/Layout/SearchableText.h>
#include <LibWeb/Layout/Viewport.h>

namespace Web::HTML {

namespace {

struct SearchPosition {
    size_t block_index { 0 };
    size_t offset { 0 };

    bool operator==(SearchPosition const&) const = default;
};

struct SelectedTextRange {
    SearchPosition start;
    SearchPosition end;
};

struct WordSpan {
    size_t start { 0 };
    size_t end { 0 };
};

static bool position_is_before(SearchPosition const& a, SearchPosition const& b)
{
    return a.block_index < b.block_index || (a.block_index == b.block_index && a.offset < b.offset);
}

static Optional<SelectedTextRange> map_range_to_searchable_text(DOM::Range const& range, Layout::SearchableText const& searchable_text)
{
    Optional<SearchPosition> first_included_position;
    Optional<SearchPosition> last_included_position;

    for (size_t block_index = 0; block_index < searchable_text.blocks().size(); ++block_index) {
        auto const& block = searchable_text.blocks()[block_index];
        for (size_t segment_index = 0; segment_index < block.segments.size(); ++segment_index) {
            auto const& segment = block.segments[segment_index];
            auto node = segment.dom_node.ptr();
            if (!node || &node->root() != &range.start_container()->root())
                continue;

            auto const segment_end = segment_index + 1 < block.segments.size()
                ? block.segments[segment_index + 1].start_offset
                : block.text.length_in_code_units();
            for (size_t offset = segment.start_offset; offset < segment_end; ++offset) {
                if (offset >= block.offset_mapping.size())
                    continue;
                auto const& rendered_offset = block.offset_mapping[offset];
                if (rendered_offset.dom_start_offset > rendered_offset.dom_end_offset || rendered_offset.dom_end_offset > node->length())
                    continue;

                DOM::BoundaryPoint character_start { *node, static_cast<WebIDL::UnsignedLong>(rendered_offset.dom_start_offset) };
                DOM::BoundaryPoint character_end { *node, static_cast<WebIDL::UnsignedLong>(rendered_offset.dom_end_offset) };
                auto character_end_relative_to_selection_start = DOM::position_of_boundary_point_relative_to_other_boundary_point(character_end, range.start());
                if (character_end_relative_to_selection_start != DOM::RelativeBoundaryPointPosition::After)
                    continue;

                auto character_start_relative_to_selection_end = DOM::position_of_boundary_point_relative_to_other_boundary_point(character_start, range.end());
                if (character_start_relative_to_selection_end != DOM::RelativeBoundaryPointPosition::Before)
                    continue;

                if (!first_included_position.has_value())
                    first_included_position = SearchPosition { block_index, offset };
                last_included_position = SearchPosition { block_index, offset + 1 };
            }
        }
    }

    if (!first_included_position.has_value() || !last_included_position.has_value())
        return {};
    return SelectedTextRange { *first_included_position, *last_included_position };
}

static Utf16String locale_for_position(Layout::SearchableText const& searchable_text, SearchPosition position, bool is_end)
{
    auto boundary = searchable_text.boundary_point_at_offset(searchable_text.blocks()[position.block_index], position.offset, is_end);
    if (boundary.has_value()) {
        if (auto* element = boundary->node->parent_or_shadow_host_element()) {
            if (auto language = element->lang(); language.has_value())
                return language.release_value();
        }
    }
    return {};
}

static Vector<WordSpan> word_spans(Utf16View text, Utf16View locale)
{
    Vector<WordSpan> words;
    auto segmenter = Unicode::Segmenter::create(locale, Unicode::SegmenterGranularity::Word);
    size_t previous_boundary = 0;
    segmenter->for_each_boundary(text, [&](size_t boundary) {
        if (boundary > previous_boundary && segmenter->is_current_boundary_word_like())
            words.append({ previous_boundary, boundary });
        previous_boundary = boundary;
        return IterationDecision::Continue;
    });
    return words;
}

static void trim_whitespace(SelectedTextRange& range, Layout::SearchableText const& searchable_text)
{
    while (position_is_before(range.start, range.end)) {
        auto const& block = searchable_text.blocks()[range.start.block_index];
        if (range.start.offset >= block.text.length_in_code_units()) {
            ++range.start.block_index;
            range.start.offset = 0;
            continue;
        }
        auto code_point = block.text.code_point_at(range.start.offset);
        if (!Unicode::code_point_has_white_space_property(code_point))
            break;
        range.start.offset += code_point > 0xffff ? 2 : 1;
    }

    while (position_is_before(range.start, range.end)) {
        auto const& block = searchable_text.blocks()[range.end.block_index];
        if (range.end.offset == 0) {
            --range.end.block_index;
            range.end.offset = searchable_text.blocks()[range.end.block_index].text.length_in_code_units();
            continue;
        }
        auto previous_offset = range.end.offset - 1;
        if (previous_offset > 0 && AK::UnicodeUtils::is_utf16_low_surrogate(block.text.code_unit_at(previous_offset))
            && AK::UnicodeUtils::is_utf16_high_surrogate(block.text.code_unit_at(previous_offset - 1)))
            --previous_offset;
        auto code_point = block.text.code_point_at(previous_offset);
        if (!Unicode::code_point_has_white_space_property(code_point))
            break;
        range.end.offset = previous_offset;
    }
}

static void expand_to_word_boundaries(SelectedTextRange& range, Layout::SearchableText const& searchable_text)
{
    auto const& start_block = searchable_text.blocks()[range.start.block_index];
    auto start_segmenter = Unicode::Segmenter::create(locale_for_position(searchable_text, range.start, false), Unicode::SegmenterGranularity::Word);
    start_segmenter->set_segmented_text(start_block.text);
    if (!start_segmenter->is_boundary(range.start.offset))
        range.start.offset = start_segmenter->previous_boundary(range.start.offset, Unicode::Segmenter::Inclusive::Yes).value_or(0);

    auto const& end_block = searchable_text.blocks()[range.end.block_index];
    auto end_segmenter = Unicode::Segmenter::create(locale_for_position(searchable_text, range.end, true), Unicode::SegmenterGranularity::Word);
    end_segmenter->set_segmented_text(end_block.text);
    if (!end_segmenter->is_boundary(range.end.offset))
        range.end.offset = end_segmenter->next_boundary(range.end.offset, Unicode::Segmenter::Inclusive::Yes).value_or(end_block.text.length_in_code_units());
}

static Optional<GC::Ref<DOM::Range>> dom_range_for_positions(Layout::SearchableText const& searchable_text, SelectedTextRange const& range)
{
    auto start = searchable_text.boundary_point_at_offset(searchable_text.blocks()[range.start.block_index], range.start.offset, false);
    auto end = searchable_text.boundary_point_at_offset(searchable_text.blocks()[range.end.block_index], range.end.offset, true);
    if (!start.has_value() || !end.has_value() || &start->node->root() != &end->node->root())
        return {};
    return DOM::Range::create(start->node, start->offset, end->node, end->offset);
}

static Utf16String text_for_positions(Layout::SearchableText const& searchable_text, SearchPosition start, SearchPosition end)
{
    Utf16StringBuilder builder;
    for (size_t block_index = start.block_index; block_index <= end.block_index; ++block_index) {
        auto const text = searchable_text.blocks()[block_index].text.utf16_view();
        auto const block_start = block_index == start.block_index ? start.offset : 0;
        auto const block_end = block_index == end.block_index ? end.offset : text.length_in_code_units();
        if (block_end > block_start)
            builder.append(text.substring_view(block_start, block_end - block_start));
        if (block_index != end.block_index)
            builder.append_ascii(' ');
    }
    return builder.to_string();
}

static String encode_term(Utf16View term)
{
    auto encoded = URL::percent_encode(term, URL::PercentEncodeSet::Component);
    return MUST(encoded.replace("-"sv, "%2D"sv, ReplaceMode::All));
}

static String serialize_text_directive(TextDirective const& directive)
{
    StringBuilder builder;
    builder.append("text="sv);
    if (directive.prefix.has_value())
        builder.appendff("{}-,", encode_term(*directive.prefix));
    builder.append(encode_term(directive.start));
    if (directive.end.has_value())
        builder.appendff(",{}", encode_term(*directive.end));
    if (directive.suffix.has_value())
        builder.appendff(",-{}", encode_term(*directive.suffix));
    return MUST(builder.to_string());
}

static bool ranges_equal(DOM::Range const& a, DOM::Range const& b)
{
    return a.start_container() == b.start_container() && a.start_offset() == b.start_offset()
        && a.end_container() == b.end_container() && a.end_offset() == b.end_offset();
}

static size_t matching_range_count(Layout::SearchableText const& searchable_text, TextDirective const& directive, size_t limit)
{
    auto collator = Unicode::Collator::create(
        {}, Unicode::Usage::Search, {}, Unicode::Sensitivity::Base,
        Unicode::CaseFirst::False, false, false);

    Vector<HashMap<Utf16String, NonnullOwnPtr<Unicode::Segmenter>>> segmenters;
    segmenters.resize(searchable_text.blocks().size());
    auto segmenter_for_position = [&](size_t block_index, size_t offset, bool is_end) -> Unicode::Segmenter& {
        auto locale = locale_for_position(searchable_text, { block_index, offset }, is_end);
        return *segmenters[block_index].ensure(locale, [&] {
            auto segmenter = Unicode::Segmenter::create(locale, Unicode::SegmenterGranularity::Word);
            segmenter->set_segmented_text(searchable_text.blocks()[block_index].text);
            return segmenter;
        });
    };

    auto is_word_boundary = [&](size_t block_index, size_t offset, bool is_end) {
        auto const length = searchable_text.blocks()[block_index].text.length_in_code_units();
        return offset == 0 || offset == length || segmenter_for_position(block_index, offset, is_end).is_boundary(offset);
    };

    auto skip_whitespace_forward = [&](SearchPosition position) {
        while (position.block_index < searchable_text.blocks().size()) {
            auto const& text = searchable_text.blocks()[position.block_index].text;
            while (position.offset < text.length_in_code_units()) {
                auto code_point = text.code_point_at(position.offset);
                if (!Unicode::code_point_has_white_space_property(code_point))
                    return position;
                position.offset += code_point > 0xffff ? 2 : 1;
            }
            if (++position.block_index < searchable_text.blocks().size())
                position.offset = 0;
        }
        return position;
    };

    auto previous_code_point_offset = [](Utf16View text, size_t offset) {
        if (offset == 0)
            return static_cast<size_t>(0);
        --offset;
        if (offset > 0 && AK::UnicodeUtils::is_utf16_low_surrogate(text.code_unit_at(offset))
            && AK::UnicodeUtils::is_utf16_high_surrogate(text.code_unit_at(offset - 1)))
            --offset;
        return offset;
    };

    auto skip_whitespace_backward = [&](SearchPosition position) {
        for (;;) {
            auto const& text = searchable_text.blocks()[position.block_index].text;
            while (position.offset > 0) {
                auto previous = previous_code_point_offset(text, position.offset);
                if (!Unicode::code_point_has_white_space_property(text.code_point_at(previous)))
                    return position;
                position.offset = previous;
            }
            if (position.block_index == 0)
                return position;
            --position.block_index;
            position.offset = searchable_text.blocks()[position.block_index].text.length_in_code_units();
        }
    };

    auto term_matches_at = [&](Utf16View term, SearchPosition position) -> Optional<SearchPosition> {
        if (position.block_index >= searchable_text.blocks().size())
            return {};
        auto const text = searchable_text.blocks()[position.block_index].text.utf16_view();
        auto searcher = collator->create_substring_searcher(text, term);
        auto match = searcher->find_from(position.offset);
        if (!match.has_value() || match->start != position.offset)
            return {};
        return SearchPosition { position.block_index, match->end };
    };

    auto term_matches_ending_at = [&](Utf16View term, SearchPosition end) {
        if (end.block_index >= searchable_text.blocks().size())
            return false;
        auto const text = searchable_text.blocks()[end.block_index].text.utf16_view();
        auto searcher = collator->create_substring_searcher(text, term);
        for (auto match = searcher->find_from(0); match.has_value() && match->start <= end.offset; match = searcher->find_from(match->start + 1)) {
            if (match->end == end.offset && is_word_boundary(end.block_index, match->start, false))
                return true;
        }
        return false;
    };

    auto find_end_term = [&](Utf16View term, SearchPosition start, bool require_word_end, Optional<Utf16String> const& suffix) -> Optional<SelectedTextRange> {
        for (size_t block_index = start.block_index; block_index < searchable_text.blocks().size(); ++block_index) {
            auto const text = searchable_text.blocks()[block_index].text.utf16_view();
            auto searcher = collator->create_substring_searcher(text, term);
            auto search_start = block_index == start.block_index ? start.offset : 0;
            for (auto match = searcher->find_from(search_start); match.has_value(); match = searcher->find_from(match->start + 1)) {
                if (!is_word_boundary(block_index, match->start, false))
                    continue;
                if (require_word_end && !is_word_boundary(block_index, match->end, true))
                    continue;
                if (suffix.has_value()) {
                    auto suffix_start = skip_whitespace_forward({ block_index, match->end });
                    auto suffix_end = term_matches_at(*suffix, suffix_start);
                    if (!suffix_end.has_value() || !is_word_boundary(suffix_end->block_index, suffix_end->offset, true))
                        continue;
                }
                return SelectedTextRange { { block_index, match->start }, { block_index, match->end } };
            }
        }
        return {};
    };

    size_t match_count = 0;
    for (size_t block_index = 0; block_index < searchable_text.blocks().size(); ++block_index) {
        auto const text = searchable_text.blocks()[block_index].text.utf16_view();
        auto start_searcher = collator->create_substring_searcher(text, directive.start);
        for (auto start_match = start_searcher->find_from(0); start_match.has_value(); start_match = start_searcher->find_from(start_match->start + 1)) {
            if (!directive.prefix.has_value() && !is_word_boundary(block_index, start_match->start, false))
                continue;
            if ((directive.end.has_value() || !directive.suffix.has_value()) && !is_word_boundary(block_index, start_match->end, true))
                continue;

            if (directive.prefix.has_value()) {
                auto prefix_end = skip_whitespace_backward({ block_index, start_match->start });
                if (!term_matches_ending_at(*directive.prefix, prefix_end))
                    continue;
            }

            SearchPosition range_end { block_index, start_match->end };
            if (directive.end.has_value()) {
                auto end_match = find_end_term(*directive.end, range_end, !directive.suffix.has_value(), directive.suffix);
                if (!end_match.has_value())
                    continue;
                range_end = end_match->end;
            }

            if (directive.suffix.has_value()) {
                auto suffix_start = skip_whitespace_forward(range_end);
                auto suffix_end = term_matches_at(*directive.suffix, suffix_start);
                if (!suffix_end.has_value() || !is_word_boundary(suffix_end->block_index, suffix_end->offset, true))
                    continue;
            }

            if (++match_count >= limit)
                return match_count;
        }
    }
    return match_count;
}

}

bool selection_is_eligible_for_text_fragment_generation(DOM::Document const& document, DOM::Range const& selection_range)
{
    if (selection_range.collapsed() || !selection_range.start_container()->is_connected()
        || !selection_range.end_container()->is_connected() || document.design_mode_enabled_state())
        return false;

    auto is_in_editable_region = [](DOM::Node const& node) {
        return node.find_in_shadow_including_ancestry([](DOM::Node const& ancestor) {
            return ancestor.is_editable_or_editing_host() || is<FormAssociatedTextControlElement>(ancestor);
        });
    };
    if (is_in_editable_region(selection_range.start_container()) || is_in_editable_region(selection_range.end_container()))
        return false;

    auto contains_editable_content = false;
    selection_range.common_ancestor_container()->for_each_shadow_including_inclusive_descendant([&](DOM::Node& node) {
        if (selection_range.intersects_node(node)
            && (node.is_editable_or_editing_host() || is<FormAssociatedTextControlElement>(node))) {
            contains_editable_content = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return !contains_editable_content;
}

Optional<URL::URL> generate_text_fragment_url(DOM::Document& document, DOM::Range const& selection_range, URL::URL const& current_url)
{
    if (!selection_is_eligible_for_text_fragment_generation(document, selection_range))
        return {};

    document.update_layout(DOM::UpdateLayoutReason::DocumentFindMatchingText);
    Layout::ForcedReadScope read { document };
    auto* viewport = document.layout_node(read);
    if (!viewport)
        return {};
    auto const& searchable_text = viewport->searchable_text();
    auto selected_range = map_range_to_searchable_text(selection_range, searchable_text);
    if (!selected_range.has_value())
        return {};

    trim_whitespace(*selected_range, searchable_text);
    if (!position_is_before(selected_range->start, selected_range->end))
        return {};
    expand_to_word_boundaries(*selected_range, searchable_text);

    auto adjusted_dom_range = dom_range_for_positions(searchable_text, *selected_range);
    if (!adjusted_dom_range.has_value()
        || !selection_is_eligible_for_text_fragment_generation(document, **adjusted_dom_range))
        return {};

    // https://wicg.github.io/scroll-to-text-fragment/#prefer-exact-matching-to-range-based
    // Prefer to specify the entire string where practical. This ensures that if the destination page is removed or
    // changed, the intended destination can still be derived from the URL itself.
    //
    // Text snippets shorter than 300 characters are encouraged to be encoded using an exact match. Above this limit,
    // the UA can encode the string as a range-based match.
    auto selected_text = text_for_positions(searchable_text, selected_range->start, selected_range->end);
    TextDirective directive;
    if (selected_range->start.block_index == selected_range->end.block_index && selected_text.length_in_code_points() <= 300) {
        directive.start = selected_text;
    } else {
        auto const& start_block = searchable_text.blocks()[selected_range->start.block_index];
        auto const& end_block = searchable_text.blocks()[selected_range->end.block_index];
        auto start_words = word_spans(start_block.text, locale_for_position(searchable_text, selected_range->start, false));
        auto end_words = word_spans(end_block.text, locale_for_position(searchable_text, selected_range->end, true));

        Vector<WordSpan> selected_start_words;
        for (auto word : start_words) {
            if (word.end > selected_range->start.offset && word.start < (selected_range->start.block_index == selected_range->end.block_index ? selected_range->end.offset : start_block.text.length_in_code_units()))
                selected_start_words.append(word);
        }
        Vector<WordSpan> selected_end_words;
        for (auto word : end_words) {
            if (word.end > (selected_range->start.block_index == selected_range->end.block_index ? selected_range->start.offset : 0) && word.start < selected_range->end.offset)
                selected_end_words.append(word);
        }
        if (selected_start_words.is_empty() || selected_end_words.is_empty())
            return {};

        auto start_word_count = min(selected_start_words.size(), static_cast<size_t>(3));
        auto end_word_count = min(selected_end_words.size(), static_cast<size_t>(3));
        if (selected_range->start.block_index == selected_range->end.block_index) {
            if (selected_start_words.size() < 2)
                return {};
            start_word_count = min(start_word_count, selected_start_words.size() / 2);
            end_word_count = min(end_word_count, selected_end_words.size() - start_word_count);
        }
        auto start_end = selected_start_words[start_word_count - 1].end;
        auto end_start = selected_end_words[selected_end_words.size() - end_word_count].start;
        directive.start = Utf16String::from_utf16(start_block.text.substring_view(selected_range->start.offset, start_end - selected_range->start.offset));
        directive.end = Utf16String::from_utf16(end_block.text.substring_view(end_start, selected_range->end.offset - end_start));
    }

    auto candidate_matches_selection = [&](TextDirective const& candidate) {
        auto match = find_a_range_from_a_text_directive(candidate, document);
        return match.has_value() && ranges_equal(**match, **adjusted_dom_range)
            && matching_range_count(searchable_text, candidate, 2) == 1;
    };

    // https://wicg.github.io/scroll-to-text-fragment/#use-context-only-when-necessary
    // Where a text snippet is long enough and unique, a UAs are encouraged to avoid adding superfluous context terms.
    //
    // Use context only if one of the following is true:
    // * The UA determines the quoted text is ambiguous
    // * The quoted text contains 3 or fewer words
    auto selected_word_count = static_cast<size_t>(0);
    for (size_t block_index = selected_range->start.block_index; block_index <= selected_range->end.block_index; ++block_index) {
        auto const& block = searchable_text.blocks()[block_index];
        for (auto word : word_spans(block.text, locale_for_position(searchable_text, { block_index, 0 }, false))) {
            auto lower_bound = block_index == selected_range->start.block_index ? selected_range->start.offset : 0;
            auto upper_bound = block_index == selected_range->end.block_index ? selected_range->end.offset : block.text.length_in_code_units();
            if (word.end > lower_bound && word.start < upper_bound)
                ++selected_word_count;
        }
    }

    auto needs_context = selected_word_count <= 3 || !candidate_matches_selection(directive);
    if (needs_context) {
        struct ContextWords {
            size_t block_index { 0 };
            Vector<WordSpan> words;
        };

        auto find_prefix_words = [&]() -> Optional<ContextWords> {
            for (size_t block_index_plus_one = selected_range->start.block_index + 1; block_index_plus_one > 0; --block_index_plus_one) {
                auto const block_index = block_index_plus_one - 1;
                auto const& block = searchable_text.blocks()[block_index];
                auto const upper_bound = block_index == selected_range->start.block_index
                    ? selected_range->start.offset
                    : block.text.length_in_code_units();
                auto words = word_spans(block.text, locale_for_position(searchable_text, { block_index, upper_bound }, true));
                words.remove_all_matching([&](auto const& word) { return word.end > upper_bound; });
                if (!words.is_empty())
                    return ContextWords { block_index, move(words) };
            }
            return {};
        };

        auto find_suffix_words = [&]() -> Optional<ContextWords> {
            for (size_t block_index = selected_range->end.block_index; block_index < searchable_text.blocks().size(); ++block_index) {
                auto const& block = searchable_text.blocks()[block_index];
                auto const lower_bound = block_index == selected_range->end.block_index ? selected_range->end.offset : 0;
                auto words = word_spans(block.text, locale_for_position(searchable_text, { block_index, lower_bound }, false));
                words.remove_all_matching([&](auto const& word) { return word.start < lower_bound; });
                if (!words.is_empty())
                    return ContextWords { block_index, move(words) };
            }
            return {};
        };

        auto prefix_context = find_prefix_words();
        auto suffix_context = find_suffix_words();
        Vector<WordSpan> const empty_words;
        auto const& prefix_words = prefix_context.has_value() ? prefix_context->words : empty_words;
        auto const& suffix_words = suffix_context.has_value() ? suffix_context->words : empty_words;

        auto found_context = false;
        for (size_t context_word_count = 1; context_word_count <= 20 && !found_context; ++context_word_count) {
            if (context_word_count <= 10 && context_word_count <= prefix_words.size()) {
                auto const& prefix_block = searchable_text.blocks()[prefix_context->block_index];
                auto candidate = directive;
                auto start = prefix_words[prefix_words.size() - context_word_count].start;
                auto end = prefix_words.last().end;
                candidate.prefix = Utf16String::from_utf16(prefix_block.text.substring_view(start, end - start));
                if (candidate_matches_selection(candidate)) {
                    directive = move(candidate);
                    found_context = true;
                    break;
                }
            }
            if (context_word_count <= 10 && context_word_count <= suffix_words.size()) {
                auto const& suffix_block = searchable_text.blocks()[suffix_context->block_index];
                auto candidate = directive;
                auto start = suffix_words.first().start;
                auto end = suffix_words[context_word_count - 1].end;
                candidate.suffix = Utf16String::from_utf16(suffix_block.text.substring_view(start, end - start));
                if (candidate_matches_selection(candidate)) {
                    directive = move(candidate);
                    found_context = true;
                    break;
                }
            }

            auto minimum_prefix_word_count = context_word_count > 10 ? context_word_count - 10 : 1;
            auto maximum_prefix_word_count = min(context_word_count - 1, static_cast<size_t>(10));
            for (size_t prefix_word_count = minimum_prefix_word_count; prefix_word_count <= maximum_prefix_word_count; ++prefix_word_count) {
                auto suffix_word_count = context_word_count - prefix_word_count;
                if (prefix_word_count > prefix_words.size() || suffix_word_count > suffix_words.size())
                    continue;

                auto const& prefix_block = searchable_text.blocks()[prefix_context->block_index];
                auto const& suffix_block = searchable_text.blocks()[suffix_context->block_index];
                auto candidate = directive;
                auto prefix_start = prefix_words[prefix_words.size() - prefix_word_count].start;
                auto prefix_end = prefix_words.last().end;
                candidate.prefix = Utf16String::from_utf16(prefix_block.text.substring_view(prefix_start, prefix_end - prefix_start));
                auto suffix_start = suffix_words.first().start;
                auto suffix_end = suffix_words[suffix_word_count - 1].end;
                candidate.suffix = Utf16String::from_utf16(suffix_block.text.substring_view(suffix_start, suffix_end - suffix_start));
                if (candidate_matches_selection(candidate)) {
                    directive = move(candidate);
                    found_context = true;
                    break;
                }
            }
        }
        if (!found_context)
            return {};
    }

    auto generated_directive = serialize_text_directive(directive);

    // Retain the ordinary fragment and unknown directive parameters, but replace existing text directives with the
    // newly generated selection.
    auto result = current_url;
    auto ordinary_fragment = ""sv;
    Vector<StringView> retained_directives;
    if (auto const& fragment = result.fragment(); fragment.has_value()) {
        auto delimiter = fragment->find(":~:"sv);
        if (delimiter.has_value()) {
            ordinary_fragment = fragment->substring_view(0, *delimiter);
            auto directive_part = fragment->substring_view(*delimiter + 3);
            for (auto parameter : directive_part.split_view('&', SplitBehavior::KeepEmpty)) {
                if (!parameter.starts_with("text="sv) && !parameter.is_empty())
                    retained_directives.append(parameter);
            }
        } else {
            ordinary_fragment = *fragment;
        }
    }

    StringBuilder fragment_builder;
    fragment_builder.append(ordinary_fragment);
    fragment_builder.append(":~:"sv);
    for (auto parameter : retained_directives)
        fragment_builder.appendff("{}&", parameter);
    fragment_builder.append(generated_directive);
    auto fragment_string = MUST(fragment_builder.to_string());
    result.set_fragment(fragment_string.bytes_as_string_view());
    return result;
}

}
