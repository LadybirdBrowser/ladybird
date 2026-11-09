/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unicode/udata.h>
#include <unicode/utypes.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#else
#    include <dlfcn.h>
#    include <fcntl.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

#if defined(AK_OS_MACOS)
#    include <mach-o/dyld.h>
#elif defined(AK_OS_FREEBSD)
#    include <sys/sysctl.h>
#    include <sys/types.h>
#endif

// ICU is built without its data, which ships as one file next to the other resources. Every process maps it before main()
// runs, so before any sandbox closes the file system, and tells ICU to never look on disk for anything else.

#if defined(AK_OS_WINDOWS)
using PathCharacter = wchar_t;
static constexpr PathCharacter path_separator = L'\\';
#else
using PathCharacter = char;
static constexpr PathCharacter path_separator = '/';
#endif

static constexpr size_t maximum_path_length = 4096;
static constexpr char icu_data_file_name[] = U_ICUDATA_NAME ".dat";

struct Path {
    PathCharacter characters[maximum_path_length] {};
    size_t length { 0 };

    bool append(PathCharacter const* string)
    {
        for (; *string; ++string) {
            if (length + 1 >= maximum_path_length)
                return false;
            characters[length++] = *string;
        }
        characters[length] = 0;
        return true;
    }

    bool append_ascii(char const* string)
    {
        for (; *string; ++string) {
            if (length + 1 >= maximum_path_length)
                return false;
            characters[length++] = *string == '/' ? path_separator : static_cast<PathCharacter>(*string);
        }
        characters[length] = 0;
        return true;
    }

    void remove_last_component()
    {
        while (length > 0 && characters[length - 1] != '/' && characters[length - 1] != path_separator)
            --length;
        if (length > 0)
            --length;
        characters[length] = 0;
    }
};

static constexpr char const* candidate_directories_relative_to_binary[] = {
    "/../share/Lagom/icu/",
#if defined(AK_OS_MACOS)
    "/../Resources/icu/",
#endif
    "/",
};

static constexpr size_t maximum_candidate_count = 1 + 2 * (sizeof(candidate_directories_relative_to_binary) / sizeof(char const*));

struct Candidates {
    Path paths[maximum_candidate_count];
    size_t count { 0 };

    void add_relative_to_directory_of(Path const& binary)
    {
        if (binary.length == 0)
            return;
        Path directory = binary;
        directory.remove_last_component();
        if (directory.length == 0)
            return;
        for (auto const* relative_directory : candidate_directories_relative_to_binary) {
            Path candidate = directory;
            if (candidate.append_ascii(relative_directory) && candidate.append_ascii(icu_data_file_name) && count < maximum_candidate_count)
                paths[count++] = candidate;
        }
    }
};

#if defined(AK_OS_WINDOWS)

static void const* map_file(Path const& path)
{
    auto file = CreateFileW(path.characters, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;
    auto mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping)
        return nullptr;
    void const* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    return data;
}

static void add_candidates(Candidates& candidates, void const* address_in_this_binary)
{
    if (auto const* override_path = _wgetenv(L"LADYBIRD_ICU_DATA")) {
        Path path;
        if (path.append(override_path))
            candidates.paths[candidates.count++] = path;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address_in_this_binary), &module))
        return;
    Path binary;
    binary.length = GetModuleFileNameW(module, binary.characters, maximum_path_length);
    if (binary.length == 0 || binary.length >= maximum_path_length)
        return;
    candidates.add_relative_to_directory_of(binary);
}

static void print_path(Path const& path)
{
    fwprintf(stderr, L"    %ls\n", path.characters);
}

#else

static void const* map_file(Path const& path)
{
    int fd = open(path.characters, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    void* data = MAP_FAILED;
    struct stat file_status {};
    if (fstat(fd, &file_status) == 0 && file_status.st_size > 0)
        data = mmap(nullptr, file_status.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return data == MAP_FAILED ? nullptr : data;
}

static Path executable_path()
{
    Path path;
#    if defined(AK_OS_MACOS)
    uint32_t size = maximum_path_length;
    if (_NSGetExecutablePath(path.characters, &size) == 0)
        path.length = strlen(path.characters);
#    elif defined(AK_OS_FREEBSD)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t size = maximum_path_length;
    if (sysctl(mib, 4, path.characters, &size, nullptr, 0) == 0)
        path.length = strlen(path.characters);
#    else
    auto length = readlink("/proc/self/exe", path.characters, maximum_path_length - 1);
    if (length > 0) {
        path.length = static_cast<size_t>(length);
        path.characters[path.length] = 0;
    }
#    endif
    return path;
}

static void add_candidates(Candidates& candidates, void const* address_in_this_binary)
{
    if (auto const* override_path = getenv("LADYBIRD_ICU_DATA")) {
        Path path;
        if (path.append(override_path))
            candidates.paths[candidates.count++] = path;
    }

    Dl_info info {};
    if (dladdr(address_in_this_binary, &info) != 0 && info.dli_fname && info.dli_fname[0] == '/') {
        Path binary;
        if (binary.append(info.dli_fname))
            candidates.add_relative_to_directory_of(binary);
    }

    candidates.add_relative_to_directory_of(executable_path());
}

static void print_path(Path const& path)
{
    fprintf(stderr, "    %s\n", path.characters);
}

#endif

extern "C" [[gnu::constructor(101)]] void ladybird_load_icu_data();

extern "C" void ladybird_load_icu_data()
{
    Candidates candidates;
    add_candidates(candidates, reinterpret_cast<void const*>(&ladybird_load_icu_data));

    for (size_t i = 0; i < candidates.count; ++i) {
        auto const* data = map_file(candidates.paths[i]);
        if (!data)
            continue;

        UErrorCode status = U_ZERO_ERROR;
        udata_setCommonData(data, &status);
        if (U_FAILURE(status)) {
            fprintf(stderr, "Unable to use the ICU data file below: %s\n", u_errorName(status));
            print_path(candidates.paths[i]);
            abort();
        }

        udata_setFileAccess(UDATA_ONLY_PACKAGES, &status);
        return;
    }

    fprintf(stderr, "Unable to find the ICU data file %s. Looked for it at:\n", icu_data_file_name);
    for (size_t i = 0; i < candidates.count; ++i)
        print_path(candidates.paths[i]);
    fprintf(stderr, "Set LADYBIRD_ICU_DATA to its path to use a file elsewhere.\n");
    abort();
}
