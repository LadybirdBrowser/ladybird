/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Set.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/FontFaceSetPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/FontFaceSetLoadEvent.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(FontFaceSet);

GC::Ref<FontFaceSet> FontFaceSet::create(JS::Realm& realm)
{
    HTML::TemporaryExecutionContext temporary_execution_context { realm };
    return realm.create<FontFaceSet>(realm);
}

FontFaceSet::FontFaceSet(JS::Realm& realm)
    : DOM::EventTarget(realm)
    , m_set_entries(JS::Set::create(realm))
    , m_ready_promise(WebIDL::create_promise(realm))
{
}

void FontFaceSet::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FontFaceSet);
    Base::initialize(realm);
}

void FontFaceSet::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_set_entries);
    visitor.visit(m_ready_promise);
    visitor.visit(m_loading_fonts);
    visitor.visit(m_loaded_fonts);
    visitor.visit(m_failed_fonts);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-add
WebIDL::ExceptionOr<GC::Ref<FontFaceSet>> FontFaceSet::add(GC::Root<FontFace> face)
{
    // 1. If font is already in the FontFaceSet’s set entries, skip to the last step of this algorithm immediately.
    if (m_set_entries->set_has(face))
        return GC::Ref<FontFaceSet>(*this);

    // 2. If font is CSS-connected, throw an InvalidModificationError exception and exit this algorithm immediately.
    if (face->is_css_connected()) {
        return WebIDL::InvalidModificationError::create(realm(), "Cannot add a CSS-connected FontFace to a FontFaceSet"_utf16);
    }

    // NB: This method implements steps 3 and 4.
    add_css_connected_font(*face);

    // 5. Return the FontFaceSet.
    return GC::Ref<FontFaceSet>(*this);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-add
void FontFaceSet::add_css_connected_font(GC::Ref<FontFace> face)
{
    // 3. Add the font argument to the FontFaceSet’s set entries.
    m_set_entries->set_add(face);
    face->add_to_set(*this);

    // 4. If font’s status attribute is "loading"
    if (face->status() == Bindings::FontFaceLoadStatus::Loading) {

        // 1. If the FontFaceSet’s [[LoadingFonts]] list is empty, switch the FontFaceSet to loading.
        if (m_loading_fonts.is_empty())
            switch_to_loading();

        // 2. Append font to the FontFaceSet’s [[LoadingFonts]] list.
        m_loading_fonts.append(*face);
    }
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-delete
bool FontFaceSet::delete_(GC::Root<FontFace> face)
{
    // 1. If font is CSS-connected, return false and exit this algorithm immediately.
    if (face->is_css_connected()) {
        return false;
    }

    // 2. Let deleted be the result of removing font from the FontFaceSet’s set entries.
    bool deleted = m_set_entries->set_remove(face);
    face->remove_from_set(*this);

    // 3. If font is present in the FontFaceSet’s [[LoadedFonts]], or [[FailedFonts]] lists, remove it.
    m_loaded_fonts.remove_all_matching([face](auto const& entry) { return entry == face; });
    m_failed_fonts.remove_all_matching([face](auto const& entry) { return entry == face; });

    // 4. If font is present in the FontFaceSet’s [[LoadingFonts]] list, remove it.
    //    If font was the last item in that list (and so the list is now empty), switch the FontFaceSet to loaded.
    m_loading_fonts.remove_all_matching([face](auto const& entry) { return entry == face; });
    if (m_loading_fonts.is_empty())
        switch_to_loaded();

    return deleted;
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-clear
void FontFaceSet::clear()
{
    // 1. Remove all non-CSS-connected items from the FontFaceSet's set entries,
    //    its [[LoadedFonts]] list, and its [[FailedFonts]] list.
    Vector<JS::Value> to_remove;
    for (auto font_face_value : *m_set_entries) {
        auto& font_face = as<FontFace>(font_face_value.key.as_object());
        if (!font_face.is_css_connected()) {
            to_remove.append(font_face_value.key);
            font_face.remove_from_set(*this);
        }
    }
    for (auto const& value : to_remove)
        m_set_entries->set_remove(value);

    m_loaded_fonts.remove_all_matching([](auto const& font) { return !font->is_css_connected(); });
    m_failed_fonts.remove_all_matching([](auto const& font) { return !font->is_css_connected(); });

    // 2. If the FontFaceSet's [[LoadingFonts]] list is non-empty, remove all items from it,
    //    then switch the FontFaceSet to loaded.
    if (!m_loading_fonts.is_empty()) {
        m_loading_fonts.clear();
        switch_to_loaded();
    }
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloading
void FontFaceSet::set_onloading(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::loading, event_handler);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloading
WebIDL::CallbackType* FontFaceSet::onloading()
{
    return event_handler_attribute(HTML::EventNames::loading);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloadingdone
void FontFaceSet::set_onloadingdone(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::loadingdone, event_handler);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloadingdone
WebIDL::CallbackType* FontFaceSet::onloadingdone()
{
    return event_handler_attribute(HTML::EventNames::loadingdone);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloadingerror
void FontFaceSet::set_onloadingerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::loadingerror, event_handler);
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-onloadingerror
WebIDL::CallbackType* FontFaceSet::onloadingerror()
{
    return event_handler_attribute(HTML::EventNames::loadingerror);
}

// https://drafts.csswg.org/css-font-loading/#find-the-matching-font-faces
static WebIDL::ExceptionOr<GC::Ref<JS::Set>> find_matching_font_faces(JS::Realm& realm, FontFaceSet& font_face_set, String const& font, String const&)
{
    // 1. Parse font using the CSS value syntax of the font property. If a syntax error occurs, return a syntax error.
    auto property = parse_css_value(CSS::Parser::ParsingParams(), font, PropertyID::Font);
    if (!property || !property->is_shorthand())
        return WebIDL::SyntaxError::create(realm, "Unable to parse font"_utf16);

    // If the parsed value is a CSS-wide keyword, return a syntax error.
    // Note: This case is already caught by the is_shorthand check.

    // FIXME: Absolutize all relative lengths against the initial values of the corresponding properties. (For example, a
    //        relative font weight like bolder is evaluated against the initial value normal.)

    // FIXME: 2. If text was not explicitly provided, let it be a string containing a single space character (U+0020 SPACE).

    // 3. Let font family list be the list of font families parsed from font, and font style be the other font style
    //    attributes parsed from font.
    auto const& font_family_list = property->as_shorthand().longhand(PropertyID::FontFamily)->as_value_list();

    // 4. Let available font faces be the available font faces within source. If the allow system fonts flag is specified,
    //    add all system fonts to available font faces.
    auto available_font_faces = font_face_set.set_entries();

    // 5. Let matched font faces initially be an empty list.
    auto matched_font_faces = JS::Set::create(realm);

    // 6. For each family in font family list, use the font matching rules to select the font faces from available font
    //    faces that match the font style, and add them to matched font faces. The use of the unicodeRange attribute means
    //    that this may be more than just a single font face.
    for (auto const& font_family : font_family_list.values()) {
        // NB: Skip generic font family keywords
        if (font_family->is_keyword())
            continue;

        // FIXME: The matching below is super basic. We currently just match font family names by their string value.
        auto font_family_name = string_from_style_value(font_family);

        for (auto font_face_value : *available_font_faces) {
            auto& font_face = as<FontFace>(font_face_value.key.as_object());
            if (font_face.family() != font_family_name)
                continue;

            matched_font_faces->set_add(font_face_value.key);
        }
    }

    // FIXME: 7. If matched font faces is empty, set the found faces flag to false. Otherwise, set it to true.
    // FIXME: 8. For each font face in matched font faces, if its defined unicode-range does not include the codepoint of at
    //           least one character in text, remove it from the list.

    // 9. Return matched font faces and the found faces flag.
    return matched_font_faces;
}

// https://drafts.csswg.org/css-font-loading/#dom-fontfaceset-load
JS::ThrowCompletionOr<GC::Ref<WebIDL::Promise>> FontFaceSet::load(String const& font, String const& text)
{
    auto& realm = this->realm();

    // 1. Let font face set be the FontFaceSet object this method was called on. Let promise be a newly-created promise object.
    GC::Ref font_face_set = *this;
    auto promise = WebIDL::create_promise(realm);

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, font_face_set, promise, font, text]() mutable {
        // 3. Find the matching font faces from font face set using the font and text arguments passed to the function,
        //    and let font face list be the return value (ignoring the found faces flag). If a syntax error was returned,
        //    reject promise with a SyntaxError exception and terminate these steps.
        auto result = find_matching_font_faces(realm, font_face_set, font, text);
        if (result.is_error()) {
            HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            WebIDL::reject_promise(realm, promise, Bindings::exception_to_throw_completion(realm.vm(), result.release_error()).release_value());
            return;
        }

        auto matched_font_faces = result.release_value();

        // 4. Queue a task to run the following steps synchronously:
        HTML::queue_a_task(HTML::Task::Source::FontLoading, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise, matched_font_faces] {
            GC::RootVector<GC::Ref<WebIDL::Promise>> promises(realm.heap());

            // 1. For all of the font faces in the font face list, call their load() method.
            for (auto font_face_value : *matched_font_faces) {
                auto& font_face = as<FontFace>(font_face_value.key.as_object());
                font_face.load();

                promises.append(font_face.font_status_promise());
            }

            // 2. Resolve promise with the result of waiting for all of the [[FontStatusPromise]]s of each font face in
            //    the font face list, in order.
            HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            WebIDL::wait_for_all(
                realm, promises,
                [&realm, promise](auto const& fonts) {
                    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                    auto fonts_array = JS::Array::create_from(realm, fonts);
                    WebIDL::resolve_promise(realm, promise, fonts_array);
                },
                [&realm, promise](auto error) {
                    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                    WebIDL::reject_promise(realm, promise, error);
                });
        }));
    }));

    // 2. Return promise. Complete the rest of these steps asynchronously.
    return promise;
}

