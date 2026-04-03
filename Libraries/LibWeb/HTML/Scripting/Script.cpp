/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Script.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Script);

Script::Script(Optional<URL::URL> base_url, ByteString filename, EnvironmentSettingsObject& settings)
    : m_base_url(move(base_url))
    , m_filename(move(filename))
    , m_settings(settings)
{
}

Script::~Script() = default;

EnvironmentSettingsObject& Script::settings_object()
{
    return m_settings;
}

void Script::visit_host_defined_self(JS::Cell::Visitor& visitor)
{
    visitor.visit(*this);
}

void Script::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_settings);
    visitor.visit(m_parse_error);
    visitor.visit(m_error_to_rethrow);
}

}
