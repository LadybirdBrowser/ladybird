/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Variant.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Collator.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/TextDirective.h>
#include <LibWeb/Layout/SearchableText.h>
#include <LibWeb/Layout/Viewport.h>

namespace Web::HTML {

// https://wicg.github.io/scroll-to-text-fragment/#extracting-the-fragment-directive
Optional<String> remove_the_fragment_directive(URL::URL& url)
{
    // 1. Let raw fragment be equal to url's fragment.
    auto const& raw_fragment = url.fragment();

    // 2. Let fragment directive be null.
    Optional<String> fragment_directive;

    // 3. If raw fragment is non-null and contains the fragment directive delimiter as a substring:
    if (raw_fragment.has_value()) {
        // 1. Let position be the string position variable pointing to the first code point of the first instance, if
        //    one exists, of the fragment directive delimiter in raw fragment, or past the end of raw fragment otherwise.
        auto position = raw_fragment->find_byte_offset(":~:"sv);
        if (position.has_value()) {
            // 2. Let new fragment be the code point substring by positions of raw fragment from the start of raw
            //    fragment to position.
            auto new_fragment = MUST(raw_fragment->substring_from_byte_offset_with_shared_superstring(0, *position));

            // 3. Advance position by the code point length of the fragment directive delimiter.
            *position += 3;

            // 4. If position does not point past the end of raw fragment:
            if (*position < raw_fragment->byte_count()) {
                // 1. Set fragment directive to the code point substring to the end of the string raw fragment starting
                //    from position.
                fragment_directive = MUST(raw_fragment->substring_from_byte_offset_with_shared_superstring(*position));
            }

            // 5. Set url's fragment to new fragment.
            url.set_fragment(move(new_fragment));
        }
    }

    // 4. Return fragment directive.
    return fragment_directive;
}

// https://wicg.github.io/scroll-to-text-fragment/#text-directives
Optional<Utf16String> percent_decode_a_text_directive_term(Optional<StringView> term)
{
    // 1. If term is null, return null.
    if (!term.has_value())
        return {};

    // 2. Assert: term is an ASCII string.
    VERIFY(term->is_ascii());

    // 3. Let decoded bytes be the result of percent-decoding term.
    auto decoded_bytes = URL::percent_decode(*term);

    // 4. Return the result of running UTF-8 decode without BOM on decoded bytes.
    return Utf16String::from_utf8_with_replacement_character(decoded_bytes, Utf16String::WithBOMHandling::No);
}

// https://wicg.github.io/scroll-to-text-fragment/#text-directives
Optional<TextDirective> parse_a_text_directive(StringView text_directive_value)
{
    // 1. Let prefix, suffix, start, end, each be null.
    Optional<StringView> prefix;
    Optional<StringView> suffix;
    Optional<StringView> start;
    Optional<StringView> end;

    // 2. Assert: text directive value is an ASCII string with no code points in the fragment percent-encode set and no
    //    instances of U+0026 (&).
    VERIFY(text_directive_value.is_ascii());
    VERIFY(!text_directive_value.contains('&'));

    // 3. Let tokens be a list of strings that result from strictly splitting text directive value on U+002C (,).
    auto tokens = text_directive_value.split_view(',', SplitBehavior::KeepEmpty);

    // 4. If tokens has size less than 1 or greater than 4, return null.
    if (tokens.is_empty() || tokens.size() > 4)
        return {};

    // 5. If the first item of tokens ends with U+002D (-):
    if (tokens.first().ends_with('-')) {
        // 1. Set prefix to the substring of tokens[0] from 0 with length tokens[0]'s length - 1.
        prefix = tokens.first().substring_view(0, tokens.first().length() - 1);

        // 2. Remove the first item of tokens.
        tokens.remove(0);

        // 3. If prefix is the empty string or contains any instances of U+002D (-), return null.
        if (prefix->is_empty() || prefix->contains('-'))
            return {};

        // 4. If tokens is empty, return null.
        if (tokens.is_empty())
            return {};
    }

    // 6. If the last item of tokens starts with U+002D (-):
    if (tokens.last().starts_with('-')) {
        // 1. Set suffix to the substring of the last item of tokens from 1 to the end of the string.
        suffix = tokens.last().substring_view(1);

        // 2. Remove the last item of tokens.
        tokens.take_last();

        // 3. If suffix is the empty string or contains any instances of U+002D (-), return null.
        if (suffix->is_empty() || suffix->contains('-'))
            return {};

        // 4. If tokens is empty, return null.
        if (tokens.is_empty())
            return {};
    }

    // 7. If tokens has size greater than 2, return null.
    if (tokens.size() > 2)
        return {};

    // 8. Assert: tokens has size 1 or 2.
    VERIFY(tokens.size() == 1 || tokens.size() == 2);

    // 9. Set start to the first item in tokens.
    start = tokens.first();

    // 10. Remove the first item in tokens.
    tokens.remove(0);

    // 11. If start is the empty string or contains any instances of U+002D (-), return null.
    if (start->is_empty() || start->contains('-'))
        return {};

    // 12. If tokens is not empty:
    if (!tokens.is_empty()) {
        // 1. Set end to the first item in tokens.
        end = tokens.first();

        // 2. If end is the empty string or contains any instances of U+002D (-), return null.
        if (end->is_empty() || end->contains('-'))
            return {};
    }

    // 13. Return a new text directive, with
    //       prefix: The percent-decoding of prefix
    //       start: The percent-decoding of start
    //       end: The percent-decoding of end
    //       suffix: The percent-decoding of suffix
    return TextDirective {
        .prefix = percent_decode_a_text_directive_term(prefix),
        .start = percent_decode_a_text_directive_term(start).release_value(),
        .end = percent_decode_a_text_directive_term(end),
        .suffix = percent_decode_a_text_directive_term(suffix),
    };
}

// https://wicg.github.io/scroll-to-text-fragment/#text-directives
Vector<TextDirective> parse_the_fragment_directive(StringView fragment_directive)
{
    // AD-HOC: A fragment directive extracted from a URL is always ASCII. Session history may be restored from an
    //         untrusted serialized representation, so reject invalid restored values before parse_a_text_directive()
    //         reaches its specification assertion.
    if (!fragment_directive.is_ascii())
        return {};

    // 1. Let directives be the result of strictly splitting fragment directive on U+0026 (&).
    auto directives = fragment_directive.split_view('&', SplitBehavior::KeepEmpty);

    // 2. Let output be an initially empty list of text directives.
    Vector<TextDirective> output;

    // 3. For each string directive in directives:
    for (auto const& directive : directives) {
        // 1. If directive does not start with "text=", then continue.
        if (!directive.starts_with("text="sv))
            continue;

        // 2. Let text directive value be the code point substring from 5 to the end of directive.
        //    Note: this may be the empty string.
        auto text_directive_value = directive.substring_view(5);

        // 3. Let parsed text directive be the result of parsing text directive value.
        auto parsed_text_directive = parse_a_text_directive(text_directive_value);

        // 4. If parsed text directive is non-null, append it to output.
        if (parsed_text_directive.has_value())
            output.append(parsed_text_directive.release_value());
    }

    // 4. Return output.
    return output;
}

namespace {

class SearchPosition {
public:
    struct InBlock {
        size_t block_index;
        size_t offset;
    };

