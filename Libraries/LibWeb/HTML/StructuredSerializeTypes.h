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

enum class SerializeType : u8 {
    Unknown = 0,
    DOMException = 1,
    DOMRectReadOnly = 2,
    DOMRect = 3,
    Blob = 4,
    ImageBitmap = 5,
    CryptoKey = 6,
    File = 7,
    FileList = 8,
    DOMMatrixReadOnly = 9,
    DOMMatrix = 10,
    DOMPointReadOnly = 11,
    DOMPoint = 12,
    DOMQuad = 13,
    ImageData = 14,
    QuotaExceededError = 15,
};

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
