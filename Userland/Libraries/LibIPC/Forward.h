/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>

namespace IPC {

class Decoder;
class Encoder;
class Message;
class MessageBuffer;
class File;
class Stub;

template<typename T>
ErrorOr<void> encode(Encoder&, T const&);

template<typename T>
ErrorOr<T> decode(Decoder&);

}
