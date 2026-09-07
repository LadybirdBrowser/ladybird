/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/Dump.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(MediaList);

GC::Ref<MediaList> MediaList::create(RustMediaList media)
{
    return GC::Heap::the().allocate<MediaList>(move(media));
}

MediaList::MediaList(RustMediaList media)
    : m_media(move(media))
{
}

void MediaList::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_style_sheet);
    visitor.visit(m_associated_cssom_sheet);
    visitor.visit(m_associated_rule);
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-mediatext
Utf16String MediaList::media_text() const
{
    return m_media.media_text();
}

MediaList::~MediaList() = default;

void MediaList::set_associated_style_sheet(NonnullRefPtr<StyleSheetState> sheet)
{
    m_associated_cssom_sheet = &sheet->cssom_sheet();
    m_associated_style_sheet = move(sheet);
}

// Both owners reach the same place: the sheet whose rules the gate belongs to.
RefPtr<StyleSheetState> MediaList::owning_style_sheet()
{
    if (m_associated_style_sheet)
        return m_associated_style_sheet;
    if (m_associated_rule)
        return m_associated_rule->parent_style_sheet();
    return {};
}

void MediaList::invalidate_owners_for_media_change()
{
    auto sheet = owning_style_sheet();
    if (!sheet)
        return;
    invalidate_style_sheet_for_media_change(*sheet);
}

void invalidate_style_sheet_for_media_change(StyleSheetState& sheet)
{
    sheet.invalidate_owners();
    sheet.synchronize_fonts_after_rule_change();

    // Which of the sheet's rules are gated is published from here rather than from the transition
    // `evaluate_media_queries` reports, because a list that has just been reparsed has evaluated
    // nothing: a freshly parsed query reads as not matching, so turning a condition off looks like
    // no change at all. Saying the state outright leaves it to the engine to reject what did not
    // move, which it does.
    record_stylesheet_rule_conditions(sheet);
}

void MediaList::set_media_text(Utf16View text)
{
    m_media.set_text(text);
    invalidate_owners_for_media_change();
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-item
Optional<Utf16String> MediaList::item(u32 index) const
{
    return m_media.item(index);
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-appendmedium
void MediaList::append_medium(Utf16View medium)
{
    if (m_media.append(medium))
        invalidate_owners_for_media_change();
}

// https://www.w3.org/TR/cssom-1/#dom-medialist-deletemedium
WebIDL::ExceptionOr<void> MediaList::delete_medium(Utf16View medium)
{
    auto result = m_media.remove(medium);
    if (result == Parser::ValueParserFFI::MediaListDeleteResult::Invalid)
        return {};
    if (result == Parser::ValueParserFFI::MediaListDeleteResult::NotFound)
        return WebIDL::NotFoundError::create("Media query not found in list"_utf16);

    invalidate_owners_for_media_change();

    return {};
}

void RustMediaList::dump(StringBuilder& builder, int indent_levels) const
{
    dump_indent(builder, indent_levels);
    builder.appendff("Media list ({}):\n", length());
    for (size_t index = 0; index < length(); ++index) {
        dump_indent(builder, indent_levels + 1);
        builder.appendff("Media query: `{}` (matches = {})\n", *item(index), Parser::ValueParserFFI::rust_media_list_item_matches(m_list, index));
    }
}

}
