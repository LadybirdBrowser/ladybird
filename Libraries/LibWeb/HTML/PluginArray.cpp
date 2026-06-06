/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/PluginArray.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PluginArray);

PluginArray::PluginArray(JS::Realm& realm)
    : Bindings::Wrappable(realm)
{
}

PluginArray::~PluginArray() = default;

// https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-refresh
void PluginArray::refresh() const
{
    // The PluginArray interface's refresh() method steps are to do nothing.
}

// https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewing-support:support-named-properties
Vector<FlyString> PluginArray::supported_property_names() const
{
    // The PluginArray interface supports named properties. If the user agent's PDF viewer supported is true, then they are the PDF viewer plugin names. Otherwise, they are the empty list.
    auto const& window = HTML::relevant_window(*this);
    if (!window.page().pdf_viewer_supported())
        return {};

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewer-plugin-names
    static NeverDestroyed<Vector<FlyString>> plugin_names { Vector<FlyString> {
        "PDF Viewer"_fly_string,
        "Chrome PDF Viewer"_fly_string,
        "Chromium PDF Viewer"_fly_string,
        "Microsoft Edge PDF Viewer"_fly_string,
        "WebKit built-in PDF"_fly_string,
    } };

    return *plugin_names;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-length
size_t PluginArray::length() const
{
    // The PluginArray interface's length getter steps are to return this's relevant global object's PDF viewer plugin objects's size.
    auto& window = HTML::relevant_window(*this);
    return window.pdf_viewer_plugin_objects().size();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-item
GC::Ptr<Plugin> PluginArray::item(u32 index) const
{
    // 1. Let plugins be this's relevant global object's PDF viewer plugin objects.
    auto& window = HTML::relevant_window(*this);
    auto plugins = window.pdf_viewer_plugin_objects();

    // 2. If index < plugins's size, then return plugins[index].
    if (index < plugins.size())
        return plugins[index];

    // 3. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-pluginarray-nameditem
GC::Ptr<Plugin> PluginArray::named_item(FlyString const& name) const
{
    // 1. For each Plugin plugin of this's relevant global object's PDF viewer plugin objects: if plugin's name is name, then return plugin.
    auto& window = HTML::relevant_window(*this);
    auto plugins = window.pdf_viewer_plugin_objects();

    for (auto& plugin : plugins) {
        if (plugin->name() == name)
            return plugin;
    }

    // 2. Return null.
    return nullptr;
}

Optional<JS::Value> PluginArray::item_value(JS::Realm& realm, size_t index) const
{
    auto return_value = item(index);
    if (!return_value)
        return {};
    return Bindings::wrap(realm, return_value).ptr();
}

JS::Value PluginArray::named_item_value(JS::Realm& realm, FlyString const& name) const
{
    auto return_value = named_item(name);
    if (!return_value)
        return JS::js_null();
    return Bindings::wrap(realm, return_value).ptr();
}

}
