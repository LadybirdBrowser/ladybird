/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <Compositor/Sandbox.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <link.h>
#include <string.h>

namespace Compositor {

// A driver or layer manifest names the library that the loader opens. A bare name is found by the dynamic loader in
// the library directories, which are already allowed. A path, which may be relative to the manifest, can be anywhere.
static ErrorOr<void> add_manifest_library_paths(Vector<Sandbox::LandlockPath>& paths, StringView manifest_path)
{
    auto file = Core::File::open(manifest_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = TRY(file.value()->read_until_eof());
    auto manifest = JsonValue::from_string(contents);
    if (manifest.is_error() || !manifest.value().is_object())
        return {};

    auto add_library_path = [&](JsonObject const& entry) -> ErrorOr<void> {
        auto library_path = entry.get_string("library_path"sv);
        if (!library_path.has_value() || !library_path->contains('/'))
            return {};
        auto path = LexicalPath::absolute_path(LexicalPath::dirname(manifest_path), *library_path);
        return Sandbox::add_landlock_path_if_exists(paths, path, Sandbox::LandlockPath::Access::ReadOnly);
    };

    auto const& object = manifest.value().as_object();
    for (auto key : { "ICD"sv, "layer"sv }) {
        if (auto entry = object.get_object(key); entry.has_value())
            TRY(add_library_path(*entry));
    }
    if (auto layers = object.get_array("layers"sv); layers.has_value()) {
        for (auto const& layer : layers->values()) {
            if (layer.is_object())
                TRY(add_library_path(layer.as_object()));
        }
    }
    return {};
}

// Allows a manifest, or every manifest in a directory, along with the libraries that they point to.
static ErrorOr<void> add_manifest_paths(Vector<Sandbox::LandlockPath>& paths, StringView manifest_file_or_directory)
{
    auto metadata = Core::System::stat(manifest_file_or_directory);
    if (metadata.is_error())
        return {};
    TRY(Sandbox::add_landlock_path_if_exists(paths, manifest_file_or_directory, Sandbox::LandlockPath::Access::ReadOnly));
    if (!S_ISDIR(metadata.value().st_mode))
        return add_manifest_library_paths(paths, manifest_file_or_directory);

    auto flags = static_cast<Core::DirIterator::Flags>(Core::DirIterator::SkipDots | Core::DirIterator::NoStat);
    return Core::Directory::for_each_entry(manifest_file_or_directory, flags, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
        if (entry.name.ends_with(".json"sv))
            TRY(add_manifest_library_paths(paths, LexicalPath::join(manifest_file_or_directory, entry.name).string()));
        return IterationDecision::Continue;
    });
}

// The Vulkan loader and glvnd find their drivers and layers through manifests, which they read while the GPU backend
// starts, after Landlock is in place. So we allow the directories that they search by default, and whatever the
// environment points them at instead.
static ErrorOr<void> add_loader_manifest_paths(Vector<Sandbox::LandlockPath>& paths)
{
    auto home = Core::Environment::get("HOME"sv).value_or(""sv);
    auto environment_or = [](StringView name, ByteString fallback) {
        auto value = Core::Environment::get(name);
        return value.has_value() && !value->is_empty() ? value->to_byte_string() : move(fallback);
    };

    // The Vulkan loader looks for a vulkan directory in each of these, in this order.
    Vector<ByteString> vulkan_search_roots;
    vulkan_search_roots.append(environment_or("XDG_CONFIG_HOME"sv, ByteString::formatted("{}/.config", home)));
    auto config_directories = environment_or("XDG_CONFIG_DIRS"sv, "/etc/xdg"sv);
    for (auto directory : config_directories.split_view(':'))
        vulkan_search_roots.append(directory);
    vulkan_search_roots.append("/etc"sv);
    vulkan_search_roots.append("/usr/local/etc"sv);
    vulkan_search_roots.append(environment_or("XDG_DATA_HOME"sv, ByteString::formatted("{}/.local/share", home)));
    auto data_directories = environment_or("XDG_DATA_DIRS"sv, "/usr/local/share:/usr/share"sv);
    for (auto directory : data_directories.split_view(':'))
        vulkan_search_roots.append(directory);

    for (auto const& root : vulkan_search_roots) {
        if (root.is_empty() || !root.starts_with('/'))
            continue;
        for (auto subdirectory : { "icd.d"sv, "implicit_layer.d"sv, "explicit_layer.d"sv })
            TRY(add_manifest_paths(paths, ByteString::formatted("{}/vulkan/{}", root, subdirectory)));
    }
    for (auto directory : { "/etc/glvnd/egl_vendor.d"sv, "/usr/share/glvnd/egl_vendor.d"sv })
        TRY(add_manifest_paths(paths, directory));

    static constexpr Array manifest_variables {
        "VK_DRIVER_FILES"sv,
        "VK_ICD_FILENAMES"sv,
        "VK_ADD_DRIVER_FILES"sv,
        "VK_LAYER_PATH"sv,
        "VK_ADD_LAYER_PATH"sv,
        "VK_IMPLICIT_LAYER_PATH"sv,
        "VK_ADD_IMPLICIT_LAYER_PATH"sv,
        "__EGL_VENDOR_LIBRARY_FILENAMES"sv,
        "__EGL_VENDOR_LIBRARY_DIRS"sv,
    };
    for (auto variable : manifest_variables) {
        auto value = Core::Environment::get(variable);
        if (!value.has_value())
            continue;
        for (auto entry : value->split_view(':'))
            TRY(add_manifest_paths(paths, entry));
    }
    return {};
}

// The driver loads more libraries after Landlock is in place, and they must come from where the libraries that we
// already use came from. Otherwise the dynamic loader skips to another copy, such as the system's instead of our own.
static ErrorOr<void> add_loaded_library_directories(Vector<Sandbox::LandlockPath>& paths)
{
    Vector<ByteString> directories;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {
        auto& directories = *static_cast<Vector<ByteString>*>(data);
        StringView name { info->dlpi_name, strlen(info->dlpi_name) };
        if (name.starts_with('/')) {
            auto directory = LexicalPath::dirname(name);
            if (!directories.contains_slow(directory))
                directories.append(move(directory));
        }
        return 0;
    },
        &directories);

