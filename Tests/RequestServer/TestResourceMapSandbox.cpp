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
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

// The sandbox refuses to apply without Landlock, so there is nothing to test on such a kernel.
static bool landlock_is_available()
{
    if (syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION) >= 1)
        return true;
    warnln("Skipping resource map sandbox test: Landlock is unavailable");
    return false;
}

TEST_CASE(resource_map_files_are_readable_without_granting_access_to_neighbors)
{
    if (!landlock_is_available())
        return;

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
        MUST(RequestServer::apply_sandbox({}, {}, cache_path));

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

TEST_CASE(certificate_files_are_readable_without_granting_access_to_neighbors)
{
    if (!landlock_is_available())
        return;

    char directory_template[] = "/tmp/ladybird-certificate-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);
    ScopeGuard cleanup = [&] {
        MUST(FileSystem::remove(ByteString { directory }, FileSystem::RecursionMode::Allowed));
    };

    auto certificate_path = ByteString::formatted("{}/certificate.pem", directory);
    auto key_path = ByteString::formatted("{}/key.pem", directory);
    auto cache_path = ByteString::formatted("{}/cache", directory);
    auto write_file = [](ByteString const& path, StringView content) {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(content.bytes()));
    };
    write_file(certificate_path, "certificate"sv);
    write_file(key_path, "key"sv);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        MUST(RequestServer::apply_sandbox({}, { certificate_path }, cache_path));

        auto certificate_file = MUST(Core::File::open(certificate_path, Core::File::OpenMode::Read));
        auto contents = MUST(certificate_file->read_until_eof());
        VERIFY(StringView(contents) == "certificate"sv);

        VERIFY(open(key_path.characters(), O_RDONLY) == -1);
        VERIFY(errno == EACCES);
        VERIFY(open(directory, O_RDONLY | O_DIRECTORY) == -1);
        VERIFY(errno == EACCES);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(certificate_directory_links_are_readable_without_granting_access_to_neighbors)
{
    if (!landlock_is_available())
        return;

    char directory_template[] = "/tmp/ladybird-certificate-directory-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);
    ScopeGuard cleanup = [&] {
        MUST(FileSystem::remove(ByteString { directory }, FileSystem::RecursionMode::Allowed));
    };

    // Distributions keep the certificates outside of the directory that OpenSSL searches, which only links to them.
    auto store_path = ByteString::formatted("{}/store", directory);
    auto shared_path = ByteString::formatted("{}/shared", directory);
    auto private_path = ByteString::formatted("{}/private", directory);
    for (auto const& path : { store_path, shared_path, private_path })
        VERIFY(mkdir(path.characters(), 0700) == 0);

    auto certificate_path = ByteString::formatted("{}/certificate.pem", shared_path);
    auto neighbor_path = ByteString::formatted("{}/neighbor.pem", shared_path);
    auto key_path = ByteString::formatted("{}/key.pem", private_path);
    auto write_file = [](ByteString const& path, StringView content) {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(content.bytes()));
    };
    write_file(certificate_path, "certificate"sv);
    write_file(neighbor_path, "neighbor"sv);
    write_file(key_path, "key"sv);

    auto certificate_link_path = ByteString::formatted("{}/0a1b2c3d.0", store_path);
    VERIFY(symlink("../shared/certificate.pem", certificate_link_path.characters()) == 0);
    auto directory_link_path = ByteString::formatted("{}/0a1b2c3d.1", store_path);
    auto unrelated_link_path = ByteString::formatted("{}/neighbor.pem", store_path);
    VERIFY(symlink(private_path.characters(), directory_link_path.characters()) == 0);
    VERIFY(symlink("../shared/neighbor.pem", unrelated_link_path.characters()) == 0);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        // A list may name directories that don't exist or can't be searched, and OpenSSL skips those.
        VERIFY(setenv("SSL_CERT_DIR", ByteString::formatted("{}/missing:{}/certificate.pem/store:{}", directory, shared_path, store_path).characters(), 1) == 0);
        MUST(RequestServer::apply_sandbox({}, {}, {}));

        auto certificate_file = MUST(Core::File::open(certificate_link_path, Core::File::OpenMode::Read));
        auto contents = MUST(certificate_file->read_until_eof());
        VERIFY(StringView(contents) == "certificate"sv);

        VERIFY(open(neighbor_path.characters(), O_RDONLY) == -1);
        VERIFY(errno == EACCES);
        VERIFY(open(ByteString::formatted("{}/key.pem", directory_link_path).characters(), O_RDONLY) == -1);
        VERIFY(errno == EACCES);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(vectored_io_and_permission_changes_work_in_the_sandbox)
{
    if (!landlock_is_available())
        return;

    char directory_template[] = "/tmp/ladybird-sandbox-io-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);
    ScopeGuard cleanup = [&] {
        MUST(FileSystem::remove(ByteString { directory }, FileSystem::RecursionMode::Allowed));
    };
    auto file_path = ByteString::formatted("{}/cache-entry", directory);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        MUST(RequestServer::apply_sandbox({}, {}, StringView { directory, strlen(directory) }));

        auto fd = open(file_path.characters(), O_CREAT | O_RDWR | O_EXCL, 0600);
        VERIFY(fd >= 0);
        char first[] = "hello";
        char second[] = " world";
        iovec output[] = { { first, 5 }, { second, 6 } };
        VERIFY(writev(fd, output, 2) == 11);
        VERIFY(lseek(fd, 0, SEEK_SET) == 0);

        char buffer[11] {};
        iovec input[] = { { buffer, 3 }, { buffer + 3, 8 } };
        VERIFY(readv(fd, input, 2) == 11);
        VERIFY(StringView(buffer, sizeof(buffer)) == "hello world"sv);

        VERIFY(fchmod(fd, 0400) == 0);
        struct stat metadata {};
        VERIFY(fstat(fd, &metadata) == 0);
        VERIFY((metadata.st_mode & 0777) == 0400);
        VERIFY(close(fd) == 0);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}
