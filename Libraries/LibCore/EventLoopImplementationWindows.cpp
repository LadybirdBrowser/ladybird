/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/Diagnostics.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/MutexProtected.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <AK/Windows.h>
#include <AK/kmalloc.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventLoopImplementationWindows.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibCore/TimeoutSet.h>
#include <LibCore/Timer.h>
#include <mswsock.h>
#include <signal.h>

struct OwnHandle {
    HANDLE handle = NULL;

    explicit OwnHandle(HANDLE h = NULL)
        : handle(h)
    {
    }

    OwnHandle(OwnHandle&& h)
    {
        handle = h.handle;
        h.handle = NULL;
    }

    // This operation can only be done when handle is NULL
    OwnHandle& operator=(OwnHandle&& other)
    {
        VERIFY(!handle);
        if (this == &other)
            return *this;
        handle = other.handle;
        other.handle = NULL;
        return *this;
    }

    ~OwnHandle()
    {
        if (handle)
            CloseHandle(handle);
    }

    bool operator==(OwnHandle const& h) const { return handle == h.handle; }
    bool operator==(HANDLE h) const { return handle == h; }
};

template<>
struct Traits<OwnHandle> : DefaultTraits<OwnHandle> {
    static unsigned hash(OwnHandle const& h) { return Traits<HANDLE>::hash(h.handle); }
};
template<>
constexpr bool IsHashCompatible<HANDLE, OwnHandle> = true;

namespace Core {

enum class CompletionType : u8 {
    Wake,
    Timer,
    Socket,
    PipeNotifier,
    Signal,
};

struct CompletionPacket {
    AK_ALLOC_WITH_KMALLOC;

    CompletionType type;
};

static CompletionPacket s_signal_packet { CompletionType::Signal };
static Atomic<HANDLE> s_signal_iocp { nullptr };
static Array<Atomic<u32>, NSIG> s_pending_signal_counts;

struct SignalHandlers {
    AK_ALLOC_WITH_KMALLOC;

    HashMap<int, HashMap<int, Function<void(int)>>> handlers_by_signal;
    int next_handler_id { 0 };
};

static SignalHandlers& signal_handlers()
{
    static NeverDestroyed<SignalHandlers> s_signal_handlers;
    return *s_signal_handlers;
}

static void handle_signal(int signal_number)
{
    // The CRT resets the handler before calling it.
    (void)signal(signal_number, handle_signal);
    s_pending_signal_counts[signal_number].fetch_add(1, AK::MemoryOrder::memory_order_release);
    if (auto* iocp = s_signal_iocp.load(AK::MemoryOrder::memory_order_acquire))
        PostQueuedCompletionStatus(iocp, 0, reinterpret_cast<ULONG_PTR>(&s_signal_packet), nullptr);
}

static void dispatch_pending_signals()
{
    for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
        auto pending_count = s_pending_signal_counts[signal_number].exchange(0, AK::MemoryOrder::memory_order_acq_rel);
        for (u32 i = 0; i < pending_count; ++i) {
            auto handlers = signal_handlers().handlers_by_signal.get(signal_number);
            if (!handlers.has_value())
                break;
            // A handler may register or unregister handlers, so each one is looked up again before it is called.
            auto handler_ids = handlers->keys();
            for (auto handler_id : handler_ids) {
                auto current_handlers = signal_handlers().handlers_by_signal.get(signal_number);
                if (!current_handlers.has_value())
                    break;
                if (auto handler = current_handlers->get(handler_id); handler.has_value())
                    (*handler)(signal_number);
            }
        }
    }
}

struct EventLoopWake final : CompletionPacket {
    OwnHandle wait_packet;
    OwnHandle wait_event;
};

// All timers of a thread share one waitable timer, armed for the earliest deadline. Own bookkeeping (rather than a
// kernel timer object per Core timer) is used because the kernel delivers simultaneously signaled wait completion
// packets in LIFO order, while timers that are due together must fire in registration order.
struct EventLoopMasterTimer final : CompletionPacket {

    ~EventLoopMasterTimer()
    {
        CancelWaitableTimer(timer.handle);
        if (wait_packet_associated) {
            NTSTATUS status = g_system.NtCancelWaitCompletionPacket(wait_packet.handle, TRUE);
            VERIFY(NT_SUCCESS(status));
        }
    }

    OwnHandle timer;
    OwnHandle wait_packet;
    bool wait_packet_associated { false };
};

class EventLoopTimer final : public EventLoopTimeout {
public:
    AK_ALLOC_WITH_KMALLOC;

    EventLoopTimer() = default;

    void reload(MonotonicTime const& now) { m_fire_time = now + interval; }

