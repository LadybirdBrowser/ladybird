/*
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibIPC/Forward.h>
#include <LibJS/Forward.h>

namespace Web::HTML {

using DeserializationMemory = GC::RootVector<JS::Value>;
using SerializationMemory = HashMap<GC::Root<JS::Value>, u32>;
using SerializationRecord = IPC::MessageDataType;

enum class TransferType : u8 {
    Unknown = 0,
    MessagePort = 1,
    ArrayBuffer = 2,
    ResizableArrayBuffer = 3,
    ReadableStream = 4,
    WritableStream = 5,
    TransformStream = 6,
    ImageBitmap = 7,
};

}
