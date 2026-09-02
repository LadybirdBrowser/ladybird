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

namespace Web::HTML {

void NavigateParams::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(response);
    visitor.visit(source_document);
    visitor.visit(source_element);
    if (form_data_entry_list.has_value()) {
        for (auto& entry : form_data_entry_list.value()) {
            entry.value.visit([&](GC::Ref<FileAPI::File> const& file) { visitor.visit(file); },
                [&](auto const&) {});
        }
    }
}

}