    virtual void fire(TimeoutSet& timeout_set, MonotonicTime current_time) override
    {
        auto strong_owner = owner.strong_ref();

        if (!strong_owner)
            return;

        if (should_reload) {
            MonotonicTime next_fire_time = m_fire_time + interval;
            if (next_fire_time <= current_time) {
                next_fire_time = current_time + interval;
            }
            m_fire_time = next_fire_time;
            if (next_fire_time != current_time) {
                timeout_set.schedule_absolute(this);
            } else {
                // NOTE: Unfortunately we need to treat timeouts with the zero interval in a
                //       special way. TimeoutSet::schedule_absolute for them will result in an
                //       infinite loop. TimeoutSet::schedule_relative, on the other hand, will do a
                //       correct thing of scheduling them for the next iteration of the loop.
                m_duration = {};
                timeout_set.schedule_relative(this);
            }
        }

        ThreadEventQueue::current().post_event(strong_owner, Event::Type::Timer);
    }

    AK::Duration interval;
    bool should_reload { false };
    WeakPtr<EventReceiver> owner;
};

// The Ancillary Function Driver's poll request, which select() and WSAPoll() are built on, is not in the Windows SDK.
// https://github.com/piscisaureus/wepoll/blob/master/src/afd.h
static constexpr DWORD IOCTL_AFD_POLL = 0x00012024;

static constexpr ULONG AFD_POLL_RECEIVE = 0x0001;
static constexpr ULONG AFD_POLL_SEND = 0x0004;
static constexpr ULONG AFD_POLL_DISCONNECT = 0x0008;
static constexpr ULONG AFD_POLL_ABORT = 0x0010;
static constexpr ULONG AFD_POLL_LOCAL_CLOSE = 0x0020;
static constexpr ULONG AFD_POLL_ACCEPT = 0x0080;
static constexpr ULONG AFD_POLL_CONNECT_FAIL = 0x0100;

struct AFD_POLL_HANDLE_INFO {
    HANDLE Handle;
    ULONG Events;
    NTSTATUS Status;
};

struct AFD_POLL_INFO {
    LARGE_INTEGER Timeout;
    ULONG NumberOfHandles;
    ULONG Exclusive;
    AFD_POLL_HANDLE_INFO Handles[];
};

struct EventLoopSocket {
    AK_ALLOC_WITH_KMALLOC;

    int fd { -1 };
    // The socket that AFD polls, which differs from the socket itself when a layered service provider is installed.
    SOCKET base_socket { INVALID_SOCKET };
    Vector<Notifier*, 2> notifiers;
    // The socket was closed under its notifiers, so it is no longer polled.
    bool is_closed { false };
};

struct PolledSocket {
    SOCKET base_socket;
    // Null once the socket's last notifier is unregistered while it is polled.
    EventLoopSocket* socket;
};

struct EventLoopPipeNotifier final : CompletionPacket {
    Notifier* notifier;
    OwnHandle wait_packet;
    OwnHandle wait_event;
    HANDLE pipe_handle { nullptr };
    OVERLAPPED pipe_read {};
    bool pipe_read_is_pending { false };
};

struct EventLoopProcess {
    AK_ALLOC_WITH_KMALLOC;

    OwnHandle process;
    pid_t pid;
    ULONG_PTR completion_key;
    Function<void(pid_t)> exit_handler;
    OwnHandle jobobject;
};

static ULONG_PTR allocate_process_completion_key()
{
    static Atomic<ULONG_PTR> s_next_key { 1 };
    return s_next_key.fetch_add(2, AK::MemoryOrder::memory_order_relaxed);
}

static bool is_process_completion_key(ULONG_PTR key)
{
    return key & 1;
}

struct ThreadData {
    AK_ALLOC_WITH_KMALLOC;

    static ThreadData* the();

