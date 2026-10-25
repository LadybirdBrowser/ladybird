/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Console.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Set.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::WebDriver::BiDi::BindingsGlue {

inline JS::Value wrap_node(HTML::EnvironmentSettingsObject& settings, DOM::Node& node)
{
    auto& realm = settings.realm();
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, GC::Ref { node });
}

inline GC::Ref<JS::Array> create_array(HTML::EnvironmentSettingsObject& settings, ReadonlySpan<JS::Value> values)
{
    return JS::Array::create_from(settings.realm(), values);
}

inline GC::Ref<JS::Date> create_date(HTML::EnvironmentSettingsObject& settings, double time)
{
    return JS::Date::create(settings.realm(), time);
}

inline GC::Ref<JS::Map> create_map(HTML::EnvironmentSettingsObject& settings)
{
    return JS::Map::create(settings.realm());
}

inline GC::Ref<JS::Set> create_set(HTML::EnvironmentSettingsObject& settings)
{
    return JS::Set::create(settings.realm());
}

inline GC::Ref<JS::Object> create_object(HTML::EnvironmentSettingsObject& settings)
{
    auto& realm = settings.realm();
    return JS::Object::create(realm, realm.intrinsics().object_prototype());
}

inline GC::Ref<JS::NativeFunction> create_channel_function(HTML::EnvironmentSettingsObject& settings, Function<JS::ThrowCompletionOr<JS::Value>(JS::VM&)> steps)
{
    return JS::NativeFunction::create(settings.realm(), move(steps), 1);
}

inline GC::Ref<JS::TypeError> create_preload_type_error(HTML::EnvironmentSettingsObject& settings)
{
    return JS::TypeError::create(settings.realm(), "Preload script is not a function"_utf16);
}

inline HTML::EnvironmentSettingsObject& console_settings(JS::Console& console)
{
    return HTML::principal_realm_settings_object(console.realm());
}

inline JS::Console& console_for(HTML::EnvironmentSettingsObject& settings)
{
    return settings.realm().intrinsics().console_object()->console();
}

}
