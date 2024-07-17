/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FileWatcher.h"
#include <AK/Debug.h>
#include <AK/LexicalPath.h>
#include <AK/OwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <errno.h>
#include <limits.h>

#if !defined(AK_OS_MACOS)
static_assert(false, "This file must only be used for macOS");
#endif

#import <CoreServices/CoreServices.h>
#import <dispatch/dispatch.h>

namespace Core {

struct MonitoredPath {
    ByteString path;
    FileWatcherEvent::Type event_mask { FileWatcherEvent::Type::Invalid };
};

static void on_file_system_event(ConstFSEventStreamRef, void*, size_t, void*, FSEventStreamEventFlags const[], FSEventStreamEventId const[]);

static ErrorOr<ino_t> inode_id_from_path(StringView path)
{
    auto stat = TRY(System::stat(path));
    return stat.st_ino;
}

class FileWatcherMacOS final : public FileWatcher {
    AK_MAKE_NONCOPYABLE(FileWatcherMacOS);

public:
    virtual ~FileWatcherMacOS() override
    {
        close_event_stream();
        dispatch_release(m_dispatch_queue);
    }

    static ErrorOr<NonnullRefPtr<FileWatcherMacOS>> create(FileWatcherFlags)
    {
        auto context = TRY(try_make<FSEventStreamContext>());

        auto queue_name = ByteString::formatted("Serenity.FileWatcher.{:p}", context.ptr());
        auto dispatch_queue = dispatch_queue_create(queue_name.characters(), DISPATCH_QUEUE_SERIAL);
        if (dispatch_queue == nullptr)
            return Error::from_errno(errno);

        // NOTE: This isn't actually used on macOS, but is needed for FileWatcherBase.
        //       Creating it with an FD of -1 will effectively disable the notifier.
        auto notifier = TRY(Notifier::try_create(-1, Notifier::Type::None));

        return adopt_nonnull_ref_or_enomem(new (nothrow) FileWatcherMacOS(move(context), dispatch_queue, move(notifier)));
    }

    ErrorOr<bool> add_watch(ByteString path, FileWatcherEvent::Type event_mask)
    {
        if (m_path_to_inode_id.contains(path)) {
            dbgln_if(FILE_WATCHER_DEBUG, "add_watch: path '{}' is already being watched", path);
            return false;
        }

        auto inode_id = TRY(inode_id_from_path(path));
        TRY(m_path_to_inode_id.try_set(path, inode_id));
        TRY(m_inode_id_to_path.try_set(inode_id, { path, event_mask }));

        TRY(refresh_monitored_paths());

        dbgln_if(FILE_WATCHER_DEBUG, "add_watch: watching path '{}' inode {}", path, inode_id);
        return true;
    }

    ErrorOr<bool> remove_watch(ByteString path)
    {
        auto it = m_path_to_inode_id.find(path);
        if (it == m_path_to_inode_id.end()) {
            dbgln_if(FILE_WATCHER_DEBUG, "remove_watch: path '{}' is not being watched", path);
            return false;
        }

        m_inode_id_to_path.remove(it->value);
        m_path_to_inode_id.remove(it);

        TRY(refresh_monitored_paths());

        dbgln_if(FILE_WATCHER_DEBUG, "remove_watch: stopped watching path '{}'", path);
        return true;
    }

    ErrorOr<MonitoredPath> canonicalize_path(ByteString path)
    {
        LexicalPath lexical_path { move(path) };
        auto parent_path = lexical_path.parent();

        auto inode_id = TRY(inode_id_from_path(parent_path.string()));

        auto it = m_inode_id_to_path.find(inode_id);
        if (it == m_inode_id_to_path.end())
            return Error::from_string_literal("Got an event for a non-existent inode ID");

        return MonitoredPath {
            LexicalPath::join(it->value.path, lexical_path.basename()).string(),
            it->value.event_mask
        };
    }

    void handle_event(FileWatcherEvent event)
    {
        NonnullRefPtr strong_this { *this };

        m_main_event_loop.deferred_invoke(
            [strong_this = move(strong_this), event = move(event)]() {
                strong_this->on_change(event);
            });
    }

private:
    FileWatcherMacOS(NonnullOwnPtr<FSEventStreamContext> context, dispatch_queue_t dispatch_queue, NonnullRefPtr<Notifier> notifier)
        : FileWatcher(-1, move(notifier))
        , m_main_event_loop(EventLoop::current())
        , m_context(move(context))
        , m_dispatch_queue(dispatch_queue)
    {
        m_context->info = this;
    }

    void close_event_stream()
    {
        if (!m_stream)
            return;

        dispatch_sync(m_dispatch_queue, ^{
            FSEventStreamStop(m_stream);
            FSEventStreamInvalidate(m_stream);
            FSEventStreamRelease(m_stream);
            m_stream = nullptr;
        });
    }

