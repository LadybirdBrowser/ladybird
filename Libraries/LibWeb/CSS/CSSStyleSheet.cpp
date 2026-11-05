/*
 * Copyright (c) 2019-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Cell.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSRuleList.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

// What the CSSStyleSheet of a sheet holds it by, as the object is a cell that keeps it alive and traces its edges.
// It also keeps the observable owner chain alive, which the state only refers to weakly.
class StyleSheetStateCell final : public GC::Cell {
    GC_CELL(StyleSheetStateCell, GC::Cell);
    GC_DECLARE_ALLOCATOR(StyleSheetStateCell);

public:
    explicit StyleSheetStateCell(StyleSheetState& state)
        : m_state(state)
    {
    }

    StyleSheetState& state() const { return m_state; }

    void update_owner_chain()
    {
        auto* parent = m_state->parent_style_sheet();
        m_parent_sheet = parent ? &parent->cssom_sheet() : nullptr;
        m_owner_import = m_state->owner_import();
    }

private:
    virtual void visit_edges(GC::Cell::Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_state);
        visitor.visit(m_parent_sheet);
        visitor.visit(m_owner_import);
    }

    NonnullRefPtr<StyleSheetState> m_state;
    GC::Ptr<CSSStyleSheet> m_parent_sheet;
    RefPtr<StyleSheetImport> m_owner_import;
};

GC_DEFINE_ALLOCATOR(StyleSheetStateCell);

// NB: What the implementation of the sheets in Rust offers to the C++ code of LibWeb.
extern "C" {
void* web_css_style_sheet_create(void* vm, void* state);
void* web_css_style_sheet_state(void* sheet);
void* web_css_style_sheet_list_create(void* vm, void* scope, void* node);
}

// NB: What the implementation of the sheets in Rust asks of the C++ code of LibWeb.
extern "C" {
void* web_css_style_sheet_construct(void* document, bool has_base_url, FlatPtr base_url, void* media_list, FlatPtr media_text, bool disabled);
size_t web_css_sheet_state_external_memory_size(void* state);
bool web_css_sheet_state_flag(void* state, u8 flag);
void web_css_sheet_state_set_disabled(void* state, bool disabled);
bool web_css_sheet_state_location(void* state, FlatPtr* location);
FlatPtr web_css_sheet_state_title(void* state);
void* web_css_sheet_state_owner_node(void* state);
void* web_css_sheet_state_parent_style_sheet(void* state);
void* web_css_sheet_state_media(void* state);
Parser::ValueParserFFI::NativeRuleList const* web_css_sheet_state_native_rules(void* state);
void* web_css_sheet_state_owner_rule(void* state);
void* web_css_sheet_state_rule_list(void* state);
Parser::ValueParserFFI::NativeRule const* web_css_sheet_state_parse_rule(void* state, FlatPtr rule);
void web_css_sheet_state_rule_inserted(void* state, Parser::ValueParserFFI::NativeRule const* rule);
void web_css_sheet_state_remove_rule(void* state, u32 index);
void* web_css_sheet_state_replace(void* state, FlatPtr text);
void web_css_sheet_state_replace_rules(void* state, FlatPtr text);
u32 web_css_style_sheet_list_length(void* scope);
void* web_css_style_sheet_list_item(void* scope, u32 index);
}

static StyleSheetStateCell& state_cell_of(void* cell)
{
    return *static_cast<StyleSheetStateCell*>(static_cast<GC::Cell*>(cell));
}

static StyleSheetState& state_of(void* cell)
{
    return state_cell_of(cell).state();
}

StyleSheetState* css_style_sheet_from_value(JS::Value value)
{
    auto* sheet = value.is_object() ? Bindings::impl_from<CSSStyleSheet>(&value.as_object()) : nullptr;
    return sheet ? &sheet->state() : nullptr;
}

JS::Value css_style_sheet(JS::Realm& realm, StyleSheetState& style_sheet)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { style_sheet.cssom_sheet() });
}

GC::Ref<JS::SyntheticModule> create_css_style_sheet_default_export_module(JS::Realm& realm, StyleSheetState& style_sheet, StringView filename)
{
    return JS::SyntheticModule::create_default_export_synthetic_module(realm, css_style_sheet(realm, style_sheet), filename);
}

GC::Ref<CSSStyleSheet> CSSStyleSheet::create(StyleSheetState& state)
{
    auto cell = GC::Heap::the().allocate<StyleSheetStateCell>(state);
    cell->update_owner_chain();
    auto* sheet = web_css_style_sheet_create(JS::HostABI::vm_to_abi(JS::VM::the()), static_cast<GC::Cell*>(cell.ptr()));
    return *static_cast<CSSStyleSheet*>(sheet);
}

StyleSheetState& CSSStyleSheet::state() const
{
    return state_of(web_css_style_sheet_state(const_cast<CSSStyleSheet*>(this)));
}

void CSSStyleSheet::update_owner_chain()
{
    state_cell_of(web_css_style_sheet_state(this)).update_owner_chain();
}

GC::Ref<StyleSheetList> StyleSheetList::create(StyleScope& scope)
{
    auto* list = web_css_style_sheet_list_create(JS::HostABI::vm_to_abi(JS::VM::the()), &scope, static_cast<GC::Cell*>(&scope.node()));
    return *static_cast<StyleSheetList*>(list);
}

void* web_css_style_sheet_construct(void* document, bool has_base_url, FlatPtr base_url, void* media_list, FlatPtr media_text, bool disabled)
{
    auto base_url_text = Utf16String::adopt_raw(base_url);
    auto media_text_string = Utf16String::adopt_raw(media_text);

    CSSStyleSheetOptions options;
    if (has_base_url)
        options.base_url = move(base_url_text);
    if (media_list)
        options.media = GC::Ref { *static_cast<MediaList*>(media_list) };
    else
        options.media = move(media_text_string);
    options.disabled = disabled;

    auto state = StyleSheetState::create_constructed(*static_cast<DOM::Document*>(static_cast<GC::Cell*>(document)), options);
    if (state.is_error())
        return nullptr;
    return &state.value()->cssom_sheet();
}

size_t web_css_sheet_state_external_memory_size(void* state)
{
    return state_of(state).external_memory_size();
}

bool web_css_sheet_state_flag(void* state, u8 flag)
{
    return state_of(state).native_sheet().flag(static_cast<RustStyleSheet::Flag>(flag));
}

void web_css_sheet_state_set_disabled(void* state, bool disabled)
{
    state_of(state).set_disabled(disabled);
}

bool web_css_sheet_state_location(void* state, FlatPtr* location)
{
    auto href = state_of(state).href_for_bindings();
    if (!href.has_value())
        return false;
    *location = href.release_value().into_raw();
    return true;
}

FlatPtr web_css_sheet_state_title(void* state)
{
    return Utf16String { state_of(state).title() }.into_raw();
}

void* web_css_sheet_state_owner_node(void* state)
{
    return static_cast<GC::Cell*>(state_of(state).owner_node());
}

void* web_css_sheet_state_parent_style_sheet(void* state)
{
    auto* parent = state_of(state).parent_style_sheet();
    return parent ? &parent->cssom_sheet() : nullptr;
}

void* web_css_sheet_state_media(void* state)
{
    return state_of(state).media().ptr();
}

Parser::ValueParserFFI::NativeRuleList const* web_css_sheet_state_native_rules(void* state)
{
    return state_of(state).native_rules().handle();
}

void* web_css_sheet_state_owner_rule(void* state)
{
    return state_of(state).owner_rule().ptr();
}

void* web_css_sheet_state_rule_list(void* state)
{
    return &state_of(state).rules();
}

Parser::ValueParserFFI::NativeRule const* web_css_sheet_state_parse_rule(void* state, FlatPtr rule)
{
    auto text = Utf16String::adopt_raw(rule);
    auto parsed_rule = state_of(state).parse_rule(text.utf16_view());
    if (!parsed_rule.has_value())
        return nullptr;
    return Parser::ValueParserFFI::rust_rule_retain(parsed_rule->handle());
}

void web_css_sheet_state_rule_inserted(void* state, Parser::ValueParserFFI::NativeRule const* rule)
{
    state_of(state).rule_inserted(RustRule { rule });
}

void web_css_sheet_state_remove_rule(void* state, u32 index)
{
    state_of(state).remove_rule(index);
}

void* web_css_sheet_state_replace(void* state, FlatPtr text)
{
    return state_of(state).replace(Utf16String::adopt_raw(text)).ptr();
}

void web_css_sheet_state_replace_rules(void* state, FlatPtr text)
{
    state_of(state).replace_rules(Utf16String::adopt_raw(text));
}

u32 web_css_style_sheet_list_length(void* scope)
{
    return static_cast<StyleScope*>(scope)->style_sheets().size();
}

void* web_css_style_sheet_list_item(void* scope, u32 index)
{
    auto const& sheets = static_cast<StyleScope*>(scope)->style_sheets();
    if (index >= sheets.size())
        return nullptr;
    return &sheets[index]->cssom_sheet();
}

}
