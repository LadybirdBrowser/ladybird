/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Script.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Script);

Script::Script(Optional<URL::URL> base_url, ByteString filename, JS::Realm& realm)
    : m_base_url(move(base_url))
    , m_filename(move(filename))
    , m_realm(realm)
{
}

Script::~Script() = default;

// https://whatpr.org/html/9893/webappapis.html#settings-object
EnvironmentSettingsObject& Script::settings_object()
{
    // The settings object of a script is the settings object of the principal realm of the script's realm.
    return principal_realm_settings_object(principal_realm(realm()));
}

void Script::visit_host_defined_self(JS::Cell::Visitor& visitor)
{
    visitor.visit(*this);
}

void Script::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_parse_error);
    visitor.visit(m_error_to_rethrow);
}

}
