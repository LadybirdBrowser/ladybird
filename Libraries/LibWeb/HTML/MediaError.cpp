/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaErrorPrototype.h>
#include <LibWeb/HTML/MediaError.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(MediaError);

MediaError::MediaError(JS::Realm& realm, Code code, String message)
    : Base(realm)
    , m_code(code)
    , m_message(move(message))
{
}

void MediaError::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaError);
}

}
