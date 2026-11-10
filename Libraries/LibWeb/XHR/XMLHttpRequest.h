/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/XMLHttpRequest.h>

namespace Web::XHR {

// https://xhr.spec.whatwg.org/#xmlhttprequest
class XMLHttpRequest final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(XMLHttpRequest, xml_http_request)

public:
    [[nodiscard]] static GC::Ref<XMLHttpRequest> create(JS::Realm&);
};

}
