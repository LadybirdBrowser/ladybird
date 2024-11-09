/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PluginArrayPrototype.h>
#include <LibWeb/HTML/PluginArray.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(PluginArray);

PluginArray::PluginArray(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
        .supports_named_properties = true,
        .has_legacy_unenumerable_named_properties_interface_extended_attribute = true,
    };
}

PluginArray::~PluginArray() = default;

void PluginArray::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PluginArray);
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-refresh
void PluginArray::refresh() const
{
    // The PluginArray interface's refresh() method steps are to do nothing.
}

// https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewing-support:support-named-properties
Vector<FlyString> PluginArray::supported_property_names() const
{
    // The PluginArray interface supports named properties. If the user agent's PDF viewer supported is true, then they are the PDF viewer plugin names. Otherwise, they are the empty list.
    auto const& window = verify_cast<HTML::Window>(HTML::relevant_global_object(*this));
    if (!window.page().pdf_viewer_supported())
        return {};

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewer-plugin-names
    static Vector<FlyString> const plugin_names = {
        "PDF Viewer"_fly_string,
        "Chrome PDF Viewer"_fly_string,
        "Chromium PDF Viewer"_fly_string,
        "Microsoft Edge PDF Viewer"_fly_string,
        "WebKit built-in PDF"_fly_string,
    };

    return plugin_names;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-length
size_t PluginArray::length() const
{
    // The PluginArray interface's length getter steps are to return this's relevant global object's PDF viewer plugin objects's size.
    auto& window = verify_cast<HTML::Window>(HTML::relevant_global_object(*this));
    return window.pdf_viewer_plugin_objects().size();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-item
JS::GCPtr<Plugin> PluginArray::item(u32 index) const
{
    // 1. Let plugins be this's relevant global object's PDF viewer plugin objects.
    auto& window = verify_cast<HTML::Window>(HTML::relevant_global_object(*this));
    auto plugins = window.pdf_viewer_plugin_objects();

    // 2. If index < plugins's size, then return plugins[index].
    if (index < plugins.size())
        return plugins[index];

    // 3. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-nameditem
JS::GCPtr<Plugin> PluginArray::named_item(FlyString const& name) const
{
    // 1. For each Plugin plugin of this's relevant global object's PDF viewer plugin objects: if plugin's name is name, then return plugin.
    auto& window = verify_cast<HTML::Window>(HTML::relevant_global_object(*this));
    auto plugins = window.pdf_viewer_plugin_objects();

    for (auto& plugin : plugins) {
        if (plugin->name() == name)
            return plugin;
    }

    // 2. Return null.
    return nullptr;
}

Optional<JS::Value> PluginArray::item_value(size_t index) const
{
    auto return_value = item(index);
    if (!return_value)
        return {};
    return return_value.ptr();
}

JS::Value PluginArray::named_item_value(FlyString const& name) const
{
    auto return_value = named_item(name);
    if (!return_value)
        return JS::js_null();
    return return_value.ptr();
}

}