    ThreadData()
        : master_timer(make<EventLoopMasterTimer>())
        , wake_data(make<EventLoopWake>())
    {
        wake_data->type = CompletionType::Wake;
        wake_data->wait_event.handle = CreateEvent(NULL, FALSE, FALSE, NULL);

        // Consider a way for different event loops to have a different number of threads
        iocp.handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
        VERIFY(iocp.handle);

        NTSTATUS status = g_system.NtCreateWaitCompletionPacket(&wake_data->wait_packet.handle, GENERIC_READ | GENERIC_WRITE, NULL);
        VERIFY(NT_SUCCESS(status));
        status = g_system.NtAssociateWaitCompletionPacket(wake_data->wait_packet.handle, iocp.handle, wake_data->wait_event.handle, wake_data.ptr(), NULL, 0, 0, NULL);
        VERIFY(NT_SUCCESS(status));

        master_timer->type = CompletionType::Timer;
        // A high-resolution timer avoids rounding short deadlines up to the default ~16ms timer tick.
        master_timer->timer.handle = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!master_timer->timer.handle)
            master_timer->timer.handle = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        VERIFY(master_timer->timer.handle);
        status = g_system.NtCreateWaitCompletionPacket(&master_timer->wait_packet.handle, GENERIC_READ | GENERIC_WRITE, NULL);
        VERIFY(NT_SUCCESS(status));
    }
    ~ThreadData()
    {
        NTSTATUS status = g_system.NtCancelWaitCompletionPacket(wake_data->wait_packet.handle, TRUE);
        VERIFY(NT_SUCCESS(status));
    }

    OwnHandle iocp;

    // A handle to AFD that sockets are polled through, opened when the first socket is watched. Its polls complete to
    // the completion port with socket_poll_packet as their key.
    OwnHandle afd;
    CompletionPacket socket_poll_packet { CompletionType::Socket };

    // All sockets are polled at once, and only while the loop waits, as with poll(). AFD completes only the oldest of
    // the polls pending on a socket, so a poll left pending between waits would starve any other wait on the socket.
    OVERLAPPED socket_poll {};
    // An AFD_POLL_INFO with a handle for each polled socket. AFD writes the ready sockets back to it, in order.
    Vector<u8> socket_poll_buffer;
    Vector<PolledSocket, 8> polled_sockets;
    bool socket_poll_is_pending { false };

    HashMap<int, NonnullOwnPtr<EventLoopSocket>> sockets;
    // These are only used to register and unregister. The event loop doesn't access these.
    HashMap<Notifier*, EventLoopSocket*> socket_notifiers;
    HashMap<Notifier*, NonnullOwnPtr<EventLoopPipeNotifier>> pipe_notifiers;

    // Pipe notifiers whose activation has been posted. Each reads its pipe again only on the next pump, once its
    // activation has been handled, so that it activates again only if there is still something to read, as a poll()
    // would report.
    Vector<EventLoopPipeNotifier*, 8> pipe_notifiers_to_rearm;

    // Owns the timer allocations; ids stay valid until unregister_timer even after a one-shot timer fires.
    HashMap<intptr_t, NonnullOwnPtr<EventLoopTimer>> timers;
    TimeoutSet timeouts;
    // The deadline the master timer is currently armed for, to skip redundant SetWaitableTimer calls.
    Optional<MonotonicTime> armed_deadline;
    NonnullOwnPtr<EventLoopMasterTimer> master_timer;

    // The wake completion packet is posted to the thread's event loop to wake it.
    NonnullOwnPtr<EventLoopWake> wake_data;
};

static thread_local ThreadData* s_thread_data;
static thread_local bool s_thread_data_was_destroyed;

struct ThreadDataOwner {
    ~ThreadDataOwner()
    {
        ThreadEventQueue::destroy_current();
        s_thread_data_was_destroyed = true;
        delete exchange(s_thread_data, nullptr);
    }
};

static thread_local ThreadDataOwner s_thread_data_owner;

ThreadData* ThreadData::the()
{
    if (!s_thread_data && !s_thread_data_was_destroyed) {
        (void)s_thread_data_owner;
        s_thread_data = new ThreadData;
    }
    return s_thread_data;
}

static MutexProtected<HashMap<pid_t, NonnullOwnPtr<EventLoopProcess>>> s_processes;

// Arms (or disarms) the thread's shared waitable timer for the earliest pending deadline.
static void arm_master_timer(ThreadData& thread_data)
{
    auto& master_timer = *thread_data.master_timer;

    auto earliest = thread_data.timeouts.next_timer_expiration();

    if (!earliest.has_value()) {
        if (thread_data.armed_deadline.has_value()) {
            CancelWaitableTimer(master_timer.timer.handle);
            thread_data.armed_deadline = {};
        }
        return;
    }

    if (thread_data.armed_deadline == earliest)
        return;

    LARGE_INTEGER due_time = {};
    // Measured in 0.1μs intervals; negative means relative to now. Round up so the timer doesn't fire
    // before the deadline (and wake the loop with nothing due), and clamp to at least one interval so
    // an already-passed deadline stays a relative time.
    due_time.QuadPart = -AK::max<i64>(((*earliest - MonotonicTime::now()).to_nanoseconds() + 99) / 100, 1);
    BOOL succeeded = SetWaitableTimer(master_timer.timer.handle, &due_time, 0, NULL, NULL, FALSE);
    VERIFY(succeeded);
    thread_data.armed_deadline = earliest;

    if (!master_timer.wait_packet_associated) {
        NTSTATUS status = g_system.NtAssociateWaitCompletionPacket(master_timer.wait_packet.handle, thread_data.iocp.handle, master_timer.timer.handle, &master_timer, NULL, 0, 0, NULL);
        VERIFY(NT_SUCCESS(status));
        master_timer.wait_packet_associated = true;
    }
}

EventLoopImplementationWindows::EventLoopImplementationWindows()
    : m_wake_event(ThreadData::the()->wake_data->wait_event.handle)
{
    VERIFY(m_wake_event);
}

EventLoopImplementationWindows::~EventLoopImplementationWindows()
{
}

int EventLoopImplementationWindows::exec()
{
    for (;;) {
        if (auto exit_code = exit_code_if_requested(); exit_code.has_value())
            return exit_code.release_value();
        pump(PumpMode::WaitForEvents);
    }
    VERIFY_NOT_REACHED();
}

