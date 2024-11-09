/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/XHR/XMLHttpRequestEventTarget.h>

namespace Web::XHR {

class XMLHttpRequestUpload : public XMLHttpRequestEventTarget {
    WEB_PLATFORM_OBJECT(XMLHttpRequestUpload, XMLHttpRequestEventTarget);
    JS_DECLARE_ALLOCATOR(XMLHttpRequestUpload);

public:
    virtual ~XMLHttpRequestUpload() override;

private:
    XMLHttpRequestUpload(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
