/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/HTML/NavigateParams.h>
#include <LibWeb/HTML/Navigation.h>

namespace Web::HTML {

void NavigateParams::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(response);
    visitor.visit(source_document);
    visitor.visit(source_element);
    visitor.visit(api_method_tracker);
    if (form_data_entry_list.has_value()) {
        for (auto& entry : form_data_entry_list.value()) {
            entry.value.visit([&](GC::Ref<FileAPI::File> const& file) { visitor.visit(file); },
                [&](auto const&) {});
        }
    }
}

void PreparedNavigation::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(response);
    visitor.visit(source_element);
    visitor.visit(api_method_tracker);
    visitor.visit(source_snapshot_params);
    if (form_data_entry_list.has_value()) {
        for (auto& entry : form_data_entry_list.value()) {
            entry.value.visit([&](GC::Ref<FileAPI::File> const& file) { visitor.visit(file); },
                [&](auto const&) {});
        }
    }
}

}
