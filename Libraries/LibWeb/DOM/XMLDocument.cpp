/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/XMLDocumentPrototype.h>
#include <LibWeb/DOM/XMLDocument.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(XMLDocument);

GC::Ref<XMLDocument> XMLDocument::create(JS::Realm& realm, URL::URL const& url)
{
    return realm.create<XMLDocument>(realm, url);
}

XMLDocument::XMLDocument(JS::Realm& realm, URL::URL const& url)
    : Document(realm, url)
{
}

void XMLDocument::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XMLDocument);
}

}
