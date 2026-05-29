/*
 * Copyright (c) 2026-present, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/PCH.h>

// NOTE: Header files included everywhere:

#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibGC/Ptr.h>

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/CyclicModule.h>
#include <LibJS/Module.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ErrorTypes.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>

#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

// NOTE: Header files included a few hundred times but are stable:

#include <LibGfx/Font/Font.h>

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>

#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGImageElement.h>
