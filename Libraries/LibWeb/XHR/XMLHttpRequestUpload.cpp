/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/XMLHttpRequestUploadPrototype.h>
#include <LibWeb/XHR/XMLHttpRequestUpload.h>

namespace Web::XHR {

JS_DEFINE_ALLOCATOR(XMLHttpRequestUpload);

XMLHttpRequestUpload::XMLHttpRequestUpload(JS::Realm& realm)
    : XMLHttpRequestEventTarget(realm)
{
}

XMLHttpRequestUpload::~XMLHttpRequestUpload() = default;

void XMLHttpRequestUpload::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XMLHttpRequestUpload);
}

}
