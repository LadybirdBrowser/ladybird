/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibJS/Runtime/Object.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/web-messaging.html#structuredserializeoptions
struct StructuredSerializeOptions {
    Vector<JS::Handle<JS::Object>> transfer;
};

}
