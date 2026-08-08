/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/XHR/XMLHttpRequestEventTarget.h>

namespace Web::XHR {

class XMLHttpRequestUpload : public XMLHttpRequestEventTarget {
    WEB_WRAPPABLE(XMLHttpRequestUpload, XMLHttpRequestEventTarget);
    GC_DECLARE_ALLOCATOR(XMLHttpRequestUpload);

public:
    virtual ~XMLHttpRequestUpload() override;

private:
    XMLHttpRequestUpload();
};

}