    ErrorOr<void> refresh_monitored_paths()
    {
        static constexpr FSEventStreamCreateFlags stream_flags = kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagUseExtendedData;
        static constexpr CFAbsoluteTime stream_latency = 0.25;

        close_event_stream();

        if (m_path_to_inode_id.is_empty())
            return {};

        auto monitored_paths = CFArrayCreateMutable(kCFAllocatorDefault, m_path_to_inode_id.size(), &kCFTypeArrayCallBacks);
        if (monitored_paths == nullptr)
            return Error::from_errno(ENOMEM);

        for (auto it : m_path_to_inode_id) {
            auto path = CFStringCreateWithCString(kCFAllocatorDefault, it.key.characters(), kCFStringEncodingUTF8);
            if (path == nullptr)
                return Error::from_errno(ENOMEM);

            CFArrayAppendValue(monitored_paths, static_cast<void const*>(path));
        }

        dispatch_sync(m_dispatch_queue, ^{
            m_stream = FSEventStreamCreate(
                kCFAllocatorDefault,
                &on_file_system_event,
                m_context.ptr(),
                monitored_paths,
                kFSEventStreamEventIdSinceNow,
                stream_latency,
                stream_flags);

            if (m_stream) {
                FSEventStreamSetDispatchQueue(m_stream, m_dispatch_queue);
                FSEventStreamStart(m_stream);
            }
        });

        if (!m_stream)
            return Error::from_string_literal("Could not create an FSEventStream");
        return {};
    }

    EventLoop& m_main_event_loop;

    NonnullOwnPtr<FSEventStreamContext> m_context;
    dispatch_queue_t m_dispatch_queue { nullptr };
    FSEventStreamRef m_stream { nullptr };

    HashMap<ByteString, ino_t> m_path_to_inode_id;
    HashMap<ino_t, MonitoredPath> m_inode_id_to_path;
};

void on_file_system_event(ConstFSEventStreamRef, void* user_data, size_t event_size, void* event_paths, FSEventStreamEventFlags const event_flags[], FSEventStreamEventId const[])
{
    auto& file_watcher = *reinterpret_cast<FileWatcherMacOS*>(user_data);
    auto paths = reinterpret_cast<CFArrayRef>(event_paths);

    for (size_t i = 0; i < event_size; ++i) {
        auto path_dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(paths, static_cast<CFIndex>(i)));
        auto path = static_cast<CFStringRef>(CFDictionaryGetValue(path_dictionary, kFSEventStreamEventExtendedDataPathKey));

        char file_path_buffer[PATH_MAX] {};
        if (!CFStringGetFileSystemRepresentation(path, file_path_buffer, sizeof(file_path_buffer))) {
            dbgln_if(FILE_WATCHER_DEBUG, "Could not convert event to a file path");
            continue;
        }

        auto maybe_monitored_path = file_watcher.canonicalize_path(ByteString { file_path_buffer });
        if (maybe_monitored_path.is_error()) {
            dbgln_if(FILE_WATCHER_DEBUG, "Could not canonicalize path {}: {}", file_path_buffer, maybe_monitored_path.error());
            continue;
        }
        auto monitored_path = maybe_monitored_path.release_value();

        FileWatcherEvent event;
        event.event_path = move(monitored_path.path);

        auto flags = event_flags[i];
        if ((flags & kFSEventStreamEventFlagItemCreated) != 0)
            event.type |= FileWatcherEvent::Type::ChildCreated;
        if ((flags & kFSEventStreamEventFlagItemRemoved) != 0)
            event.type |= FileWatcherEvent::Type::ChildDeleted;
        if ((flags & kFSEventStreamEventFlagItemModified) != 0)
            event.type |= FileWatcherEvent::Type::ContentModified;
        if ((flags & kFSEventStreamEventFlagItemInodeMetaMod) != 0)
            event.type |= FileWatcherEvent::Type::MetadataModified;

        if (event.type == FileWatcherEvent::Type::Invalid) {
            dbgln_if(FILE_WATCHER_DEBUG, "Unknown event type {:x} returned by the FS event for {}", flags, path);
            continue;
        }
        if ((event.type & monitored_path.event_mask) == FileWatcherEvent::Type::Invalid) {
            dbgln_if(FILE_WATCHER_DEBUG, "Dropping unwanted FS event {} for {}", flags, path);
            continue;
        }

        file_watcher.handle_event(move(event));
    }
}

ErrorOr<NonnullRefPtr<FileWatcher>> FileWatcher::create(FileWatcherFlags flags)
{
    return TRY(FileWatcherMacOS::create(flags));
}

FileWatcher::FileWatcher(int watcher_fd, NonnullRefPtr<Notifier> notifier)
    : FileWatcherBase(watcher_fd)
    , m_notifier(move(notifier))
{
}

FileWatcher::~FileWatcher() = default;

ErrorOr<bool> FileWatcherBase::add_watch(ByteString path, FileWatcherEvent::Type event_mask)
{
    auto& file_watcher = verify_cast<FileWatcherMacOS>(*this);
    return file_watcher.add_watch(move(path), event_mask);
}

ErrorOr<bool> FileWatcherBase::remove_watch(ByteString path)
{
    auto& file_watcher = verify_cast<FileWatcherMacOS>(*this);
    return file_watcher.remove_watch(move(path));
}

}
