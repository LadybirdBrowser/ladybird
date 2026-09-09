/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <RequestServer/ResourceSubstitutionMap.h>
#include <RequestServer/Sandbox.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace RequestServer {

OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

TEST_CASE(resource_map_files_are_readable_without_granting_access_to_neighbors)
{
    if (syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION) < 1) {
        warnln("Skipping resource map sandbox test: Landlock is unavailable");
        return;
    }

    char directory_template[] = "/tmp/ladybird-resource-map-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);
    ScopeGuard cleanup = [&] {
        MUST(FileSystem::remove(ByteString { directory }, FileSystem::RecursionMode::Allowed));
    };

    auto mapped_path = ByteString::formatted("{}/mapped.txt", directory);
    auto unmapped_path = ByteString::formatted("{}/unmapped.txt", directory);
    auto map_path = ByteString::formatted("{}/resources.json", directory);
    auto cache_path = ByteString::formatted("{}/cache", directory);
    auto write_file = [](ByteString const& path, StringView content) {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(content.bytes()));
    };
    write_file(mapped_path, "mapped resource"sv);
    write_file(unmapped_path, "unmapped resource"sv);
    write_file(map_path, ByteString::formatted("{{\"substitutions\":[{{\"url\":\"https://example.com/\",\"file\":\"{}\"}}]}}", mapped_path));

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        RequestServer::g_resource_substitution_map = MUST(RequestServer::ResourceSubstitutionMap::load_from_file(map_path));
        MUST(RequestServer::apply_sandbox({}, cache_path));

        auto mapped_file = MUST(Core::File::open(mapped_path, Core::File::OpenMode::Read));
        auto contents = MUST(mapped_file->read_until_eof());
        VERIFY(StringView(contents) == "mapped resource"sv);

        VERIFY(open(unmapped_path.characters(), O_RDONLY) == -1);
        VERIFY(errno == EACCES);
        VERIFY(open(mapped_path.characters(), O_WRONLY) == -1);
        VERIFY(errno == EACCES || errno == EPERM);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}
