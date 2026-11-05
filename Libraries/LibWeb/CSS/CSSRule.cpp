/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/CSSDescriptors.h>
#include <LibWeb/CSS/CSSFontFeatureValuesRule.h>
#include <LibWeb/CSS/CSSPositionTryDescriptors.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleSheetState.h>

namespace Web::CSS {

using Parser::ValueParserFFI::NativeRule;
using Parser::ValueParserFFI::NativeRuleList;

// NB: Implemented in Libraries/LibWeb/Bindings/Rust/src/css/exports.rs.
extern "C" {
NativeRule const* web_css_rule_native(void* rule);
void* web_css_rule_parent_rule(void* rule);
void* web_css_rule_parent_style_sheet(void* rule);
FlatPtr web_css_rule_serialized(void* vm, void* rule);
void* web_css_rule_declarations(void* vm, void* rule);
void web_css_font_face_descriptors_create(void* vm, void* descriptors);
void web_css_page_descriptors_create(void* vm, void* descriptors);
void web_css_function_descriptors_create(void* vm, void* descriptors);
}

// NB: What the implementation of the rules in Rust asks of the C++ code of LibWeb.
extern "C" {
void* web_css_rule_create_declarations(void* rule);
FlatPtr web_css_declarations_serialized(void* declarations);
NativeRuleList const* web_css_style_sheet_native_rules(void* sheet);
FlatPtr web_css_selector_list_text(void const* selectors, void* sheet);
void* web_css_style_sheet_import_rule(void* sheet, u64 identity);
void web_css_style_sheet_disconnect_font_faces(void* sheet, NativeRule const* rule);
void web_css_style_sheet_rules_changed(void* sheet);
void web_css_counter_style_rule_changed(void* rule);
void web_css_font_feature_values_rule_clear_caches(void* rule);
}

static StyleSheetState& state_of_sheet(void* sheet)
{
    return static_cast<CSSStyleSheet*>(sheet)->state();
}

CSSRule::Type CSSRule::type() const
{
    return Parser::ValueParserFFI::rust_rule_type(native_handle());
}

NativeRule const* CSSRule::native_handle() const
{
    return web_css_rule_native(const_cast<CSSRule*>(this));
}

CSSRule* CSSRule::parent_rule() const
{
    return static_cast<CSSRule*>(web_css_rule_parent_rule(const_cast<CSSRule*>(this)));
}

StyleSheetState* CSSRule::parent_style_sheet() const
{
    auto* sheet = web_css_rule_parent_style_sheet(const_cast<CSSRule*>(this));
    return sheet ? &state_of_sheet(sheet) : nullptr;
}

// NB: Read from the native rule when asked, as only DevTools asks.
Optional<SourcePosition> CSSRule::source_location() const
{
    auto payload = native_rule().payload();
    if (!payload.has_source_position)
        return {};
    VERIFY(payload.start_line <= NumericLimits<u32>::max());
    VERIFY(payload.start_column <= NumericLimits<u32>::max());
    return SourcePosition { static_cast<u32>(payload.start_line), static_cast<u32>(payload.start_column) };
}

Utf16String CSSRule::serialized() const
{
    return Utf16String::adopt_raw(web_css_rule_serialized(JS::HostABI::vm_to_abi(JS::VM::the()), const_cast<CSSRule*>(this)));
}

GC::Ref<CSSStyleDeclaration> CSSRule::declarations() const
{
    auto* declarations = web_css_rule_declarations(JS::HostABI::vm_to_abi(JS::VM::the()), const_cast<CSSRule*>(this));
    return *static_cast<CSSStyleDeclaration*>(static_cast<GC::Cell*>(declarations));
}

static GC::Ref<CSSStyleDeclaration> create_rust_descriptors(AtRuleID at_rule, Parser::ValueParserFFI::FfiDescriptorBlock const* block, void (*create_owner)(void* vm, void* descriptors))
{
    auto descriptors = CSSDescriptors::create(at_rule, RustDescriptorBlock { Parser::ValueParserFFI::rust_descriptor_block_retain(block) });
    create_owner(JS::HostABI::vm_to_abi(JS::VM::the()), static_cast<GC::Cell*>(descriptors.ptr()));
    return descriptors;
}

void* web_css_rule_create_declarations(void* cell)
{
    using namespace Parser::ValueParserFFI;

    auto& rule = *static_cast<CSSRule*>(cell);
    auto native_rule = rule.native_rule();
    auto payload = native_rule.payload();

    auto declarations = [&]() -> GC::Ref<CSSStyleDeclaration> {
        switch (native_rule.type()) {
        case CSSRule::Type::Style:
            return CSSStyleProperties::create(RustDeclarationBlock { rust_rule_declarations(native_rule.handle()) });
        case CSSRule::Type::NestedDeclarations:
        case CSSRule::Type::Margin:
            return CSSStyleProperties::create(RustDeclarationBlock { rust_declaration_block_retain(payload.declarations) });
        case CSSRule::Type::Keyframe:
            return CSSStyleProperties::create(RustDeclarationBlock { rust_keyframe_declarations(payload.keyframe) });
        case CSSRule::Type::PositionTry:
            return CSSPositionTryDescriptors::create(RustDeclarationBlock { rust_declaration_block_retain(payload.declarations) });
        case CSSRule::Type::FontFace:
            return create_rust_descriptors(AtRuleID::FontFace, payload.descriptors, web_css_font_face_descriptors_create);
        case CSSRule::Type::Page:
            return create_rust_descriptors(AtRuleID::Page, payload.descriptors, web_css_page_descriptors_create);
        case CSSRule::Type::FunctionDeclarations:
            return create_rust_descriptors(AtRuleID::Function, payload.descriptors, web_css_function_descriptors_create);
        default:
            VERIFY_NOT_REACHED();
        }
    }();
    declarations->set_parent_rule(rule);
    return static_cast<GC::Cell*>(declarations.ptr());
}

FlatPtr web_css_declarations_serialized(void* declarations)
{
    return static_cast<CSSStyleDeclaration*>(static_cast<GC::Cell*>(declarations))->serialized().into_raw();
}

NativeRuleList const* web_css_style_sheet_native_rules(void* sheet)
{
    return state_of_sheet(sheet).native_rules().handle();
}

FlatPtr web_css_selector_list_text(void const* selectors, void* sheet)
{
    auto list = selector_list_from_rust(static_cast<SelectorFFI::RustParsedSelectorList const*>(selectors));
    return serialize_a_group_of_selectors(list, sheet ? &state_of_sheet(sheet) : nullptr).into_raw();
}

void* web_css_style_sheet_import_rule(void* sheet, u64 identity)
{
    auto* import = state_of_sheet(sheet).import_for_rule(identity);
    return import ? &import->cssom_rule() : nullptr;
}

void web_css_style_sheet_disconnect_font_faces(void* sheet, NativeRule const* rule)
{
    state_of_sheet(sheet).disconnect_font_faces_in_rule(RustRule { rule });
}

void web_css_style_sheet_rules_changed(void* sheet)
{
    state_of_sheet(sheet).recalculate_rule_caches();
}

void web_css_counter_style_rule_changed(void* cell)
{
    auto& rule = *static_cast<CSSRule*>(cell);
    auto* parent_style_sheet = rule.parent_style_sheet();

    if (!parent_style_sheet)
        return;

    parent_style_sheet->for_each_owning_style_scope([&](StyleScope& style_scope) {
        style_scope.invalidate_counter_style_cache();
    });
    record_style_rule_declarations_changed(rule);
}

void web_css_font_feature_values_rule_clear_caches(void* rule)
{
    static_cast<CSSFontFeatureValuesRule*>(rule)->clear_caches();
}

}
