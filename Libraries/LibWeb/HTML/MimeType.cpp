/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MimeType.h>
#include <LibWeb/HTML/MimeType.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(MimeType);

MimeType::MimeType(JS::Realm& realm, Utf16FlyString type)
    : Bindings::PlatformObject(realm)
    , m_type(move(type))
{
}

MimeType::~MimeType() = default;

void MimeType::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MimeType);
    Base::initialize(realm);
}

// https://html.spec.whatwg.org/multipage/system-state.html#concept-mimetype-type
Utf16FlyString const& MimeType::type() const
{
    // The MimeType interface's type getter steps are to return this's type.
    return m_type;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-description
Utf16FlyString MimeType::description() const
{
    // The MimeType interface's description getter steps are to return "Portable Document Format".
    return "Portable Document Format"_utf16_fly_string;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-suffixes
Utf16FlyString MimeType::suffixes() const
{
    // The MimeType interface's suffixes getter steps are to return "pdf".
    return "pdf"_utf16_fly_string;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-enabledplugin
GC::Ref<Plugin> MimeType::enabled_plugin() const
{
    // The MimeType interface's enabledPlugin getter steps are to return this's relevant global object's PDF viewer plugin objects[0] (i.e., the generic "PDF Viewer" one).
    auto& window = as<HTML::Window>(HTML::relevant_global_object(*this));
    auto plugin_objects = window.pdf_viewer_plugin_objects();

    // NOTE: If a MimeType object was created, that means PDF viewer support is enabled, meaning there will be Plugin objects.
    VERIFY(!plugin_objects.is_empty());
    return plugin_objects.first();
}

}