static constexpr bool debug_event_loop = false;

static void handle_process_message(OVERLAPPED_ENTRY const& entry)
{
    // A job delivers its message identifier in place of the byte count, which propagates through lpNumberOfBytes to
    // dwNumberOfBytesTransferred.
    // See: https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_associate_completion_port#remarks
    DWORD const event_type = entry.dwNumberOfBytesTransferred;
    if (event_type != JOB_OBJECT_MSG_EXIT_PROCESS && event_type != JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS)
        return;

    // A job also reports the exits of its process's descendants, which may have jobs of their own, so the exit is only
    // taken from the job that was registered for the exiting process.
    auto const process_id = static_cast<pid_t>(reinterpret_cast<intptr_t>(entry.lpOverlapped));
    auto owned_process = s_processes.with_locked([&](auto& processes) -> Optional<NonnullOwnPtr<EventLoopProcess>> {
        auto process = processes.get(process_id);
        if (!process.has_value() || process.value()->completion_key != entry.lpCompletionKey)
            return {};
        return processes.take(process_id);
    });
    if (!owned_process.has_value())
        return;

    // Run the handler from the event queue rather than inline: it may unregister notifiers whose completion packets
    // were dequeued into this very batch and are not yet re-associated, which would fail the packet cancellation (and
    // leave dangling pointers in the batch).
    deferred_invoke([process = owned_process.release_value(), process_id] {
        process->exit_handler(process_id);
    });
}

static ULONG socket_poll_events(EventLoopSocket const& socket)
{
    // As with poll(), hang-ups and errors are always reported. A socket that is closed while it is polled completes its
    // poll.
    ULONG events = AFD_POLL_ABORT | AFD_POLL_CONNECT_FAIL | AFD_POLL_LOCAL_CLOSE;
    for (auto* notifier : socket.notifiers) {
        if (has_flag(notifier->type(), NotificationType::Read))
            events |= AFD_POLL_RECEIVE | AFD_POLL_ACCEPT | AFD_POLL_DISCONNECT;
        if (has_flag(notifier->type(), NotificationType::Write))
            events |= AFD_POLL_SEND;
    }
    return events;
}

static NotificationType afd_poll_events_to_notification_type(ULONG events)
{
    // This follows EventLoopImplementationUnix's translation of poll() events.
    auto type = NotificationType::None;
    if (events & (AFD_POLL_RECEIVE | AFD_POLL_ACCEPT | AFD_POLL_DISCONNECT))
        type |= NotificationType::Read;
    if (events & AFD_POLL_SEND)
        type |= NotificationType::Write;
    if (events & AFD_POLL_ABORT)
        type |= NotificationType::Read | NotificationType::Write | NotificationType::HangUp;
    if (events & AFD_POLL_CONNECT_FAIL)
        type |= NotificationType::Read | NotificationType::Write | NotificationType::Error;
    return type;
}

static void open_afd(ThreadData& thread_data)
{
    // Any name under \Device\Afd opens a handle that polls can be issued through.
    static constexpr wchar_t device_name[] = L"\\Device\\Afd\\Ladybird";
    UNICODE_STRING name {
        .Length = sizeof(device_name) - sizeof(wchar_t),
        .MaximumLength = sizeof(device_name),
        .Buffer = const_cast<PWSTR>(device_name),
    };
    OBJECT_ATTRIBUTES attributes {};
    attributes.Length = sizeof(attributes);
    attributes.ObjectName = &name;
    IO_STATUS_BLOCK io_status {};
    NTSTATUS status = g_system.NtCreateFile(&thread_data.afd.handle, SYNCHRONIZE, &attributes, &io_status, NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
    VERIFY(NT_SUCCESS(status));
    VERIFY(CreateIoCompletionPort(thread_data.afd.handle, thread_data.iocp.handle, reinterpret_cast<ULONG_PTR>(&thread_data.socket_poll_packet), 0));
}

static AFD_POLL_INFO& socket_poll_info(ThreadData& thread_data)
{
    return *reinterpret_cast<AFD_POLL_INFO*>(thread_data.socket_poll_buffer.data());
}

static bool socket_is_closed(SOCKET socket)
{
    int type = 0;
    int type_size = sizeof(type);
    return getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &type_size) == SOCKET_ERROR
        && WSAGetLastError() == WSAENOTSOCK;
}

