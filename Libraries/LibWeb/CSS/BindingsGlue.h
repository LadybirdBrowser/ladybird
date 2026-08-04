/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/CSS/CSS.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CSS {

class CSSFontFeatureValuesMap;
class FontFace;
class FontFaceSet;

}

namespace Web::Bindings {

inline WebIDL::ExceptionOr<void> register_property(JS::Realm& realm, CSS::PropertyDefinition const& definition)
{
    auto& window = HTML::relevant_window(realm.global_object());
    return CSS::register_property(window.associated_document(), definition);
}

WEB_API GC::Ref<JS::Map> map_entries(JS::Realm&, CSS::CSSFontFeatureValuesMap&);
WEB_API Optional<JS::Value> map_get(JS::Realm&, CSS::CSSFontFeatureValuesMap&, FlyString const& key);
WEB_API bool map_has(CSS::CSSFontFeatureValuesMap&, FlyString const& key);
WEB_API bool map_remove(CSS::CSSFontFeatureValuesMap&, FlyString const& key);
WEB_API void map_clear(CSS::CSSFontFeatureValuesMap&);
WEB_API WebIDL::ExceptionOr<void> set(CSS::CSSFontFeatureValuesMap&, Utf16String const& feature_value_name, Variant<u32, Vector<u32>> const& values);

WEB_API void resolve_font_face_list_promise(JS::Realm&, WebIDL::Promise const&, Vector<GC::Ref<CSS::FontFace>> const&);
WEB_API void resolve_font_face_set_promise(JS::Realm&, WebIDL::Promise const&, CSS::FontFaceSet&);
WEB_API GC::Ref<JS::Set> setlike_entries(JS::Realm&, WrapperWorld const&, CSS::FontFaceSet const&);
WEB_API bool setlike_has(CSS::FontFaceSet const&, JS::Value);
WEB_API void did_add_font_face(CSS::FontFaceSet const&, GC::Ref<CSS::FontFace>);
WEB_API void did_remove_font_face(CSS::FontFaceSet const&, GC::Ref<CSS::FontFace>);

}
