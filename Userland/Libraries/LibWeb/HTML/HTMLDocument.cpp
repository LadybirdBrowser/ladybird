/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLDocumentPrototype.h>
#include <LibWeb/HTML/HTMLDocument.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLDocument);

HTMLDocument::HTMLDocument(JS::Realm& realm, URL::URL const& url)
    : Document(realm, url)
{
}

HTMLDocument::~HTMLDocument() = default;

WebIDL::ExceptionOr<GC::Ref<HTMLDocument>> HTMLDocument::construct_impl(JS::Realm& realm)
{
    return HTMLDocument::create(realm);
}

GC::Ref<HTMLDocument> HTMLDocument::create(JS::Realm& realm, URL::URL const& url)
{
    return realm.heap().allocate<HTMLDocument>(realm, realm, url);
}

void HTMLDocument::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLDocument);
}

}