// Polls all sockets with notifiers for the events those notifiers are waiting for, until finish_socket_poll().
static void start_socket_poll(ThreadData& thread_data, bool should_wait)
{
    VERIFY(!thread_data.socket_poll_is_pending);
    for (;;) {
        thread_data.polled_sockets.clear_with_capacity();
        for (auto& [fd, socket] : thread_data.sockets) {
            if (!socket->is_closed)
                thread_data.polled_sockets.append({ socket->base_socket, socket.ptr() });
        }
        if (thread_data.polled_sockets.is_empty())
            return;

        auto buffer_size = sizeof(AFD_POLL_INFO) + (thread_data.polled_sockets.size() * sizeof(AFD_POLL_HANDLE_INFO));
        thread_data.socket_poll_buffer.resize(buffer_size);
        auto& poll_info = socket_poll_info(thread_data);
        poll_info.Timeout.QuadPart = should_wait ? NumericLimits<i64>::max() : 0;
        poll_info.NumberOfHandles = thread_data.polled_sockets.size();
        poll_info.Exclusive = FALSE;
        for (size_t i = 0; i < thread_data.polled_sockets.size(); i++) {
            auto& polled_socket = thread_data.polled_sockets[i];
            poll_info.Handles[i] = {
                .Handle = reinterpret_cast<HANDLE>(polled_socket.base_socket),
                .Events = socket_poll_events(*polled_socket.socket),
                .Status = 0,
            };
        }

        thread_data.socket_poll = {};
        // A poll whose events have already occurred completes at once, but its completion is still queued.
        if (DeviceIoControl(thread_data.afd.handle, IOCTL_AFD_POLL, &poll_info, buffer_size, &poll_info, buffer_size, NULL, &thread_data.socket_poll)
            || GetLastError() == ERROR_IO_PENDING) {
            thread_data.socket_poll_is_pending = true;
            return;
        }

        // The whole poll fails if any of its sockets was closed under its notifiers. Like poll() with POLLNVAL, those
        // never activate their notifiers, so they are left out of the poll.
        bool found_closed_socket = false;
        for (auto& polled_socket : thread_data.polled_sockets) {
            if (socket_is_closed(polled_socket.base_socket)) {
                polled_socket.socket->is_closed = true;
                found_closed_socket = true;
            }
        }
        if (!found_closed_socket) {
            dbgln("Failed to poll sockets: {}", Error::from_windows_error());
            return;
        }
    }
}

static void handle_socket_poll_completion(ThreadData& thread_data, ThreadEventQueue& event_queue)
{
    thread_data.socket_poll_is_pending = false;

    // A cancelled poll reports no events.
    auto status = static_cast<NTSTATUS>(thread_data.socket_poll.Internal);
    if (!NT_SUCCESS(status))
        return;

    auto& poll_info = socket_poll_info(thread_data);
    size_t polled_socket_index = 0;
    for (ULONG i = 0; i < poll_info.NumberOfHandles; i++) {
        auto& result = poll_info.Handles[i];
        while (polled_socket_index < thread_data.polled_sockets.size() && thread_data.polled_sockets[polled_socket_index].base_socket != reinterpret_cast<SOCKET>(result.Handle))
            polled_socket_index++;
        if (polled_socket_index == thread_data.polled_sockets.size())
            break;

        auto* socket = thread_data.polled_sockets[polled_socket_index].socket;
        if (!socket)
            continue;
        auto type = afd_poll_events_to_notification_type(result.Events);
        for (auto* notifier : socket->notifiers) {
            if ((type & notifier->type()) != NotificationType::None)
                event_queue.post_event(notifier, Core::Event::Type::NotifierActivation);
        }
    }
}

static bool handle_is_overlapped(HANDLE handle)
{
    constexpr ULONG file_mode_information = 16;
    constexpr ULONG synchronous_io_alert = 0x10;
    constexpr ULONG synchronous_io_nonalert = 0x20;

    IO_STATUS_BLOCK io_status {};
    ULONG mode = 0;
    NTSTATUS status = g_system.NtQueryInformationFile(handle, &io_status, &mode, sizeof(mode), file_mode_information);
    VERIFY(NT_SUCCESS(status));
    return !(mode & (synchronous_io_alert | synchronous_io_nonalert));
}

static void read_pipe_until_readable(EventLoopPipeNotifier& notifier_data)
{
    static u8 s_zero_byte_buffer;
    notifier_data.pipe_read = {};
    notifier_data.pipe_read.hEvent = notifier_data.wait_event.handle;
    notifier_data.pipe_read_is_pending = false;
    if (ReadFile(notifier_data.pipe_handle, &s_zero_byte_buffer, 0, nullptr, &notifier_data.pipe_read))
        return;
    if (GetLastError() == ERROR_IO_PENDING) {
        notifier_data.pipe_read_is_pending = true;
        return;
    }
    // A read that fails at once, as one does at the end of the pipe, does not set the event, but the reader must still
    // be activated to see it.
    SetEvent(notifier_data.wait_event.handle);
}

static void cancel_pipe_read(EventLoopPipeNotifier& notifier_data)
{
    if (!notifier_data.pipe_read_is_pending || HasOverlappedIoCompleted(&notifier_data.pipe_read))
        return;
    CancelIoEx(notifier_data.pipe_handle, &notifier_data.pipe_read);
    // The read must have stopped writing to pipe_read before it can be freed.
    while (!HasOverlappedIoCompleted(&notifier_data.pipe_read))
        WaitForSingleObject(notifier_data.wait_event.handle, INFINITE);
}

