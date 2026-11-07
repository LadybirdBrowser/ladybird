/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Platform/ProcessResourceUsage.h>
#include <LibCore/Platform/ProcessStatistics.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <sys/mman.h>
#include <unistd.h>

static constexpr size_t mapping_size = 32 * MiB;

static u64 own_memory_usage()
{
    Core::Platform::ProcessStatistics statistics;
    statistics.processes.append(make<Core::Platform::ProcessInfo>(Core::System::getpid()));
    MUST(Core::Platform::update_process_statistics(statistics));
    return statistics.processes[0]->memory_usage_bytes;
}

static u64 own_attributed_memory()
{
    auto usage = Core::Platform::process_resource_usage(Core::Platform::ProcessInfo { Core::System::getpid() });
    VERIFY(usage.has_value());
    return usage->attributed_memory_bytes;
}

// NB: The task manager shows the memory usage of each process, and the per-tab performance monitor adds up the attributed
//     memory of a tab's processes, so the two have to agree on every process.
TEST_CASE(memory_usage_and_attributed_memory_agree)
{
    auto usage = own_memory_usage();
    auto attributed = own_attributed_memory();
    auto difference = usage > attributed ? usage - attributed : attributed - usage;
    EXPECT(difference < 4 * MiB);
}

TEST_CASE(anonymous_memory_counts)
{
    auto before_usage = own_memory_usage();
    auto before_attributed = own_attributed_memory();

    auto* memory = static_cast<u8*>(mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    VERIFY(memory != MAP_FAILED);
    __builtin_memset(memory, 1, mapping_size);

    EXPECT(own_memory_usage() >= before_usage + mapping_size / 2);
    EXPECT(own_attributed_memory() >= before_attributed + mapping_size / 2);

    VERIFY(munmap(memory, mapping_size) == 0);
}

TEST_CASE(shared_memory_counts)
{
    auto before_usage = own_memory_usage();
    auto before_attributed = own_attributed_memory();

    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(mapping_size));
    __builtin_memset(buffer.data<u8>(), 1, mapping_size);

    EXPECT(own_memory_usage() >= before_usage + mapping_size / 2);
    EXPECT(own_attributed_memory() >= before_attributed + mapping_size / 2);
}

TEST_CASE(file_backed_memory_does_not_count)
{
    // NB: The page cache shares these pages with every other process that maps the file, so they are no one's own.
    //     The file goes next to the executable, as a temporary directory may be in memory and so count as shared memory.
    auto path = ByteString::formatted("{}/.TestLibCoreProcessStatistics.{}", LexicalPath::dirname(MUST(Core::System::current_executable_path())), Core::System::getpid());
    auto fd = MUST(Core::System::open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600));
    MUST(Core::System::unlink(path));
    VERIFY(ftruncate(fd, mapping_size) == 0);
    auto* memory = static_cast<u8*>(mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    VERIFY(memory != MAP_FAILED);
    MUST(Core::System::close(fd));

    auto before_usage = own_memory_usage();
    auto before_attributed = own_attributed_memory();
    __builtin_memset(memory, 1, mapping_size);

    EXPECT(own_memory_usage() < before_usage + mapping_size / 2);
    EXPECT(own_attributed_memory() < before_attributed + mapping_size / 2);

    VERIFY(munmap(memory, mapping_size) == 0);
}
