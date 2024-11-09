/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MimeTypePrototype.h>
#include <LibWeb/HTML/MimeType.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(MimeType);

MimeType::MimeType(JS::Realm& realm, String type)
    : Bindings::PlatformObject(realm)
    , m_type(move(type))
{
}

MimeType::~MimeType() = default;

void MimeType::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MimeType);
}

// https://html.spec.whatwg.org/multipage/system-state.html#concept-mimetype-type
String const& MimeType::type() const
{
    // The MimeType interface's type getter steps are to return this's type.
    return m_type;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-description
String MimeType::description() const
{
    // The MimeType interface's description getter steps are to return "Portable Document Format".
    static String description_string = "Portable Document Format"_string;
    return description_string;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-suffixes
String const& MimeType::suffixes() const
{
    // The MimeType interface's suffixes getter steps are to return "pdf".
    static String suffixes_string = "pdf"_string;
    return suffixes_string;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetype-enabledplugin
JS::NonnullGCPtr<Plugin> MimeType::enabled_plugin() const
{
    // The MimeType interface's enabledPlugin getter steps are to return this's relevant global object's PDF viewer plugin objects[0] (i.e., the generic "PDF Viewer" one).
    auto& window = verify_cast<HTML::Window>(HTML::relevant_global_object(*this));
    auto plugin_objects = window.pdf_viewer_plugin_objects();

    // NOTE: If a MimeType object was created, that means PDF viewer support is enabled, meaning there will be Plugin objects.
    VERIFY(!plugin_objects.is_empty());
    return plugin_objects.first();
}

}
