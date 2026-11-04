/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/MediaQueryList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::CSS {

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_media_query_list_create(JSVM*, void* realm, void* document, Parser::ValueParserFFI::MediaList const*);
bool web_media_query_list_has_change_listener(void* list);
void web_media_query_list_evaluate_and_report_change(JSVM*, void* list);

// Declared in Bindings/Rust/src/css/media_query_list.rs.
bool web_css_media_query_list_evaluate(void* document, Parser::ValueParserFFI::MediaList const*);
bool web_css_media_query_list_update_container_layout(void* document);
}

static DOM::Document& document_from_abi(void* document)
{
    return *static_cast<DOM::Document*>(static_cast<GC::Cell*>(document));
}

bool web_css_media_query_list_evaluate(void* document, Parser::ValueParserFFI::MediaList const* media)
{
    return RustMediaList { media, RustMediaList::Ownership::Borrow }.evaluate(document_from_abi(document));
}

bool web_css_media_query_list_update_container_layout(void* document)
{
    auto container_document = document_from_abi(document).container_document();
    if (!container_document)
        return false;
    container_document->update_layout(DOM::UpdateLayoutReason::MediaQueryListMatches);
    return true;
}

GC::Ref<MediaQueryList> MediaQueryList::create(DOM::Document& document, RustMediaList const& media)
{
    auto* list = web_media_query_list_create(JS::HostABI::vm_to_abi(JS::VM::the()), &HTML::relevant_realm(document), static_cast<GC::Cell*>(&document), media.handle());
    return GC::Ref { *static_cast<MediaQueryList*>(list) };
}

bool MediaQueryList::has_change_listener() const
{
    return web_media_query_list_has_change_listener(const_cast<MediaQueryList*>(this));
}

void MediaQueryList::evaluate_and_report_change()
{
    web_media_query_list_evaluate_and_report_change(JS::HostABI::vm_to_abi(JS::VM::the()), this);
}

}
