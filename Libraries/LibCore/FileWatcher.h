/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2021-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/EnumBits.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibCore/Notifier.h>

namespace Core {

struct FileWatcherEvent {
    enum class Type {
        Invalid = 0,
        MetadataModified = 1 << 0,
        ContentModified = 1 << 1,
        Deleted = 1 << 2,
        ChildCreated = 1 << 3,
        ChildDeleted = 1 << 4,
        DoNotFollowLink = 1 << 5,
    };
    Type type { Type::Invalid };
    ByteString event_path;
};

AK_ENUM_BITWISE_OPERATORS(FileWatcherEvent::Type);

enum class FileWatcherFlags : u32 {
    None = 0,
    Nonblock = 1 << 0,
    CloseOnExec = 1 << 1,
};

AK_ENUM_BITWISE_OPERATORS(FileWatcherFlags);

class FileWatcherBase {
public:
    virtual ~FileWatcherBase() = default;

    ErrorOr<bool> add_watch(ByteString const& path, FileWatcherEvent::Type event_mask);
    ErrorOr<bool> remove_watch(ByteString const& path);
    bool is_watching(ByteString const& path) const { return m_path_to_wd.find(path) != m_path_to_wd.end(); }

protected:
    FileWatcherBase(int watcher_fd)
        : m_watcher_fd(watcher_fd)
    {
    }

    int m_watcher_fd { -1 };
    HashMap<ByteString, unsigned> m_path_to_wd;
    HashMap<unsigned, ByteString> m_wd_to_path;
};

class BlockingFileWatcher final : public FileWatcherBase {
    AK_MAKE_NONCOPYABLE(BlockingFileWatcher);

public:
    explicit BlockingFileWatcher(FileWatcherFlags = FileWatcherFlags::None);
    ~BlockingFileWatcher();

    Optional<FileWatcherEvent> wait_for_event();
};

class FileWatcher : public FileWatcherBase
    , public RefCounted<FileWatcher> {
    AK_MAKE_NONCOPYABLE(FileWatcher);

public:
    static ErrorOr<NonnullRefPtr<FileWatcher>> create(FileWatcherFlags = FileWatcherFlags::None);
    ~FileWatcher();

    Function<void(FileWatcherEvent const&)> on_change;

protected:
    FileWatcher(int watcher_fd, NonnullRefPtr<Notifier>);

    NonnullRefPtr<Notifier> m_notifier;
};

}

namespace AK {

template<>
struct Formatter<Core::FileWatcherEvent> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Core::FileWatcherEvent const& value)
    {
        return Formatter<FormatString>::format(builder, "FileWatcherEvent(\"{}\", {})"sv, value.event_path, value.type);
    }
};

template<>
struct Formatter<Core::FileWatcherEvent::Type> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Core::FileWatcherEvent::Type const& value)
    {
        bool had_any_flag = false;

        auto put_string_if_has_flag = [&](auto mask, auto name) -> ErrorOr<void> {
            if (!has_flag(value, mask))
                return {};

            if (had_any_flag)
                TRY(builder.put_string(", "sv));
            TRY(builder.put_string(name));

            had_any_flag = true;
            return {};
        };

        TRY(builder.put_string("["sv));
        TRY(put_string_if_has_flag(Core::FileWatcherEvent::Type::ChildCreated, "ChildCreated"sv));
        TRY(put_string_if_has_flag(Core::FileWatcherEvent::Type::ChildDeleted, "ChildDeleted"sv));
        TRY(put_string_if_has_flag(Core::FileWatcherEvent::Type::Deleted, "Deleted"sv));
        TRY(put_string_if_has_flag(Core::FileWatcherEvent::Type::ContentModified, "ContentModified"sv));
        TRY(put_string_if_has_flag(Core::FileWatcherEvent::Type::MetadataModified, "MetadataModified"sv));
        TRY(builder.put_string("]"sv));

        VERIFY(had_any_flag);
        return {};
    }
};

}
