/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/LexicalPath.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <Security/Security.h>
#include <Services/Compositor/Sandbox.h>
#include <Services/RendererSandbox.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// These tests apply the real sandbox of a helper in a child process, then try an operation that the helper must not
// be able to perform.

enum class Outcome {
    Allowed,
    Denied,
};

static Outcome run_in_helper_sandbox(Function<ErrorOr<void>()> const& apply_sandbox, Function<bool()> const& operation)
{
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        if (apply_sandbox().is_error())
            _exit(2);
        _exit(operation() ? 0 : 1);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    if (WIFSIGNALED(status))
        return Outcome::Denied;
    VERIFY(WIFEXITED(status));
    VERIFY(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 1);
    return WEXITSTATUS(status) == 0 ? Outcome::Allowed : Outcome::Denied;
}

static ByteString darwin_user_cache_directory()
{
    char directory[PATH_MAX];
    VERIFY(confstr(_CS_DARWIN_USER_CACHE_DIR, directory, sizeof(directory)) > 0);
    char resolved[PATH_MAX];
    VERIFY(realpath(directory, resolved));
    return resolved;
}

// A directory that stands in for another application's cache.
struct OtherApplicationCache {
    OtherApplicationCache()
    {
        path = ByteString::formatted("{}/org.ladybird.TestHelperSandboxes.{}", darwin_user_cache_directory(), getpid());
        VERIFY(mkdir(path.characters(), 0700) == 0);
        file = ByteString::formatted("{}/secret", path);
        auto fd = open(file.characters(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        VERIFY(fd >= 0);
        VERIFY(write(fd, "secret", 6) == 6);
        close(fd);
    }

    ~OtherApplicationCache()
    {
        unlink(file.characters());
        rmdir(path.characters());
    }

    ByteString path;
    ByteString file;
};

static bool can_open(ByteString const& path, int flags)
{
    auto fd = open(path.characters(), flags | O_CLOEXEC);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

static ByteString temporary_cache_path()
{
    char path_template[] = "/tmp/TestHelperSandboxes.XXXXXX";
    VERIFY(mkdtemp(path_template));
    return path_template;
}

TEST_CASE(compositor_cannot_touch_other_applications_caches)
{
    OtherApplicationCache other_application;
    auto cache_path = temporary_cache_path();
    for (auto flags : { O_RDONLY, O_WRONLY })
        EXPECT_EQ(run_in_helper_sandbox([&] { return Compositor::apply_sandbox({}, cache_path, {}); }, [&] { return can_open(other_application.file, flags); }), Outcome::Denied);
}

TEST_CASE(renderer_cannot_touch_other_applications_caches)
{
    OtherApplicationCache other_application;
    auto cache_path = temporary_cache_path();
    for (auto audio_access : { RendererSandbox::AudioAccess::Yes, RendererSandbox::AudioAccess::No }) {
        for (auto flags : { O_RDONLY, O_WRONLY })
            EXPECT_EQ(run_in_helper_sandbox([&] { return RendererSandbox::apply_sandbox({}, cache_path.view(), audio_access); }, [&] { return can_open(other_application.file, flags); }), Outcome::Denied);
    }
}

struct HelperSignature {
    bool uses_hardened_runtime { false };
    bool may_map_jit_memory { false };
};

static Optional<HelperSignature> signature_of_helper(StringView name)
{
    // The helpers live in the application bundle next to the test executables.
    auto test_directory = LexicalPath::dirname(MUST(Core::System::current_executable_path()));
    auto path = ByteString::formatted("{}/Ladybird.app/Contents/MacOS/{}", test_directory, name);
    struct stat metadata {};
    if (stat(path.characters(), &metadata) != 0)
        return {};

    auto url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<u8 const*>(path.characters()), path.length(), false);
    SecStaticCodeRef code = nullptr;
    VERIFY(SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) == errSecSuccess);
    CFRelease(url);
    CFDictionaryRef information = nullptr;
    VERIFY(SecCodeCopySigningInformation(code, kSecCSSigningInformation, &information) == errSecSuccess);

    HelperSignature signature;
    u32 flags = 0;
    if (auto flags_number = static_cast<CFNumberRef>(CFDictionaryGetValue(information, kSecCodeInfoFlags)))
        CFNumberGetValue(flags_number, kCFNumberSInt32Type, &flags);
    signature.uses_hardened_runtime = (flags & kSecCodeSignatureRuntime) != 0;
    if (auto entitlements = static_cast<CFDictionaryRef>(CFDictionaryGetValue(information, kSecCodeInfoEntitlementsDict)))
        signature.may_map_jit_memory = CFDictionaryGetValue(entitlements, CFSTR("com.apple.security.cs.allow-jit")) == kCFBooleanTrue;

    CFRelease(information);
    CFRelease(code);
    return signature;
}

TEST_CASE(helpers_run_with_the_hardened_runtime)
{
    // With the hardened runtime, the kernel refuses to run code from memory that was writable, unless the process may
    // map JIT memory. Only the renderers run code that they compile at runtime.
    for (auto name : { "Compositor"sv, "ImageDecoder"sv, "ProcessReaper"sv, "RequestServer"sv, "WasmCompiler"sv, "WebContent"sv, "WebWorker"sv }) {
        auto signature = signature_of_helper(name);
        if (!signature.has_value()) {
            warnln("Skipping {}, which was not built", name);
            continue;
        }
        EXPECT(signature->uses_hardened_runtime);
        EXPECT_EQ(signature->may_map_jit_memory, name == "WebContent"sv || name == "WebWorker"sv);
    }
}