    static SearchPosition in_block(size_t block_index, size_t offset)
    {
        return InBlock { block_index, offset };
    }

    static SearchPosition in_block(InBlock position)
    {
        return position;
    }

    static SearchPosition end_of_document()
    {
        return EndOfDocument {};
    }

    bool is_end_of_document() const
    {
        return m_position.has<EndOfDocument>();
    }

    Optional<InBlock&> block_position()
    {
        if (!m_position.has<InBlock>())
            return {};
        return m_position.get<InBlock>();
    }

    Optional<InBlock const&> block_position() const
    {
        if (!m_position.has<InBlock>())
            return {};
        return m_position.get<InBlock>();
    }

    int operator<=>(SearchPosition const& other) const
    {
        if (is_end_of_document())
            return other.is_end_of_document() ? 0 : 1;
        if (other.is_end_of_document())
            return -1;

        auto const& this_position = m_position.get<InBlock>();
        auto const& other_position = other.m_position.get<InBlock>();
        if (this_position.block_index < other_position.block_index)
            return -1;
        if (this_position.block_index > other_position.block_index)
            return 1;
        if (this_position.offset < other_position.offset)
            return -1;
        if (this_position.offset > other_position.offset)
            return 1;
        return 0;
    }

private:
    struct EndOfDocument { };

