/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/MediaError.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(MediaError);

MediaError::MediaError(JS::Realm& realm, Code code, String message)
    : Wrappable(realm)
    , m_code(code)
    , m_message(move(message))
{
}

}
