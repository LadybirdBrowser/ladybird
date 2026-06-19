/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/Script.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Script);

void register_javascript_source(Script& script, NonnullRefPtr<JS::SourceCode const> source_code, ScriptRegistry::IsInlineSource is_inline_source, size_t source_line_number)
{
    auto document = script.settings_object().responsible_document();
    if (!document) {
        // FIXME: Register worker sources once DevTools can target workers.
        return;
    }

    auto source_length = is_inline_source == ScriptRegistry::IsInlineSource::Yes
        ? document->source().byte_count()
        : source_code->length_in_code_units();

    auto const& registered_script = document->script_registry().register_javascript_source(
        move(source_code),
        script.filename(),
        "scriptElement"_string,
        is_inline_source,
        source_line_number,
        source_length);

    if (document->page().client().has_active_devtools_client())
        document->page().client().page_did_register_javascript_source(*document, registered_script.description);
}

Script::Script(Optional<URL::URL> base_url, ByteString filename, EnvironmentSettingsObject& settings)
    : m_base_url(move(base_url))
    , m_filename(filename)
    , m_display_filename(Utf16String::from_utf8(filename))
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
