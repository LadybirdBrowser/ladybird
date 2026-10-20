/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StatsLog.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>

#if !__has_feature(address_sanitizer) && !defined(__SANITIZE_ADDRESS__) && !defined(HAS_THREAD_SANITIZER)
#    include <mimalloc.h>
#endif

#if defined(__GLIBC__)
#    if __GLIBC_PREREQ(2, 33)
#        include <malloc.h>
#        define HAVE_MALLINFO2 1
#    endif
#endif

namespace Core {

// NB: Not a static data member, since a thread-local member of a DLL-exported class cannot be exported.
static constinit thread_local StatsLog* s_instance = nullptr;

StatsLog* StatsLog::the()
{
    return s_instance;
}

ErrorOr<OwnPtr<StatsLog>> StatsLog::create(StringView directory, StringView process_name)
{
    if (directory.is_empty())
        return OwnPtr<StatsLog> {};
    if (!LexicalPath(directory).is_absolute())
        return Error::from_string_literal("--stats-log-dir must be an absolute path");
    VERIFY(!s_instance);
    if (auto result = Directory::create(directory, Directory::CreateDirectories::Yes); result.is_error()) {
        warnln("Could not create stats log directory '{}': {}", directory, result.error());
        return result.release_error();
    }
    auto path = ByteString::formatted("{}/{}.{}.jsonl", directory, process_name, System::getpid());
    auto file = File::open(path, File::OpenMode::Write | File::OpenMode::Append | File::OpenMode::NoFollow | File::OpenMode::Nonblocking, 0600);
    if (file.is_error()) {
        warnln("Could not open stats log '{}': {}", path, file.error());
        return file.release_error();
    }
    if (!S_ISREG(TRY(file.value()->stat()).st_mode)) {
        warnln("Stats log '{}' is not a regular file", path);
        return Error::from_string_literal("Stats log must be a regular file");
    }
    return adopt_own(*new StatsLog(directory, move(path), process_name, file.release_value()));
}

StatsLog::StatsLog(ByteString directory, ByteString path, ByteString process_name, NonnullOwnPtr<File> file)
    : m_directory(move(directory))
    , m_path(move(path))
    , m_process_name(move(process_name))
    , m_file(move(file))
{
    s_instance = this;
    warnln("Stats log: {}", m_path);
}

StatsLog::~StatsLog()
{
    if (m_timer)
        m_timer->stop();
    s_instance = nullptr;
}

void StatsLog::start_sampling(Function<void(JsonObject&)> collect_values)
{
    VERIFY(!m_timer);
    m_collect_values = move(collect_values);
    m_timer = Timer::create_repeating(AK::Duration::from_milliseconds(100), [this] { sample(); });
    m_timer->start();
    sample();
}

void StatsLog::sample()
{
    JsonObject values;
#if !__has_feature(address_sanitizer) && !defined(__SANITIZE_ADDRESS__) && !defined(HAS_THREAD_SANITIZER)
    size_t current_rss = 0, peak_rss = 0, current_commit = 0, peak_commit = 0;
    mi_process_info(nullptr, nullptr, nullptr, &current_rss, &peak_rss, &current_commit, &peak_commit, nullptr);
    values.set("mimalloc.current_rss_bytes"sv, current_rss);
    values.set("mimalloc.peak_rss_bytes"sv, peak_rss);
    values.set("mimalloc.current_commit_bytes"sv, current_commit);
    values.set("mimalloc.peak_commit_bytes"sv, peak_commit);
#endif
#ifdef HAVE_MALLINFO2
    auto info = mallinfo2();
    values.set("malloc.arena_bytes"sv, info.arena);
    values.set("malloc.allocated_bytes"sv, info.uordblks);
    values.set("malloc.free_bytes"sv, info.fordblks);
    values.set("malloc.mmap_bytes"sv, info.hblkhd);
#endif
    if (m_collect_values)
        m_collect_values(values);
    JsonObject record;
    record.set("type"sv, "sample"sv);
    record.set("values"sv, move(values));
    write_record(move(record));
}

void StatsLog::event(StringView name, JsonObject data)
{
    JsonObject record;
    record.set("type"sv, "event"sv);
    record.set("name"sv, name);
    record.set("data"sv, move(data));
    write_record(move(record));
}

void StatsLog::write_record(JsonObject record)
{
    record.set("time_ns"sv, MonotonicTime::now().nanoseconds());
    record.set("process"sv, m_process_name.view());
    record.set("pid"sv, System::getpid());
    StringBuilder builder;
    record.serialize(builder);
    builder.append('\n');
    if (auto result = m_file->write_until_depleted(builder.string_view().bytes()); result.is_error()) {
        warnln("Could not write stats log '{}': {}", m_path, result.error());
        // A partial timeline must not be mistaken for a successful measurement.
        EventLoop::current().quit(1);
    }
}

}