// https://drafts.csswg.org/css-font-loading/#font-face-set-ready
GC::Ref<WebIDL::Promise> FontFaceSet::ready() const
{
    return m_ready_promise;
}

// https://drafts.csswg.org/css-font-loading/#fire-a-font-load-event
void FontFaceSet::fire_a_font_load_event(FlyString name, Vector<GC::Ref<FontFace>> font_faces)
{
    // To fire a font load event named e at a FontFaceSet target with optional font faces means to fire a simple
    // event named e using the FontFaceSetLoadEvent interface that also meets these conditions:
    // 1. The fontfaces attribute is initialized to the result of filtering font faces to only contain FontFace
    //    objects contained in target.
    FontFaceSetLoadEventInit load_event_init {};
    for (auto const& font_face : font_faces) {
        if (set_entries()->set_has(font_face))
            load_event_init.fontfaces.append(font_face);
    }
    dispatch_event(FontFaceSetLoadEvent::create(realm(), move(name), move(load_event_init)));
}

// https://drafts.csswg.org/css-font-loading/#ref-for-fontfaceset-pending-on-the-environment%E2%91%A1
void FontFaceSet::set_is_pending_on_the_environment(bool is_pending_on_the_environment)
{
    if (is_pending_on_the_environment == m_is_pending_on_the_environment)
        return;
    m_is_pending_on_the_environment = is_pending_on_the_environment;

    // Whenever a FontFaceSet goes from pending on the environment to not pending on the environment, the user agent
    // must run the following steps:
    if (!is_pending_on_the_environment) {
        // 1. If the FontFaceSet is stuck on the environment and its [[LoadingFonts]] list is empty, switch the
        //    FontFaceSet to loaded.
        // FIXME: We also need to mark empty FontFaceSets as loaded, so that the [[ReadyPromise]] gets resolved.
        //        Spec issue: https://github.com/w3c/csswg-drafts/issues/13538#issuecomment-3933951987
        if (m_set_entries->set_size() == 0 || (m_is_stuck_on_the_environment && m_loading_fonts.is_empty()))
            switch_to_loaded();

        // 2. If the FontFaceSet is stuck on the environment, unmark it as such.
        m_is_stuck_on_the_environment = false;
    }
}

