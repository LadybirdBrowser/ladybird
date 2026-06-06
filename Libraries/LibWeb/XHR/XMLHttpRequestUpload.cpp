/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/XMLHttpRequestUpload.h>
#include <LibWeb/XHR/XMLHttpRequestUpload.h>

namespace Web::XHR {

GC_DEFINE_ALLOCATOR(XMLHttpRequestUpload);

XMLHttpRequestUpload::XMLHttpRequestUpload()
    : XMLHttpRequestEventTarget()
{
}

XMLHttpRequestUpload::~XMLHttpRequestUpload() = default;

}