static void handle_completion(ThreadData& thread_data, OVERLAPPED_ENTRY const& entry, ThreadEventQueue& event_queue)
{
    if (is_process_completion_key(entry.lpCompletionKey)) {
        handle_process_message(entry);
        return;
    }
    auto* packet = reinterpret_cast<CompletionPacket*>(entry.lpCompletionKey);

    if (packet->type == CompletionType::Signal) {
        dispatch_pending_signals();
        return;
    }
    if (packet->type == CompletionType::Wake) {
        auto* wake_data = static_cast<EventLoopWake*>(packet);
        NTSTATUS status = g_system.NtAssociateWaitCompletionPacket(wake_data->wait_packet.handle, thread_data.iocp.handle, wake_data->wait_event.handle, wake_data, NULL, 0, 0, NULL);
        VERIFY(NT_SUCCESS(status));
        return;
    }
    if (packet->type == CompletionType::Timer) {
        auto* master_timer = static_cast<EventLoopMasterTimer*>(packet);
        master_timer->wait_packet_associated = false;
        thread_data.armed_deadline = {};

        // TimeoutSet fires due timers in registration order for equal deadlines. (This cannot rely on
        // kernel wakeup order: simultaneously signaled wait completion packets are delivered in LIFO
        // order.)
        auto now = MonotonicTime::now();
        thread_data.timeouts.fire_expired(now);
        // Zero-interval reloads are rescheduled relative to defer them to the next pump.
        thread_data.timeouts.absolutize_relative_timeouts(now);

        arm_master_timer(thread_data);
        return;
    }
    if (packet->type == CompletionType::Socket) {
        handle_socket_poll_completion(thread_data, event_queue);
        return;
    }
    if (packet->type == CompletionType::PipeNotifier) {
        auto* notifier_data = static_cast<EventLoopPipeNotifier*>(packet);
        event_queue.post_event(notifier_data->notifier, Core::Event::Type::NotifierActivation);
        NTSTATUS status = g_system.NtAssociateWaitCompletionPacket(notifier_data->wait_packet.handle, thread_data.iocp.handle, notifier_data->wait_event.handle, notifier_data, NULL, 0, 0, NULL);
        VERIFY(NT_SUCCESS(status));
        thread_data.pipe_notifiers_to_rearm.append(notifier_data);
        return;
    }
    VERIFY_NOT_REACHED();
}

// NOTE: The number of entries to dequeue is to be optimized. Ideally we always dequeue all outstanding packets,
// but we don't want to increase the cost of each pump unnecessarily. If more than one entry is never dequeued
// at once, we could switch to using GetQueuedCompletionStatus which directly returns the values.
static constexpr ULONG completion_entry_count = 32;

// Cancels the socket poll if it is still pending, and waits for it to complete so that its results can be reported
// and its buffer reused. Completions dequeued meanwhile are handled as usual.
static void finish_socket_poll(ThreadData& thread_data, ThreadEventQueue& event_queue)
{
    if (!thread_data.socket_poll_is_pending)
        return;
    CancelIoEx(thread_data.afd.handle, &thread_data.socket_poll);
    while (thread_data.socket_poll_is_pending) {
        OVERLAPPED_ENTRY entries[completion_entry_count];
        ULONG entries_removed = 0;
        VERIFY(GetQueuedCompletionStatusEx(thread_data.iocp.handle, entries, completion_entry_count, &entries_removed, INFINITE, FALSE));
        for (ULONG i = 0; i < entries_removed; i++)
            handle_completion(thread_data, entries[i], event_queue);
    }
}

size_t EventLoopImplementationWindows::pump(PumpMode pump_mode)
{
    auto& event_queue = ThreadEventQueue::current();
    auto* thread_data = ThreadData::the();

    for (auto* notifier_data : thread_data->pipe_notifiers_to_rearm)
        read_pipe_until_readable(*notifier_data);
    thread_data->pipe_notifiers_to_rearm.clear();

    OVERLAPPED_ENTRY entries[completion_entry_count];
    ULONG entries_removed = 0;

    bool has_pending_events = event_queue.has_pending_events();
    DWORD timeout = 0;
    if (!has_pending_events && pump_mode == PumpMode::WaitForEvents)
        timeout = INFINITE;

    start_socket_poll(*thread_data, timeout != 0);

    BOOL success = GetQueuedCompletionStatusEx(thread_data->iocp.handle, entries, completion_entry_count, &entries_removed, timeout, FALSE);
    dbgln_if(debug_event_loop, "Event loop dequed {} events", entries_removed);

    if (success) {
        for (ULONG i = 0; i < entries_removed; i++)
            handle_completion(*thread_data, entries[i], event_queue);
    } else {
        DWORD error = GetLastError();
        switch (error) {
        case WAIT_TIMEOUT:
            break;
        default:
            dbgln("GetQueuedCompletionStatusEx failed with unexpected error: {}", Error::from_windows_error(error));
            VERIFY_NOT_REACHED();
        }
    }

    finish_socket_poll(*thread_data, event_queue);

    return event_queue.process();
}