    for (auto const& directory : directories)
        TRY(Sandbox::add_landlock_path_if_exists(paths, directory, Sandbox::LandlockPath::Access::ReadOnly));
    return {};
}

ErrorOr<void> restrict_filesystem(StringView resource_root)
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    Vector<Sandbox::LandlockPath> paths;
    TRY(Sandbox::add_landlock_path_if_exists(paths, TRY(String::formatted("{}/fonts", resource_root)), Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/lib"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/lib64"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/lib"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/local/lib"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(add_loaded_library_directories(paths));
    // cpptrace opens loaded ELF objects when symbolizing in-process stack traces, and drivers read it too.
    TRY(Sandbox::add_landlock_path_if_exists(paths, TRY(Core::System::current_executable_path()), Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/glvnd"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/share/glvnd"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/share/drirc.d"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/share/libdrm"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/share/vulkan"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/dri"sv, Sandbox::LandlockPath::Access::ReadWrite));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/udmabuf"sv, Sandbox::LandlockPath::Access::ReadWrite));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/sys"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/ld.so.cache"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/egl"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/share/egl"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(add_loader_manifest_paths(paths));

    // NB: GPU drivers read these while they set up and while they run. They used to do the setup before Landlock was in
    //     place, and in threads that it never confined.
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/proc/self"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/proc/meminfo"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/proc/stat"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/proc/filesystems"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/proc/sys/dev"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/proc/driver/nvidia"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/etc/nvidia"sv, Sandbox::LandlockPath::Access::ReadOnly));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/usr/share/nvidia"sv, Sandbox::LandlockPath::Access::ReadOnly));

    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/nvidiactl"sv, Sandbox::LandlockPath::Access::ReadWrite));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/nvidia-modeset"sv, Sandbox::LandlockPath::Access::ReadWrite));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/nvidia-uvm"sv, Sandbox::LandlockPath::Access::ReadWrite));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/nvidia-uvm-tools"sv, Sandbox::LandlockPath::Access::ReadWrite));
    TRY(Sandbox::add_landlock_path_if_exists(paths, "/dev/nvidia-caps"sv, Sandbox::LandlockPath::Access::ReadWrite));

    // NB: Add all of the primary nvidia device files (e.g. /dev/nvidia0, /dev/nvidia1, etc).
    auto flags = static_cast<Core::DirIterator::Flags>(Core::DirIterator::SkipDots | Core::DirIterator::NoStat);
    TRY(Core::Directory::for_each_entry("/dev"sv, flags, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
        if (entry.name.starts_with("nvidia"sv)) {
            auto suffix = entry.name.substring_view(6);
            if (!suffix.is_empty() && all_of(suffix, is_ascii_digit))
                TRY(Sandbox::add_landlock_path_if_exists(paths, TRY(String::formatted("/dev/{}", entry.name)), Sandbox::LandlockPath::Access::ReadWrite));
        }

        return IterationDecision::Continue;
    }));

    auto mesa_shader_cache_path = Core::Environment::get("MESA_SHADER_CACHE_DIR"sv)
                                      .map([](auto path) { return path.to_byte_string(); })
                                      .value_or_lazy_evaluated([] { return ByteString::formatted("{}/mesa_shader_cache", Core::StandardPaths::cache_directory()); });
    TRY(Core::Directory::create(mesa_shader_cache_path, Core::Directory::CreateDirectories::Yes));
    TRY(Sandbox::add_landlock_path_if_exists(paths, mesa_shader_cache_path, Sandbox::LandlockPath::Access::ReadWrite));

    // The NVIDIA driver keeps its own shader cache.
    if (!Core::System::access("/dev/nvidiactl"sv, F_OK).is_error()) {
        auto nvidia_shader_cache_path = Core::Environment::get("__GL_SHADER_DISK_CACHE_PATH"sv)
                                            .map([](auto path) { return path.to_byte_string(); })
                                            .value_or_lazy_evaluated([] { return ByteString::formatted("{}/nvidia", Core::StandardPaths::cache_directory()); });
        TRY(Core::Directory::create(nvidia_shader_cache_path, Core::Directory::CreateDirectories::Yes));
        TRY(Sandbox::add_landlock_path_if_exists(paths, nvidia_shader_cache_path, Sandbox::LandlockPath::Access::ReadWrite));
    }

    TRY(Sandbox::restrict_filesystem_with_landlock(paths.span()));
    return {};
}

ErrorOr<void> apply_sandbox(StringView, StringView, StringView)
{
    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_filesystem_writes();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_gpu_device_operations();
    policy.allow_common_runtime();
    policy.allow_executable_memory_mappings();
    // Some GPU drivers allocate writable executable code heaps lazily after
    // context creation, including while handling WebGL commands.
    policy.allow_writable_executable_memory_mappings();
    TRY(policy.install());

    return {};
}

}
