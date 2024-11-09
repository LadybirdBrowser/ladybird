/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>

namespace Core {

class AnonymousBuffer;
class ArgsParser;
class BufferedSocketBase;
class ChildEvent;
class ConfigFile;
class CustomEvent;
class DateTime;
class DirIterator;
class DeferredInvocationContext;
class ElapsedTimer;
class Event;
class EventLoop;
class EventReceiver;
class File;
class LocalServer;
class LocalSocket;
class MappedFile;
class MimeData;
class NetworkJob;
class NetworkResponse;
class Notifier;
class Process;
class ProcessStatisticsReader;
class Resource;
class ResourceImplementation;
class Socket;
template<typename Result, typename TError = AK::Error>
class Promise;
template<typename Result, typename TError = AK::Error>
class ThreadedPromise;
class SocketAddress;
class TCPServer;
class TCPSocket;
class Timer;
class TimerEvent;
class TimeZoneWatcher;
class UDPServer;
class UDPSocket;

enum class TimerShouldFireWhenNotVisible;

#ifdef AK_OS_MACH
class MachPort;
#endif

}
