/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/MediaError.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(MediaError);

GC::Ref<MediaError> MediaError::create(Code code, String message)
{
    return GC::Heap::the().allocate<MediaError>(code, move(message));
}

MediaError::MediaError(Code code, String message)
    : Bindings::Wrappable()
    , m_code(code)
    , m_message(move(message))
{
}

}
