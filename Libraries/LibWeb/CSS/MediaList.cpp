/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/MediaList.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_media_list_create(JSVM*, Parser::ValueParserFFI::MediaList const*);
void web_media_list_set_associated_style_sheet(JSVM*, void* list, void* sheet);
void web_media_list_set_associated_rule(JSVM*, void* list, void* rule);
Parser::ValueParserFFI::MediaList* web_media_list_share_native_list(void* list);

// Declared in Bindings/Rust/src/css/media_list.rs.
void web_css_media_list_changed(void* style_sheet, void* rule);
}

GC::Ref<MediaList> MediaList::create(RustMediaList const& media)
{
    return GC::Ref { *static_cast<MediaList*>(web_media_list_create(JS::HostABI::vm_to_abi(JS::VM::the()), media.handle())) };
}

RustMediaList MediaList::share_native_list() const
{
    return RustMediaList { web_media_list_share_native_list(const_cast<MediaList*>(this)) };
}

void MediaList::set_associated_style_sheet(NonnullRefPtr<StyleSheetState> sheet)
{
    web_media_list_set_associated_style_sheet(JS::HostABI::vm_to_abi(JS::VM::the()), this, &sheet->cssom_sheet());
}

void MediaList::set_associated_rule(GC::Ref<CSSRule> rule)
{
    web_media_list_set_associated_rule(JS::HostABI::vm_to_abi(JS::VM::the()), this, rule.ptr());
}

void web_css_media_list_changed(void* style_sheet_cell, void* rule_cell)
{
    auto* style_sheet = static_cast<CSSStyleSheet*>(style_sheet_cell);
    auto* rule = static_cast<CSSRule*>(rule_cell);

    // Both owners reach the same place: the sheet whose rules the gate belongs to.
    RefPtr<StyleSheetState> sheet;
    if (style_sheet)
        sheet = style_sheet->state();
    else if (rule)
        sheet = rule->parent_style_sheet();
    if (!sheet)
        return;

    if (rule)
        stop_sharing_compiled_style_sheet(*sheet);
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

MediaEnvironmentSnapshot::MediaEnvironmentSnapshot(DOM::Document const& document)
{
    if (!document.window())
        return;

    m_length_resolution_context = to_ffi_length_resolution_context_with_container_bases(
        Length::ResolutionContext::for_document(document), all_container_relative_length_units_mask);

    for (size_t index = 0; index < m_values.size(); ++index)
        m_values[index] = document.window()->query_media_feature(static_cast<MediaFeatureID>(index));
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