    SearchPosition(InBlock position)
        : m_position(position)
    {
    }

    SearchPosition(EndOfDocument position)
        : m_position(position)
    {
    }

    Variant<InBlock, EndOfDocument> m_position;
};

struct SearchRange {
    Layout::SearchableText const& searchable_text;
    SearchPosition start;
    SearchPosition end;

    bool collapsed() const
    {
        return (start <=> end) >= 0;
    }
};

struct TextMatch {
    SearchPosition::InBlock start;
    SearchPosition::InBlock end;
};

struct CachedSubstringSearcher {
    size_t block_index;
    Utf16String query;
    NonnullOwnPtr<Unicode::Collator::SubstringSearcher> searcher;
};

struct CachedWordSegmenter {
    size_t block_index;
    Utf16String locale;
    NonnullOwnPtr<Unicode::Segmenter> segmenter;
};

struct TextDirectiveSearchContext {
    NonnullOwnPtr<Unicode::Collator> collator;
    Vector<CachedSubstringSearcher> substring_searchers {};
    Vector<CachedWordSegmenter> word_segmenters {};
};

}

// https://wicg.github.io/scroll-to-text-fragment/#word-boundaries
static bool is_at_a_word_boundary(Utf16View text, size_t position, Unicode::Segmenter& segmenter)
{
    // A number position is at a word boundary in a string text, given a locale locale, if, using
    // locale, either a word boundary immediately precedes the positionth code unit, or text's
    // length is more than 0 and position equals either 0 or text's length.
    if (!text.is_empty() && (position == 0 || position == text.length_in_code_units()))
        return true;
    if (position > text.length_in_code_units())
        return false;

    return segmenter.is_boundary(position);
}

// https://wicg.github.io/scroll-to-text-fragment/#get-boundary-point-at-index
static Optional<DOM::BoundaryPoint> get_boundary_point_at_index(size_t index, Layout::SearchableText::Block const& block, bool is_end)
{
    // 1. Let counted be 0.
    size_t counted = 0;

    // 2. For each curNode of nodes:
    for (size_t segment_index = 0; segment_index < block.segments.size(); ++segment_index) {
        auto const& segment = block.segments[segment_index];
        auto node = segment.dom_node.ptr();
        if (!node)
            return {};

        auto const next_segment_offset = segment_index + 1 < block.segments.size()
            ? block.segments[segment_index + 1].start_offset
            : block.text.length_in_code_units();
        auto const segment_length = next_segment_offset - segment.start_offset;

        // 1. Let nodeEnd be counted + curNode's length.
        auto node_end = counted + segment_length;

        // 2. If isEnd is true, add 1 to nodeEnd.
        if (is_end)
            ++node_end;

        // 3. If nodeEnd is greater than index then:
        if (node_end > index) {
            // 1. Return the boundary point (curNode, index - counted).
            // NB: TextNode::compute_text_for_rendering() preserves the DOM range which produced
            //     each rendered UTF-16 code unit. Use that mapping because text transformations can
            //     change the number of code units relative to CharacterData/data.
            auto const mapping_index = is_end && index > 0 ? index - 1 : index;
            if (mapping_index >= block.offset_mapping.size())
                return {};
            auto const& mapping = block.offset_mapping[mapping_index];
            return DOM::BoundaryPoint {
                *node,
                static_cast<WebIDL::UnsignedLong>(is_end && index > 0 ? mapping.dom_end_offset : mapping.dom_start_offset),
            };
        }

        // 4. Increment counted by curNode's length.
        counted += segment_length;
    }

    // 3. Return null.
    return {};
}

// https://wicg.github.io/scroll-to-text-fragment/#finding-ranges-in-a-document
static Optional<TextMatch> find_a_range_from_a_node_list(Utf16View query_string, SearchRange const& search_range, size_t block_index, bool word_start_bounded, bool word_end_bounded, bool match_must_be_at_beginning, TextDirectiveSearchContext& search_context)
{
    auto const& block = search_range.searchable_text.blocks()[block_index];

    // 1. Let searchBuffer be the concatenation of the CharacterData/data of each item in nodes.
    auto search_buffer = block.text.utf16_view();

    // 2. Let searchStart be 0.
    size_t search_start = 0;

    // 3. If the first item in nodes is searchRange's start node then set searchStart to
    //    searchRange's start offset.
    if (auto start_position = search_range.start.block_position(); start_position.has_value() && block_index == start_position->block_index)
        search_start = start_position->offset;

    // 4. Let start and end be boundary points, initially null.
    Optional<DOM::BoundaryPoint> start;
    Optional<DOM::BoundaryPoint> end;

    // 5. Let matchIndex be null.
    Optional<Unicode::Collator::Match> match;

    Unicode::Collator::SubstringSearcher* substring_searcher = nullptr;
    for (auto& cached_searcher : search_context.substring_searchers) {
        if (cached_searcher.block_index == block_index && cached_searcher.query.utf16_view() == query_string) {
            substring_searcher = cached_searcher.searcher.ptr();
            break;
        }
    }
    if (!substring_searcher) {
        search_context.substring_searchers.append({ block_index, Utf16String::from_utf16(query_string), search_context.collator->create_substring_searcher(search_buffer, query_string) });
        substring_searcher = search_context.substring_searchers.last().searcher.ptr();
    }

    auto word_segmenter_for_locale = [&](Utf16String const& locale) -> Unicode::Segmenter& {
        for (auto& cached_segmenter : search_context.word_segmenters) {
            if (cached_segmenter.block_index == block_index && cached_segmenter.locale == locale)
                return *cached_segmenter.segmenter;
        }

        auto segmenter = Unicode::Segmenter::create(locale, Unicode::SegmenterGranularity::Word);
        segmenter->set_segmented_text(search_buffer);
        search_context.word_segmenters.append({ block_index, locale, move(segmenter) });
        return *search_context.word_segmenters.last().segmenter;
    };

    // 6. While matchIndex is null
    while (!match.has_value()) {
        // 1. Set matchIndex to the index of the first instance of queryString in searchBuffer, starting at
        //    searchStart. The string search must be performed using a base character comparison, or the primary level,
        //    as defined in UTS10.
        match = substring_searcher->find_from(search_start);

        // 2. If matchIndex is null, return null.
        if (!match.has_value())
            return {};

        // 3. If matchMustBeAtBeginning is true and matchIndex is not 0, return null.
        // AD-HOC: searchBuffer represents an entire rendered block rather than a freshly sliced node list, so
        //         searchStart is its logical index 0.
        if (match_must_be_at_beginning && match->start != search_start)
            return {};

        // 4. Let endIx be matchIndex + queryString's length.
        // AD-HOC: ICU returns the actual end of a primary-strength match, which may have a different UTF-16 length
        //         from queryString when the strings are canonically equivalent.
        auto end_index = match->end;

        // 5. Set start to the boundary point result of get boundary point at index matchIndex run over nodes with
        //    isEnd false.
        start = get_boundary_point_at_index(match->start, block, false);

        // 6. Set end to the boundary point result of get boundary point at index endIx run over nodes with isEnd true.
        end = get_boundary_point_at_index(end_index, block, true);
        if (!start.has_value() || !end.has_value())
            return {};

        auto locale_for_boundary_point = [](DOM::BoundaryPoint const& boundary_point) {
            if (auto* element = boundary_point.node->parent_or_shadow_host_element()) {
                if (auto language = element->lang(); language.has_value())
                    return language.release_value();
            }
            return Utf16String {};
        };

        // 7. If wordStartBounded is true and matchIndex is not at a word boundary in searchBuffer, given the language
        //    from start's node as the locale; or wordEndBounded is true and endIx is not at a word boundary in
        //    searchBuffer, given the language from end's node as the locale:
        auto start_locale = locale_for_boundary_point(*start);
        auto end_locale = locale_for_boundary_point(*end);
        if ((word_start_bounded && !is_at_a_word_boundary(search_buffer, match->start, word_segmenter_for_locale(start_locale)))
            || (word_end_bounded && !is_at_a_word_boundary(search_buffer, end_index, word_segmenter_for_locale(end_locale)))) {
            // 1. Set searchStart to matchIndex + 1.
            search_start = match->start + 1;

            // 2. Set matchIndex to null.
            match.clear();
        }
    }

    // 7. Let endInset be 0.
    size_t end_inset = 0;

    // 8. If the last item in nodes is searchRange's end node then set endInset to (searchRange's end node's length -
    //    searchRange's end offset).
    if (auto end_position = search_range.end.block_position(); end_position.has_value() && block_index == end_position->block_index) {
        auto const search_buffer_length = search_buffer.length_in_code_units();
        end_inset = end_position->offset < search_buffer_length
            ? search_buffer_length - end_position->offset
            : 0;
    }

    // 9. If matchIndex + queryString's length is greater than searchBuffer's length - endInset return null.
    if (match->end > search_buffer.length_in_code_units() - end_inset)
        return {};

    // 10. Assert: start and end are non-null, valid boundary points in searchRange.
    VERIFY(start.has_value());
    VERIFY(end.has_value());

    // 11. Return a range with start and end.
    return TextMatch {
        .start = { block_index, match->start },
        .end = { block_index, match->end },
    };
}

// https://wicg.github.io/scroll-to-text-fragment/#find-a-string-in-range
static Optional<TextMatch> find_a_string_in_a_range(Utf16View query, SearchRange& search_range, bool word_start_bounded, bool word_end_bounded, bool match_must_be_at_beginning, TextDirectiveSearchContext& search_context)
{
    // 1. While searchRange is not collapsed:
    while (!search_range.collapsed()) {
        // 1. Let curNode be searchRange's start node.
        // NB: Layout::SearchableText::collect() precomputes curNode and the following traversal steps. Each block in
        //     the resulting index represents one textNodeList and records its position in document order.

        // 2. If curNode is part of a non-searchable subtree:
        //     1. Set searchRange's start node to the next node, in shadow-including tree order, that isn't a
        //        shadow-including descendant of curNode.
        //     2. Set searchRange's start offset to 0.
        //     3. Continue.
        // NB: Layout::SearchableText::collect() omits non-searchable subtrees while constructing the index.

        // 3. If curNode is not a visible text node:
        //     1. Set searchRange's start node to the next node, in shadow-including tree order, that is not a doctype.
        //     2. Set searchRange's start offset to 0.
        //     3. Continue.
        // NB: Layout::SearchableText::collect() omits nodes that are not visible text nodes while constructing the
        //     index.

        // 4. Let blockAncestor be the nearest block ancestor of curNode.
        // NB: Layout::SearchableText::collect() uses the nearest block ancestor to divide the index into blocks.

        // 5. Let textNodeList be a list of Text nodes, initially empty.
        // NB: A SearchableText block's segments retain the Text nodes and DOM offsets corresponding to its rendered
        //     text.

        // 6. While curNode is a shadow-including descendant of blockAncestor and the position of the boundary point
        //    (curNode, 0) is not after searchRange's end:
        //     1. If curNode has block-level display then break.
        //     2. If curNode is search invisible:
        //         1. Set curNode to the next node, in shadow-including tree order, that isn't a shadow-including
        //            descendant of curNode.
        //         2. Continue.
        //     3. If curNode is a visible text node then append it to textNodeList.
        //     4. Set curNode to the next node in shadow-including tree order.
        // NB: Layout::SearchableText::collect() performs this loop and stores each completed textNodeList as a block.
        auto const start_position = search_range.start.block_position();
        VERIFY(start_position.has_value());
        auto const block_index = start_position->block_index;

        // 7. Run the find a range from a node list steps given query, searchRange, textNodeList, wordStartBounded,
        //    wordEndBounded and matchMustBeAtBeginning as input. If the resulting range is not null, then return it.
        if (auto match = find_a_range_from_a_node_list(query, search_range, block_index, word_start_bounded, word_end_bounded, match_must_be_at_beginning, search_context); match.has_value())
            return match;

        // 8. If matchMustBeAtBeginning is true, return null.
        if (match_must_be_at_beginning)
            return {};

        // 9. If curNode is null, then break.
        auto const end_position = search_range.end.block_position();
        if ((end_position.has_value() && block_index >= end_position->block_index)
            || block_index + 1 >= search_range.searchable_text.blocks().size())
            break;

        // 10. Assert: curNode follows searchRange's start node.
        // NB: SearchableText blocks are stored in document order, so the next block necessarily follows the current
        //     start block.

        // 11. Set searchRange's start to the boundary point (curNode, 0).
        search_range.start = SearchPosition::in_block(block_index + 1, 0);
    }

    // 2. Return null.
    return {};
}

// https://wicg.github.io/scroll-to-text-fragment/#next-non-whitespace-position
static void advance_a_range_start_to_the_next_non_whitespace_position(SearchRange& range)
{
    // 1. While range is not collapsed:
    while (!range.collapsed()) {
        // 1. Let node be range's start node.
        // NB: The current SearchableText block represents node and its searchable rendered text.
        auto start_position = range.start.block_position();
        VERIFY(start_position.has_value());
        auto const& block = range.searchable_text.blocks()[start_position->block_index];

        // 2. Let offset be range's start offset.
        auto text = block.text.utf16_view();

        // 3. If node is part of a non-searchable subtree or if node is not a visible text node or if offset is equal
        //    to node's length then:
        // NB: Layout::SearchableText::collect() omits non-searchable subtrees and nodes that are not visible text
        //     nodes, so only the offset condition remains to be checked here.
        if (start_position->offset >= text.length_in_code_units()) {
            // 1. Set range's start node to the next node, in shadow-including tree order.
            auto next_position = start_position->block_index + 1 < range.searchable_text.blocks().size()
                ? SearchPosition::in_block(start_position->block_index + 1, 0)
                : SearchPosition::end_of_document();
            if ((next_position <=> range.end) >= 0) {
                range.start = range.end;
                return;
            }

            // 2. Set range's start offset to 0.
            range.start = move(next_position);

            // 3. Continue.
            continue;
        }

        // 4. If the Text/substring data of node at offset and count 6 is equal to the string "&nbsp;" then:
        if (text.substring_view(start_position->offset).starts_with(u"&nbsp;"sv)) {
            // 1. Add 6 to range's start offset.
            start_position->offset += 6;
            continue;
        }

        // 5. Otherwise, if the Text/substring data of node at offset and count 5 is equal to the string "&nbsp"
        //    then:
        if (text.substring_view(start_position->offset).starts_with(u"&nbsp"sv)) {
            // 1. Add 5 to range's start offset.
            start_position->offset += 5;
            continue;
        }

        // 6. Otherwise:
        //     1. Let cp be the code point at the offset index in node's CharacterData/data.
        auto code_point = text.code_point_at(start_position->offset);

        //     2. If cp does not have the White_Space property set, return.
        if (!Unicode::code_point_has_white_space_property(code_point))
            return;

        //     3. Add 1 to range's start offset.
        // NB: SearchRange offsets are UTF-16 code-unit offsets, so advance by two code units for a supplementary code
        //     point.
        start_position->offset += code_point > 0xffff ? 2 : 1;
    }
}

// https://wicg.github.io/scroll-to-text-fragment/#finding-ranges-in-a-document
Optional<GC::Ref<DOM::Range>> find_a_range_from_a_text_directive(TextDirective const& parsed_values, DOM::Document& document)
{
    document.update_layout(DOM::UpdateLayoutReason::DocumentFindMatchingText);
    auto* viewport = document.layout_node();
    if (!viewport)
        return {};

    auto const& searchable_text = viewport->searchable_text();
    if (searchable_text.blocks().is_empty())
        return {};

    // 1. Let searchRange be a range with start (document, 0) and end (document, document's length).
    SearchRange search_range {
        searchable_text,
        SearchPosition::in_block(0, 0),
        SearchPosition::end_of_document(),
    };

    TextDirectiveSearchContext search_context {
        .collator = Unicode::Collator::create(
            {}, Unicode::Usage::Search, {}, Unicode::Sensitivity::Base,
            Unicode::CaseFirst::False, false, false),
    };

    auto first_boundary_point_after = [&](SearchPosition::InBlock position) {
        auto const block_length = searchable_text.blocks()[position.block_index].text.length_in_code_units();
        if (position.offset < block_length)
            return SearchPosition::in_block(position.block_index, position.offset + 1);
        if (position.block_index + 1 < searchable_text.blocks().size())
            return SearchPosition::in_block(position.block_index + 1, 0);
        return search_range.end;
    };

    auto create_range = [&](TextMatch const& match) -> Optional<GC::Ref<DOM::Range>> {
        auto start = get_boundary_point_at_index(match.start.offset, searchable_text.blocks()[match.start.block_index], false);
        auto end = get_boundary_point_at_index(match.end.offset, searchable_text.blocks()[match.end.block_index], true);
        if (!start.has_value() || !end.has_value() || &start->node->root() != &end->node->root())
            return {};
        return DOM::Range::create(start->node, start->offset, end->node, end->offset);
    };

    // 2. While searchRange is not collapsed:
    while (!search_range.collapsed()) {
        // 1. Let potentialMatch be null.
        Optional<TextMatch> potential_match;

        // 2. If parsedValues's prefix is not null:
        if (parsed_values.prefix.has_value()) {
            // 1. Let prefixMatch be the the result of running the find a string in range steps
            //    with query parsedValues's prefix, searchRange searchRange, wordStartBounded true,
            //    wordEndBounded false and matchMustBeAtBeginning false.
            auto prefix_match = find_a_string_in_a_range(*parsed_values.prefix, search_range, true, false, false, search_context);

            // 2. If prefixMatch is null, return null.
            if (!prefix_match.has_value())
                return {};

            // 3. Set searchRange's start to the first boundary point after prefixMatch's start.
            search_range.start = first_boundary_point_after(prefix_match->start);

            // 4. Let matchRange be a range whose start is prefixMatch's end and end is
            //    searchRange's end.
            SearchRange match_range { searchable_text, SearchPosition::in_block(prefix_match->end), search_range.end };

            // 5. Advance matchRange's start to the next non-whitespace position.
            advance_a_range_start_to_the_next_non_whitespace_position(match_range);

            // 6. If matchRange is collapsed return null.
            if (match_range.collapsed())
                return {};

            // 7. Assert: matchRange's start node is a Text node.
            auto const match_range_start = match_range.start.block_position();
            VERIFY(match_range_start.has_value());
            VERIFY(match_range_start->block_index < searchable_text.blocks().size());

            // 8. Let mustEndAtWordBoundary be true if parsedValues's end is non-null or
            //    parsedValues's suffix is null, false otherwise.
            auto must_end_at_word_boundary = parsed_values.end.has_value() || !parsed_values.suffix.has_value();

            // 9. Set potentialMatch to the result of running find a string in range with query
            //    start, matchRange, wordStartBounded false, wordEndBounded mustEndAtWordBoundary and
            //    matchMustBeAtBeginning true.
            potential_match = find_a_string_in_a_range(parsed_values.start, match_range, false, must_end_at_word_boundary, true, search_context);

            // 10. If potentialMatch is null, continue.
            if (!potential_match.has_value())
                continue;
        }

        // 3. Otherwise:
        else {
            // 1. Let mustEndAtWordBoundary be true if parsedValues's end is non-null or
            //    parsedValues's suffix is null, false otherwise.
            auto must_end_at_word_boundary = parsed_values.end.has_value() || !parsed_values.suffix.has_value();

            // 2. Set potentialMatch to the result of running find a string in range with query
            //    start, searchRange, wordStartBounded true, wordEndBounded mustEndAtWordBoundary and
            //    matchMustBeAtBeginning false.
            potential_match = find_a_string_in_a_range(parsed_values.start, search_range, true, must_end_at_word_boundary, false, search_context);

            // 3. If potentialMatch is null, return null.
            if (!potential_match.has_value())
                return {};

            // 4. Set searchRange's start to the first boundary point after potentialMatch's
            //    start.
            search_range.start = first_boundary_point_after(potential_match->start);
        }

        // 4. Let rangeEndSearchRange be a range whose start is potentialMatch's end and whose end
        //    is searchRange's end.
        SearchRange range_end_search_range { searchable_text, SearchPosition::in_block(potential_match->end), search_range.end };

        // If an exact match without suffix context ends at the end of searchRange, the following
        // loop is already collapsed and the range is complete.
        if (!parsed_values.end.has_value() && !parsed_values.suffix.has_value())
            return create_range(*potential_match);

        // 5. While rangeEndSearchRange is not collapsed:
        while (!range_end_search_range.collapsed()) {
            // 1. If parsedValues's end item is non-null, then:
            if (parsed_values.end.has_value()) {
                // 1. Let mustEndAtWordBoundary be true if parsedValues's suffix is null,
                //    false otherwise.
                auto must_end_at_word_boundary = !parsed_values.suffix.has_value();

                // 2. Let endMatch be the result of running find a string in range with query
                //    end, rangeEndSearchRange, wordStartBounded true, wordEndBounded
                //    mustEndAtWordBoundary and matchMustBeAtBeginning false.
                auto end_match = find_a_string_in_a_range(*parsed_values.end, range_end_search_range, true, must_end_at_word_boundary, false, search_context);

                // 3. If endMatch is null then return null.
                if (!end_match.has_value())
                    return {};

                // 4. Set potentialMatch's end to endMatch's end.
                potential_match->end = end_match->end;
            }

            // 3. If parsedValues's suffix is null, return potentialMatch.
            if (!parsed_values.suffix.has_value())
                return create_range(*potential_match);

            // 4. Let suffixRange be a range with start equal to potentialMatch's end and end
            //    equal to searchRange's end.
            SearchRange suffix_range { searchable_text, SearchPosition::in_block(potential_match->end), search_range.end };

            // 5. Advance suffixRange's start to the next non-whitespace position.
            advance_a_range_start_to_the_next_non_whitespace_position(suffix_range);

            // 6. Let suffixMatch be result of running find a string in range with query suffix,
            //    suffixRange, wordStartBounded false, wordEndBounded true and
            //    matchMustBeAtBeginning true.
            auto suffix_match = find_a_string_in_a_range(*parsed_values.suffix, suffix_range, false, true, true, search_context);

            // 7. If suffixMatch is non-null, return potentialMatch.
            if (suffix_match.has_value())
                return create_range(*potential_match);

            // 8. If parsedValues's end item is null and suffixMatch is null, then break.
            if (!parsed_values.end.has_value())
                break;

            // 9. Set rangeEndSearchRange's start to potentialMatch's end.
            range_end_search_range.start = SearchPosition::in_block(potential_match->end);
        }

        // 6. If rangeEndSearchRange is collapsed then:
        if (range_end_search_range.collapsed()) {
            // 1. Assert: parsedValues's end item is non-null.
            VERIFY(parsed_values.end.has_value());

            // 2. Return null.
            return {};
        }
    }

    // 3. Return null.
    return {};
}

}
