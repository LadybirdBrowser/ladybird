/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/Export.h>
#include <LibCore/Forward.h>

namespace Core {

// Owned by the process's main thread. Sampling and events run on that thread's event loop.
class CORE_API StatsLog {
    AK_MAKE_NONCOPYABLE(StatsLog);
    AK_MAKE_NONMOVABLE(StatsLog);

public:
    AK_ALLOC_WITH_KMALLOC;

    // An empty directory leaves logging disabled, without opening a file or creating a timer.
    static ErrorOr<OwnPtr<StatsLog>> create(StringView directory, StringView process_name);
    static StatsLog* the();
    ~StatsLog();

    StringView directory() const { return m_directory; }
    StringView path() const { return m_path; }
    void start_sampling(Function<void(JsonObject&)> = {});
    void event(StringView name, JsonObject data);

private:
    StatsLog(ByteString directory, ByteString path, ByteString process_name, NonnullOwnPtr<File>);
    void sample();
    void write_record(JsonObject);

    ByteString m_directory;
    ByteString m_path;
    ByteString m_process_name;
    NonnullOwnPtr<File> m_file;
    RefPtr<Timer> m_timer;
    Function<void(JsonObject&)> m_collect_values;
};

}