// https://drafts.csswg.org/css-font-loading/#switch-the-fontfaceset-to-loading
void FontFaceSet::switch_to_loading()
{
    // 1. Let font face set be the given FontFaceSet.
    // 2. Set the status attribute of font face set to "loading".
    m_status = Bindings::FontFaceSetLoadStatus::Loading;

    // 3. If font face set’s [[ReadyPromise]] slot currently holds a fulfilled promise, replace it with a fresh pending
    //    promise.
    if (WebIDL::is_promise_fulfilled(m_ready_promise))
        m_ready_promise = WebIDL::create_promise(realm());

    // 4. Queue a task to fire a font load event named loading at font face set.
    HTML::queue_a_task(HTML::Task::Source::FontLoading, nullptr, nullptr, GC::create_function(realm().heap(), [this] {
        fire_a_font_load_event("loading"_fly_string);
    }));
}

// https://drafts.csswg.org/css-font-loading/#switch-the-fontfaceset-to-loaded
void FontFaceSet::switch_to_loaded()
{
    // 1. Let font face set be the given FontFaceSet.
    // 2. If font face set is pending on the environment, mark it as stuck on the environment, and exit this algorithm.
    if (m_is_pending_on_the_environment) {
        m_is_stuck_on_the_environment = true;
        return;
    }

    // 3. Set font face set’s status attribute to "loaded".
    m_status = Bindings::FontFaceSetLoadStatus::Loaded;

    // 4. Fulfill font face set’s [[ReadyPromise]] attribute’s value with font face set.
    WebIDL::resolve_promise(realm(), m_ready_promise, this);

    // 5. Queue a task to perform the following steps synchronously:
    HTML::queue_a_task(HTML::Task::Source::FontLoading, nullptr, nullptr, GC::create_function(realm().heap(), [this] {
        // 1. Let loaded fonts be the (possibly empty) contents of font face set’s [[LoadedFonts]] slot.
        // 2. Let failed fonts be the (possibly empty) contents of font face set’s [[FailedFonts]] slot.
        // 3. Reset the [[LoadedFonts]] and [[FailedFonts]] slots to empty lists.
        auto loaded_fonts = move(m_loaded_fonts);
        auto failed_fonts = move(m_failed_fonts);

        // 4. Fire a font load event named loadingdone at font face set with loaded fonts.
        fire_a_font_load_event("loadingdone"_fly_string, move(loaded_fonts));

        // 5. If font face set’s failed fonts is non-empty, fire a font load event named loadingerror at font face set
        //    with failed fonts.
        if (!failed_fonts.is_empty())
            fire_a_font_load_event("loadingerror"_fly_string, move(failed_fonts));
    }));
}

}
