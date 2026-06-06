/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MediaSourceExtensions/MediaSourceHandle.h>

namespace Web::MediaSourceExtensions {

GC_DEFINE_ALLOCATOR(MediaSourceHandle);

MediaSourceHandle::MediaSourceHandle()
    : Bindings::Wrappable()
{
}

MediaSourceHandle::~MediaSourceHandle() = default;

}
