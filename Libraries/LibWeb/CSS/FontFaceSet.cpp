/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/RenderingStats.h>
#include <LibWeb/WebIDL/Promise.h>

// The FontFaceSet implementation is in Libraries/LibWeb/Bindings/Rust/src/css/font_face_set.rs. This is what C++ asks
// of it, and what it asks of the FontFace objects and the document that LibWeb keeps in C++.

namespace Web::CSS {

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_font_face_set_create(JSVM*, void* realm, void* environment);
size_t web_font_face_set_size(void* set);
void* web_font_face_set_relevant_settings_object(void* set);
void web_font_face_set_add_css_connected_font(JSVM*, void* set, void* font_face);
void web_font_face_set_remove_css_connected_font(JSVM*, void* set, void* font_face);
void web_font_face_set_synchronize_css_connected_font_order(JSVM*, void* set);
void web_font_face_set_set_is_pending_on_the_environment(JSVM*, void* set, bool is_pending_on_the_environment);
void web_font_face_set_font_started_loading(JSVM*, void* set, void* font_face);
void web_font_face_set_font_finished_loading(JSVM*, void* set, void* font_face, bool succeeded);

// Declared in Bindings/Rust/src/css/font_face_set.rs.
void* web_css_font_face_of_wrapper(void* wrapper);
void web_css_font_face_ref(void* font_face);
void web_css_font_face_unref(void* font_face);
void web_css_font_face_visit_edges(void* font_face, void* visitor);
void* web_css_font_face_cssom_font_face(void* font_face);
bool web_css_font_face_is_css_connected(void* font_face);
bool web_css_font_face_css_rule_identity(void* font_face, u64* identity);
bool web_css_font_face_is_loading(void* font_face);
bool web_css_font_face_is_loaded(void* font_face);
FlatPtr web_css_font_face_family_name(void* font_face);
bool web_css_font_face_unicode_ranges_include_any_of(void* font_face, FlatPtr text);
void* web_css_font_face_load(void* font_face);
void web_css_font_face_did_join_set(void* font_face, void* set);
void web_css_font_face_did_leave_set(void* font_face, void* set);
void web_css_font_face_set_record_loading_fonts(void* set, size_t count);
void web_css_font_face_set_update_layout(void* set, bool skip_if_running);
bool web_css_font_face_set_for_each_font_face_rule(void* set, void* context, void (*callback)(void*, u64));
void web_css_font_face_set_synchronize_font_computer(void* set, void* const* font_faces, size_t count);
}

static FontFaceState& state_from_abi(void* font_face)
{
    return *static_cast<FontFaceState*>(font_face);
}

static FontFaceSet& set_from_abi(void* set)
{
    return *static_cast<FontFaceSet*>(set);
}

static JSVM* vm_abi()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

GC::Ref<FontFaceSet> FontFaceSet::create(HTML::EnvironmentSettingsObject& environment)
{
    return *static_cast<FontFaceSet*>(web_font_face_set_create(vm_abi(), &environment.realm(), static_cast<GC::Cell*>(&environment)));
}

size_t FontFaceSet::set_size()
{
    return web_font_face_set_size(this);
}

void FontFaceSet::add_css_connected_font(NonnullRefPtr<FontFaceState> font_face)
{
    web_font_face_set_add_css_connected_font(vm_abi(), this, font_face.ptr());
}

void FontFaceSet::remove_css_connected_font(NonnullRefPtr<FontFaceState> font_face)
{
    web_font_face_set_remove_css_connected_font(vm_abi(), this, font_face.ptr());
}

void FontFaceSet::synchronize_css_connected_font_order()
{
    web_font_face_set_synchronize_css_connected_font_order(vm_abi(), this);
}

HTML::EnvironmentSettingsObject& FontFaceSet::relevant_settings_object()
{
    return *static_cast<HTML::EnvironmentSettingsObject*>(static_cast<GC::Cell*>(web_font_face_set_relevant_settings_object(this)));
}

void FontFaceSet::set_is_pending_on_the_environment(bool is_pending_on_the_environment)
{
    web_font_face_set_set_is_pending_on_the_environment(vm_abi(), this, is_pending_on_the_environment);
}

void FontFaceSet::font_started_loading(FontFaceState& font_face)
{
    web_font_face_set_font_started_loading(vm_abi(), this, &font_face);
}

void FontFaceSet::font_finished_loading(FontFaceState& font_face, bool succeeded)
{
    web_font_face_set_font_finished_loading(vm_abi(), this, &font_face, succeeded);
}

void* web_css_font_face_of_wrapper(void* wrapper)
{
    auto* font_face = Bindings::impl_from<FontFace>(static_cast<JS::Object*>(wrapper));
    VERIFY(font_face);
    return &font_face->state();
}

void web_css_font_face_ref(void* font_face)
{
    state_from_abi(font_face).ref();
}