void EventLoopImplementationWindows::quit(int code)
{
    request_exit(code);
}

void EventLoopImplementationWindows::wake()
{
    SetEvent(m_wake_event);
}

static SOCKET base_socket(int fd)
{
    SOCKET base = INVALID_SOCKET;
    DWORD bytes = 0;
    if (WSAIoctl(static_cast<SOCKET>(fd), SIO_BASE_HANDLE, NULL, 0, &base, sizeof(base), &bytes, NULL, NULL) == SOCKET_ERROR)
        return static_cast<SOCKET>(fd);
    return base;
}

static void register_socket_notifier(ThreadData& thread_data, Notifier& notifier)
{
    if (!thread_data.afd.handle)
        open_afd(thread_data);

    auto& socket = *thread_data.sockets.ensure(notifier.fd(), [&] {
        auto socket = make<EventLoopSocket>();
        socket->fd = notifier.fd();
        return socket;
    });
    // A socket that was closed under its notifiers may have had its handle reused since.
    if (socket.notifiers.is_empty() || socket.is_closed) {
        socket.base_socket = base_socket(socket.fd);
        socket.is_closed = false;
        // WSAEventSelect(), which sockets used to be watched with, made them non-blocking, so this does too.
        u_long non_blocking = 1;
        (void)ioctlsocket(static_cast<SOCKET>(socket.fd), FIONBIO, &non_blocking);
    }
    socket.notifiers.append(&notifier);
    thread_data.socket_notifiers.set(&notifier, &socket);
}

static void unregister_socket_notifier(ThreadData& thread_data, Notifier& notifier, EventLoopSocket& socket)
{
    socket.notifiers.remove_first_matching([&](auto* entry) { return entry == &notifier; });
    if (!socket.notifiers.is_empty())
        return;

    // A signal handler may unregister a notifier while its socket is polled.
    for (auto& polled_socket : thread_data.polled_sockets) {
        if (polled_socket.socket == &socket)
            polled_socket.socket = nullptr;
    }
    thread_data.sockets.remove(socket.fd);
}

static void register_pipe_notifier(ThreadData& thread_data, Notifier& notifier)
{
    auto* handle = to_handle(notifier.fd());
    VERIFY(!has_flag(notifier.type(), NotificationType::Write));
    // A zero-byte read of a pipe that is not overlapped would block until there is data. Pipes from pipe2() are
    // overlapped unless they are read by another process.
    VERIFY(handle_is_overlapped(handle));

    auto notifier_data = make<EventLoopPipeNotifier>();
    notifier_data->type = CompletionType::PipeNotifier;
    notifier_data->notifier = &notifier;
    notifier_data->wait_event.handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    VERIFY(notifier_data->wait_event.handle);
    notifier_data->pipe_handle = handle;
    NTSTATUS status = g_system.NtCreateWaitCompletionPacket(&notifier_data->wait_packet.handle, GENERIC_READ | GENERIC_WRITE, NULL);
    VERIFY(NT_SUCCESS(status));
    status = g_system.NtAssociateWaitCompletionPacket(notifier_data->wait_packet.handle, thread_data.iocp.handle, notifier_data->wait_event.handle, notifier_data.ptr(), NULL, 0, 0, NULL);
    VERIFY(NT_SUCCESS(status));
    read_pipe_until_readable(*notifier_data);
    thread_data.pipe_notifiers.set(&notifier, move(notifier_data));
}

static void unregister_pipe_notifier(ThreadData& thread_data, NonnullOwnPtr<EventLoopPipeNotifier> notifier_data)
{
    // We are removing the signalled packets since the caller no longer expects them
    NTSTATUS status = g_system.NtCancelWaitCompletionPacket(notifier_data->wait_packet.handle, TRUE);
    VERIFY(NT_SUCCESS(status));
    thread_data.pipe_notifiers_to_rearm.remove_first_matching([&](auto* entry) { return entry == notifier_data.ptr(); });
    cancel_pipe_read(*notifier_data);
}

void EventLoopManagerWindows::register_notifier(Notifier& notifier)
{
    auto* thread_data = ThreadData::the();
    if (thread_data->socket_notifiers.contains(&notifier) || thread_data->pipe_notifiers.contains(&notifier))
        return;

    if (!System::is_socket(notifier.fd()) && GetFileType(to_handle(notifier.fd())) == FILE_TYPE_PIPE)
        register_pipe_notifier(*thread_data, notifier);
    else
        register_socket_notifier(*thread_data, notifier);
}

void EventLoopManagerWindows::unregister_notifier(Notifier& notifier)
{
    auto* thread_data = ThreadData::the();
    if (!thread_data)
        return;

    if (auto notifier_data = thread_data->pipe_notifiers.take(&notifier); notifier_data.has_value())
        unregister_pipe_notifier(*thread_data, notifier_data.release_value());
    else if (auto socket = thread_data->socket_notifiers.take(&notifier); socket.has_value())
        unregister_socket_notifier(*thread_data, notifier, **socket);
}

