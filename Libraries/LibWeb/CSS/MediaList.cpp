/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/Dump.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(MediaList);

GC::Ref<MediaList> MediaList::create(Vector<NonnullRefPtr<MediaQuery>>&& media)
{
    return GC::Heap::the().allocate<MediaList>(move(media));
}

MediaList::MediaList(Vector<NonnullRefPtr<MediaQuery>>&& media)
    : m_media(move(media))
{
}

void MediaList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_style_sheet);
    visitor.visit(m_associated_rule);
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-mediatext
Utf16String MediaList::media_text() const
{
    return serialize_a_media_query_list(m_media);
}

// Both owners reach the same place: the sheet whose rules the gate belongs to.
GC::Ptr<CSSStyleSheet> MediaList::owning_style_sheet()
{
    if (m_associated_style_sheet)
        return as<CSSStyleSheet>(*m_associated_style_sheet);
    if (m_associated_rule)
        return m_associated_rule->parent_style_sheet();
    return {};
}

void MediaList::invalidate_owners_for_media_change()
{
    auto sheet = owning_style_sheet();
    if (!sheet)
        return;
    sheet->invalidate_owners();

    // Which of the sheet's rules are gated is published from here rather than from the transition
    // `evaluate_media_queries` reports, because a list that has just been reparsed has evaluated
    // nothing: a freshly parsed query reads as not matching, so turning a condition off looks like
    // no change at all. Saying the state outright leaves it to the engine to reject what did not
    // move, which it does.
    record_stylesheet_rule_conditions(*sheet);
}

void MediaList::set_media_text(Utf16View text)
{
    ScopeGuard guard = [&] {
        invalidate_owners_for_media_change();
    };

    m_media.clear();
    if (text.is_empty())
        return;
    m_media = parse_media_query_list(Parser::ParsingParams {}, text);
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-item
Optional<Utf16String> MediaList::item(u32 index) const
{
    if (index >= m_media.size())
        return {};

    return m_media[index]->to_string();
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-appendmedium
void MediaList::append_medium(Utf16View medium)
{
    // 1. Let m be the result of parsing the given value.
    auto m = parse_media_query(Parser::ParsingParams {}, medium);

    // 2. If m is null, then return.
    if (!m)
        return;

    // 3. If comparing m with any of the media queries in the collection of media queries returns true, then return.
    auto serialized = m->to_string();
    for (auto& existing_medium : m_media) {
        if (existing_medium->to_string() == serialized)
            return;
    }

    // 4. Append m to the collection of media queries.
    m_media.append(m.release_nonnull());

    invalidate_owners_for_media_change();
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-deletemedium
WebIDL::ExceptionOr<void> MediaList::delete_medium(Utf16View medium)
{
    // 1. Let m be the result of parsing the given value.
    auto m = parse_media_query(Parser::ParsingParams {}, medium);

    // 2. If m is null, then return.
    if (!m)
        return {};

    // 3. Remove any media query from the collection of media queries for which comparing the media query with m
    //    returns true. If nothing was removed, then throw a NotFoundError exception.
    bool was_removed = m_media.remove_all_matching([&](auto& existing) -> bool {
        return m->to_string() == existing->to_string();
    });
    if (!was_removed)
        return WebIDL::NotFoundError::create("Media query not found in list"_utf16);

    invalidate_owners_for_media_change();

    return {};
}

bool MediaList::evaluate(DOM::Document const& document)
{
    MediaEnvironmentSnapshot environment { document };
    for (auto& media : m_media)
        media->evaluate(environment);

    return matches();
}

bool MediaList::matches() const
{
    if (m_media.is_empty())
        return true;

    for (auto& media : m_media) {
        if (media->matches())
            return true;
    }
    return false;
}

void MediaList::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Media list ({}):\n", m_media.size());
    for (auto const& media : m_media)
        media->dump(builder, indent_levels + 1);
}

}