void web_css_font_face_unref(void* font_face)
{
    state_from_abi(font_face).unref();
}

void web_css_font_face_visit_edges(void* font_face, void* visitor)
{
    state_from_abi(font_face).visit_edges(*static_cast<GC::Cell::Visitor*>(visitor));
}

void* web_css_font_face_cssom_font_face(void* font_face)
{
    return static_cast<GC::Cell*>(&state_from_abi(font_face).cssom_font_face());
}

bool web_css_font_face_is_css_connected(void* font_face)
{
    return state_from_abi(font_face).is_css_connected();
}

bool web_css_font_face_css_rule_identity(void* font_face, u64* identity)
{
    auto rule_identity = state_from_abi(font_face).css_rule_identity();
    if (!rule_identity.has_value())
        return false;
    *identity = *rule_identity;
    return true;
}

bool web_css_font_face_is_loading(void* font_face)
{
    return state_from_abi(font_face).status() == FontFaceLoadStatus::Loading;
}

bool web_css_font_face_is_loaded(void* font_face)
{
    return state_from_abi(font_face).status() == FontFaceLoadStatus::Loaded;
}

FlatPtr web_css_font_face_family_name(void* font_face)
{
    return state_from_abi(font_face).family_name().to_utf16_string().into_raw();
}

bool web_css_font_face_unicode_ranges_include_any_of(void* font_face, FlatPtr raw_text)
{
    auto text = Utf16String::adopt_raw(raw_text);
    auto const& ranges = state_from_abi(font_face).unicode_ranges();
    for (auto code_point : text.utf16_view()) {
        for (auto const& range : ranges) {
            if (range.contains(code_point))
                return true;
        }
    }
    return false;
}

void* web_css_font_face_load(void* font_face)
{
    auto& state = state_from_abi(font_face);
    state.load();
    return state.font_status_promise().ptr();
}

void web_css_font_face_did_join_set(void* font_face, void* raw_set)
{
    auto& state = state_from_abi(font_face);
    auto& set = set_from_abi(raw_set);
    state.add_to_set(set);
    if (state.should_be_registered_with_font_computer()) {
        if (auto font_computer = font_computer_for_font_source(set.relevant_settings_object().global_object()); font_computer.has_value())
            font_computer->register_font_face(state);
    }
}

void web_css_font_face_did_leave_set(void* font_face, void* raw_set)
{
    auto& state = state_from_abi(font_face);
    auto& set = set_from_abi(raw_set);
    if (state.should_be_registered_with_font_computer()) {
        if (auto font_computer = font_computer_for_font_source(set.relevant_settings_object().global_object()); font_computer.has_value())
            font_computer->unregister_font_face(state);
    }
    state.remove_from_set(set);
}

void web_css_font_face_set_record_loading_fonts(void* raw_set, size_t count)
{
    if (!Core::StatsLog::the())
        return;
    auto& set = set_from_abi(raw_set);
    auto* window = HTML::window_from_global_object(set.relevant_settings_object().global_object());
    if (!window || window->associated_document().fonts().ptr() != &set)
        return;
    set_rendering_pending(window->associated_document(), RenderingPendingReason::FontLoads, count);
}

void web_css_font_face_set_update_layout(void* set, bool skip_if_running)
{
    auto* window = HTML::window_from_global_object(set_from_abi(set).relevant_settings_object().global_object());
    if (!window)
        return;
    auto& document = window->associated_document();
    if (!skip_if_running || !document.is_running_update_layout())
        document.update_layout(DOM::UpdateLayoutReason::FontFaceSetReady);
}

bool web_css_font_face_set_for_each_font_face_rule(void* set, void* context, void (*callback)(void*, u64))
{
    auto* window = HTML::window_from_global_object(set_from_abi(set).relevant_settings_object().global_object());
    if (!window)
        return false;
    Function<void(StyleSheetState&)> for_each_rule = [&](StyleSheetState& sheet) {
        sheet.for_each_effective_rule_data(TraversalOrder::Preorder, [&](RustRuleView const& rule, Utf16View) {
            if (rule.type() == RustRule::Type::FontFace)
                callback(context, rule.identity());
        });
    };
    window->associated_document().for_each_active_css_style_sheet(for_each_rule);
    return true;
}

void web_css_font_face_set_synchronize_font_computer(void* set, void* const* font_faces, size_t count)
{
    auto* window = HTML::window_from_global_object(set_from_abi(set).relevant_settings_object().global_object());
    if (!window)
        return;
    Vector<NonnullRefPtr<FontFaceState>> states;
    states.ensure_capacity(count);
    for (size_t index = 0; index < count; ++index)
        states.unchecked_append(state_from_abi(font_faces[index]));
    window->associated_document().font_computer().synchronize_font_face_order(states);
}

}