intptr_t EventLoopManagerWindows::register_timer(EventReceiver& object, AK::Duration interval, bool should_reload)
{
    VERIFY(!interval.is_negative());
    auto* thread_data = ThreadData::the();
    VERIFY(thread_data);

    auto timer = make<EventLoopTimer>();
    timer->owner = object.make_weak_ptr();
    timer->interval = interval;
    timer->should_reload = should_reload;
    timer->reload(MonotonicTime::now());
    thread_data->timeouts.schedule_absolute(timer.ptr());

    // Ids are process-wide unique so a stale id (e.g. one unregistered from the wrong thread) can never
    // match another thread's timer.
    static Atomic<intptr_t> next_timer_id { 1 };
    auto timer_id = next_timer_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    thread_data->timers.set(timer_id, move(timer));
    arm_master_timer(*thread_data);
    return timer_id;
}

void EventLoopManagerWindows::unregister_timer(intptr_t timer_id)
{
    if (auto* thread_data = ThreadData::the()) {
        auto timer = thread_data->timers.take(timer_id);
        if (!timer.has_value())
            return;
        if ((*timer)->is_scheduled())
            thread_data->timeouts.unschedule(timer->ptr());
        arm_master_timer(*thread_data);
    }
}

int EventLoopManagerWindows::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number > 0 && signal_number < NSIG);

    HANDLE expected = nullptr;
    // Only the first thread to register a handler becomes the signals' target.
    (void)s_signal_iocp.compare_exchange_strong(expected, ThreadData::the()->iocp.handle, AK::MemoryOrder::memory_order_acq_rel);

    auto& handlers = signal_handlers();
    auto handler_id = ++handlers.next_handler_id;
    auto& handlers_of_signal = handlers.handlers_by_signal.ensure(signal_number);
    if (handlers_of_signal.is_empty())
        VERIFY(signal(signal_number, handle_signal) != SIG_ERR);
    handlers_of_signal.set(handler_id, move(handler));
    return handler_id;
}

void EventLoopManagerWindows::unregister_signal(int handler_id)
{
    VERIFY(handler_id != 0);

    auto& handlers = signal_handlers();
    for (auto& [signal_number, handlers_of_signal] : handlers.handlers_by_signal) {
        if (!handlers_of_signal.remove(handler_id))
            continue;
        if (handlers_of_signal.is_empty()) {
            auto signal_number_to_reset = signal_number;
            (void)signal(signal_number_to_reset, SIG_DFL);
            handlers.handlers_by_signal.remove(signal_number_to_reset);
        }
        return;
    }
}

void EventLoopManagerWindows::register_process(pid_t pid, ESCAPING Function<void(pid_t)> exit_handler)
{
    auto* thread_data = ThreadData::the();
    VERIFY(thread_data);

    s_processes.with_locked([&](auto& processes) {
        if (processes.contains(pid))
            return;

        HANDLE process_handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        VERIFY(process_handle);

        HANDLE job_object_handle = CreateJobObject(nullptr, nullptr);
        VERIFY(job_object_handle);

        BOOL succeeded = AssignProcessToJobObject(job_object_handle, process_handle);
        VERIFY(succeeded);

        auto process_data = make<EventLoopProcess>();
        process_data->process.handle = process_handle;
        process_data->pid = pid;
        process_data->completion_key = allocate_process_completion_key();
        process_data->exit_handler = move(exit_handler);
        process_data->jobobject.handle = job_object_handle;

        JOBOBJECT_ASSOCIATE_COMPLETION_PORT joacp = { .CompletionKey = reinterpret_cast<PVOID>(process_data->completion_key), .CompletionPort = thread_data->iocp.handle };
        succeeded = SetInformationJobObject(job_object_handle, JobObjectAssociateCompletionPortInformation, &joacp, sizeof(JOBOBJECT_ASSOCIATE_COMPLETION_PORT));
        VERIFY(succeeded);

        processes.set(pid, move(process_data));
    });
}

void EventLoopManagerWindows::unregister_process(pid_t pid)
{
    auto maybe_process = s_processes.with_locked([&](auto& processes) {
        return processes.take(pid);
    });
    if (!maybe_process.has_value())
        return;

    auto process_data = maybe_process.release_value();
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT joacp = { .CompletionKey = nullptr, .CompletionPort = nullptr };
    BOOL succeeded = SetInformationJobObject(process_data->jobobject.handle, JobObjectAssociateCompletionPortInformation, &joacp, sizeof(JOBOBJECT_ASSOCIATE_COMPLETION_PORT));
    VERIFY(succeeded);
}

void EventLoopManagerWindows::did_post_event()
{
}

NonnullOwnPtr<EventLoopImplementation> EventLoopManagerWindows::make_implementation()
{
    return make<EventLoopImplementationWindows>();
}

}
