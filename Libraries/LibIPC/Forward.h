/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/Platform.h>

namespace IPC {

class Attachment;
class AutoCloseFileDescriptor;
class Decoder;
class Encoder;
class Message;
class MessageBuffer;
class File;
class HandleBroker;
struct HandleBrokerRequest;
struct HandleBrokerResponse;
class HandleBrokerServer;
class LocalHandleBroker;
class Stub;
class TransportHandle;

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
class MachBootstrapListener;
class TransportMachPort;
using Transport = TransportMachPort;
#elif !defined(AK_OS_WINDOWS)
class TransportSocket;
using Transport = TransportSocket;
#else
class TransportWindows;
using Transport = TransportWindows;
#endif

template<typename T>
ErrorOr<void> encode(Encoder&, T const&);

template<typename T>
ErrorOr<T> decode(Decoder&);

// NB: Declared here so that a codec specialized for one enum, wherever it is declared, specializes these and not the
//     templates above.
template<Enum T>
ErrorOr<void> encode(Encoder&, T const&);

template<Enum T>
ErrorOr<T> decode(Decoder&);

using MessageDataType = Vector<u8, 1024>;

}
