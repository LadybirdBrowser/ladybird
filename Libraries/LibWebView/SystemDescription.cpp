/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <AK/GenericShorthands.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/StringBuilder.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <LibWebView/SystemDescription.h>

#if defined(AK_OS_MACOS)
#    include <sys/sysctl.h>
#endif

#if !defined(AK_OS_WINDOWS)
#    include <sys/utsname.h>
#endif

#if defined(__GLIBC__)
#    include <gnu/libc-version.h>
#endif

#if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
#    include <link.h>
#endif

namespace WebView {

static NeverDestroyed<String> s_ui_toolkit;
static NeverDestroyed<String> s_ui_platform;

// https://www.freedesktop.org/software/systemd/man/latest/os-release.html
OSRelease parse_os_release(StringView contents)
{
    OSRelease result;
    for (auto line : contents.lines()) {
        line = line.trim_whitespace();
        if (line.is_empty() || line.starts_with('#'))
            continue;

        auto equals = line.find('=');
        if (!equals.has_value())
            continue;
        auto key = line.substring_view(0, *equals);
        auto value = line.substring_view(*equals + 1);
        if (value.length() >= 2 && (value[0] == '"' || value[0] == '\'') && value[value.length() - 1] == value[0])
            value = value.substring_view(1, value.length() - 2);

        auto string = String::from_utf8(value);
        if (string.is_error())
            continue;
        if (key == "ID"sv)
            result.id = string.release_value();
        else if (key == "VERSION_ID"sv)
            result.version_id = string.release_value();
    }
    return result;
}

Optional<InstallMethod> parse_package_format(StringView contents)
{
    auto format = contents.trim_whitespace();
    if (format == "DEB"sv)
        return InstallMethod::DEB;
    if (format == "RPM"sv)
        return InstallMethod::RPM;
    if (format == "AppImage"sv)
        return InstallMethod::AppImage;
    return {};
}

// Distributions number their releases, so a version that isn't a number is one of a kind, such as a custom image's.
StringView version_number_from(StringView version)
{
    if (version.is_empty() || !is_ascii_digit(version[0]) || !all_of(version, [](char character) { return is_ascii_digit(character) || character == '.'; }))
        return {};
    return version;
}

// Only the runtimes Flatpaks are built on, at a numbered branch: anyone can install a runtime of their own.
Optional<String> flatpak_runtime_from(StringView flatpak_info)
{
    static constexpr Array known_runtimes { "org.freedesktop.Platform"sv, "org.gnome.Platform"sv, "org.kde.Platform"sv };

    auto in_application_group = false;
    for (auto line : flatpak_info.lines()) {
        line = line.trim_whitespace();
        if (line.starts_with('[')) {
            in_application_group = line == "[Application]"sv;
            continue;
        }
        if (!in_application_group || !line.starts_with("runtime="sv))
            continue;

        // A runtime is named runtime/NAME/ARCH/BRANCH, and a report gives the architecture already.
        auto parts = line.substring_view("runtime="sv.length()).split_view('/');
        if (parts.size() != 4 || parts[0] != "runtime"sv)
            return {};
        if (!known_runtimes.contains_slow(parts[1]))
            return "other"_string;
        auto branch = version_number_from(parts[3]);
        return MUST(String::formatted("{} {}", parts[1], branch.is_empty() ? "other"sv : branch));
    }
    return {};
}

Optional<String> package_target_from(StringView package_target)
{
    auto release = parse_os_release(package_target);
    if (!release.id.has_value())
        return {};
    auto version = release.version_id.has_value() ? version_number_from(*release.version_id) : StringView {};
    if (version.is_empty())
        return release.id;
    return MUST(String::formatted("{} {}", *release.id, version));
}

// musl names itself nowhere a program can ask, but its dynamic loader, which is also its C library, is
// ld-musl-<arch>.so.1.
Optional<String> c_library_from_loaded_object(StringView path)
{
    if (path.find_last_split_view('/').starts_with("ld-musl-"sv))
        return "musl"_string;
    return {};
}

// Rounded to whole gibibytes, the way machines are sold.
String describe_memory_size(u64 bytes)
{
    auto gibibytes = max<u64>(1, (bytes + GiB / 2) / GiB);
    return MUST(String::formatted("{} GiB", gibibytes));
}

StringView install_method_name(InstallMethod install_method)
{
    switch (install_method) {
    case InstallMethod::MacOSApp:
        return "app"sv;
    case InstallMethod::DEB:
        return "deb"sv;
    case InstallMethod::RPM:
        return "rpm"sv;
    case InstallMethod::Flatpak:
        return "flatpak"sv;
    case InstallMethod::AppImage:
        return "appimage"sv;
    case InstallMethod::Source:
        return "source"sv;
    }
    VERIFY_NOT_REACHED();
}

static InstallMethod current_install_method()
{
#if defined(AK_OS_MACOS)
    return InstallMethod::MacOSApp;
#else
    if (FileSystem::exists("/.flatpak-info"sv))
        return InstallMethod::Flatpak;

    // The Linux packages say which kind they are in a file of their own, which an archive of the same build lacks.
    auto marker = LexicalPath::join(s_ladybird_resource_root, "package-format"sv).string();
    if (auto file = Core::File::open(marker, Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            if (auto install_method = parse_package_format(contents.value()); install_method.has_value())
                return *install_method;
        }
    }
    return InstallMethod::Source;
#endif
}

static OSRelease read_os_release(ReadonlySpan<StringView> paths)
{
    for (auto path : paths) {
        auto file = Core::File::open(path, Core::File::OpenMode::Read);
        if (file.is_error())
            continue;
        auto contents = file.value()->read_until_eof();
        if (!contents.is_error())
            return parse_os_release(contents.value());
    }
    return {};
}

static OSRelease current_os_release(InstallMethod install_method)
{
    // A Flatpak's /etc describes its runtime. Flatpak shows the host's os-release at /run/host.
    if (install_method == InstallMethod::Flatpak)
        return read_os_release(Array { "/run/host/os-release"sv });
    return read_os_release(Array { "/etc/os-release"sv, "/usr/lib/os-release"sv });
}

static Optional<String> current_macos_version()
{
#if defined(AK_OS_MACOS)
    char version[32] {};
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0)
        return {};
    auto string = String::from_utf8(StringView { version, strnlen(version, sizeof(version)) });
    if (string.is_error())
        return {};
    return string.release_value();
#else
    return {};
#endif
}

SystemDescription describe_current_system()
{
    SystemDescription system;
    system.install_method = current_install_method();
#if ARCH(X86_64)
    system.architecture = Architecture::X86_64;
#elif ARCH(AARCH64)
    system.architecture = Architecture::AArch64;
#endif
    if (system.install_method != InstallMethod::MacOSApp)
        system.os_release = current_os_release(system.install_method);
    system.macos_version = current_macos_version();
    return system;
}

static ByteString kernel_release()
{
#if !defined(AK_OS_WINDOWS)
    utsname system {};
    if (uname(&system) != 0)
        return {};
    // Only the numeric release, never nodename or custom kernel build text.
    auto release = StringView { system.release, strnlen(system.release, sizeof(system.release)) };
    size_t length = 0;
    while (length < release.length() && (is_ascii_digit(release[length]) || release[length] == '.'))
        ++length;
    return release.substring_view(0, length);
#else
    return {};
#endif
}

static Optional<String> current_flatpak_runtime()
{
    auto file = Core::File::open("/.flatpak-info"sv, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    return flatpak_runtime_from(contents.value());
}

static Optional<String> current_package_target()
{
    auto path = LexicalPath::join(s_ladybird_resource_root, "package-target"sv).string();
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    return package_target_from(contents.value());
}

// Whatever produced a value, a report only carries it if it is short and in a plain alphabet.
static bool is_reportable(StringView value)
{
    static constexpr size_t maximum_length = 64;
    if (value.is_empty() || value.length() > maximum_length)
        return false;
    return all_of(value, [](char character) {
        return is_ascii_alphanumeric(character) || " ._:+~-"sv.contains(character);
    });
}

Optional<String> hypervisor_from_dmi(StringView sys_vendor, StringView product_name)
{
    struct Hypervisor {
        StringView mark;
        StringView name;
    };
    static constexpr Array hypervisors {
        Hypervisor { "Apple Virtualization"sv, "Apple Virtualization"sv },
        Hypervisor { "QEMU"sv, "QEMU"sv },
        Hypervisor { "KVM"sv, "KVM"sv },
        Hypervisor { "VMware"sv, "VMware"sv },
        Hypervisor { "VirtualBox"sv, "VirtualBox"sv },
        Hypervisor { "innotek"sv, "VirtualBox"sv },
        Hypervisor { "Parallels"sv, "Parallels"sv },
        Hypervisor { "Xen"sv, "Xen"sv },
        Hypervisor { "Bochs"sv, "Bochs"sv },
        Hypervisor { "Libkrun"sv, "Libkrun"sv },
    };
    for (auto const& hypervisor : hypervisors) {
        if (sys_vendor.contains(hypervisor.mark, CaseSensitivity::CaseInsensitive) || product_name.contains(hypervisor.mark, CaseSensitivity::CaseInsensitive))
            return MUST(String::from_utf8(hypervisor.name));
    }

    // Firmware can be given any vendor, so a machine that calls itself virtual is only reported as one.
    if (!product_name.contains("Virtual Machine"sv, CaseSensitivity::CaseInsensitive) && !product_name.contains("Virtual Platform"sv, CaseSensitivity::CaseInsensitive))
        return {};
    if (sys_vendor.starts_with("Microsoft"sv, CaseSensitivity::CaseInsensitive))
        return "Hyper-V"_string;
    return "yes"_string;
}

// A display manager names the session, but one started another way, such as over a remote desktop, may say nothing, or
// "tty" for the login it came from. What a browser connects to tells then.
StringView session_type_from(StringView xdg_session_type, StringView wayland_display, StringView x11_display)
{
    if (xdg_session_type == "wayland"sv || xdg_session_type == "x11"sv)
        return xdg_session_type;
    if (!wayland_display.is_empty())
        return "wayland"sv;
    if (!x11_display.is_empty())
        return "x11"sv;
    if (first_is_one_of(xdg_session_type, "tty"sv, "mir"sv, "unspecified"sv))
        return xdg_session_type;
    return {};
}

// The desktops that the freedesktop.org menu specification registers, and compositors that name themselves the same
// way. The variable can say anything, so any other name is reported as "other".
String desktop_from(StringView xdg_current_desktop)
{
    static constexpr Array known_desktops {
        "Budgie"sv, "Cinnamon"sv, "COSMIC"sv, "DDE"sv, "Deepin"sv, "EDE"sv, "Endless"sv, "Enlightenment"sv, "GNOME"sv,
        "GNOME-Classic"sv, "GNOME-Flashback"sv, "Hyprland"sv, "i3"sv, "KDE"sv, "labwc"sv, "Lomiri"sv, "LXDE"sv, "LXQt"sv,
        "MATE"sv, "niri"sv, "Old"sv, "Openbox"sv, "Pantheon"sv, "Phosh"sv, "pop"sv, "Razor"sv, "river"sv, "ROX"sv,
        "sway"sv, "TDE"sv, "ubuntu"sv, "Unity"sv, "wayfire"sv, "X-Cinnamon"sv, "XFCE"sv
    };

    StringBuilder builder;
    for (auto name : xdg_current_desktop.split_view(':')) {
        auto reported = "other"sv;
        for (auto known : known_desktops) {
            if (name.equals_ignoring_ascii_case(known)) {
                reported = known;
                break;
            }
        }
        if (!builder.is_empty())
            builder.append(':');
        builder.append(reported);
    }
    return builder.to_string_without_validation();
}

// The kinds of container that set the variable themselves. Any other value is reported as "other".
StringView container_kind_from(StringView container_variable)
{
    static constexpr Array known_containers { "docker"sv, "lxc"sv, "lxc-libvirt"sv, "oci"sv, "podman"sv, "systemd-nspawn"sv };
    if (container_variable.is_empty())
        return {};
    for (auto known : known_containers) {
        if (container_variable == known)
            return known;
    }
    return "other"sv;
}

static StringView current_session_type()
{
    auto xdg_session_type = Core::Environment::get("XDG_SESSION_TYPE"sv).value_or({});
#if defined(AK_OS_MACOS)
    // XQuartz sets DISPLAY, which says nothing of how the browser draws.
    return session_type_from(xdg_session_type, {}, {});
#else
    return session_type_from(xdg_session_type, Core::Environment::get("WAYLAND_DISPLAY"sv).value_or({}), Core::Environment::get("DISPLAY"sv).value_or({}));
#endif
}

#if defined(AK_OS_LINUX)
static ByteString read_first_line(StringView path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    return StringView { contents.value() }.find_first_split_view('\n').trim_whitespace();
}
#endif

// The hypervisor by name where it says which it is, or whether there is one at all.
static Optional<String> current_virtual_machine()
{
#if defined(AK_OS_MACOS)
    int present = 0;
    size_t size = sizeof(present);
    if (sysctlbyname("kern.hv_vmm_present", &present, &size, nullptr, 0) != 0)
        return {};
    if (!present)
        return "no"_string;
    char model[64] {};
    size = sizeof(model);
    if (sysctlbyname("hw.model", model, &size, nullptr, 0) == 0 && StringView { model, strnlen(model, sizeof(model)) }.starts_with("VirtualMac"sv))
        return "Apple Virtualization"_string;
    return "yes"_string;
#elif defined(AK_OS_LINUX)
    // Firmware on any architecture can name the machine, and does under the common hypervisors.
    if (auto hypervisor = hypervisor_from_dmi(read_first_line("/sys/class/dmi/id/sys_vendor"sv), read_first_line("/sys/class/dmi/id/product_name"sv)); hypervisor.has_value())
        return hypervisor;
    // QEMU's virt machine, started without firmware, describes itself in a device tree instead.
    if (auto compatible = read_first_line("/proc/device-tree/compatible"sv); compatible.contains("linux,dummy-virt"sv))
        return "QEMU"_string;
    if (FileSystem::exists("/proc/device-tree/hypervisor"sv))
        return "yes"_string;
#    if ARCH(X86_64)
    // An x86 processor says whether a hypervisor runs it. Other processors don't, so a report leaves it out.
    auto file = Core::File::open("/proc/cpuinfo"sv, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    for (auto line : StringView { contents.value() }.lines()) {
        if (line.starts_with("flags"sv))
            return line.split_view(' ').contains_slow("hypervisor"sv) ? "yes"_string : "no"_string;
    }
#    endif
    return {};
#else
    return {};
#endif
}

#if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
// A C library told apart by the name of an object loaded into the process. A static build loads none.
[[maybe_unused]] static Optional<String> c_library_from_loaded_objects()
{
    Optional<String> c_library;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {
        if (!info->dlpi_name)
            return 0;
        auto& result = *static_cast<Optional<String>*>(data);
        result = c_library_from_loaded_object({ info->dlpi_name, strlen(info->dlpi_name) });
        return result.has_value() ? 1 : 0;
    },
        &c_library);
    return c_library;
}
#endif

// The system calls behind many C library functions differ between C libraries, and between versions of one.
static Optional<String> current_c_library()
{
#if defined(__GLIBC__)
    auto version = version_number_from(StringView { gnu_get_libc_version(), strlen(gnu_get_libc_version()) });
    if (version.is_empty())
        return "glibc"_string;
    return MUST(String::formatted("glibc {}", version));
#elif defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
    return c_library_from_loaded_objects();
#else
    return {};
#endif
}

// What kind of container runs the browser, if any. Its os-release then describes the container, not the host.
static StringView current_container()
{
    // Toolbox is built on Podman, so it is told apart first.
    if (FileSystem::exists("/run/.toolboxenv"sv))
        return "toolbox"sv;
    if (FileSystem::exists("/run/.containerenv"sv))
        return "podman"sv;
    if (FileSystem::exists("/.dockerenv"sv))
        return "docker"sv;
    return container_kind_from(Core::Environment::get("container"sv).value_or({}));
}

Vector<ReportedSystemDetail> describe_system_for_report()
{
    Vector<ReportedSystemDetail> details;
    auto add = [&](StringView name, StringView value) {
        value = value.trim_whitespace();
        if (!is_reportable(value))
            return;
        for (auto const& field : reported_system_fields) {
            if (field.name == name) {
                details.append({ field, MUST(String::from_utf8(value)) });
                return;
            }
        }
        VERIFY_NOT_REACHED();
    };
    auto view = [](Optional<String> const& value) {
        return value.has_value() ? value->bytes_as_string_view() : StringView {};
    };

    auto system = describe_current_system();
    add("Kernel release"sv, kernel_release());

    add("OS version"sv, version_number_from(view(system.macos_version.has_value() ? system.macos_version : system.os_release.version_id)));
    add("Distribution"sv, view(system.os_release.id));
    add("C library"sv, view(current_c_library()));
    add("Install method"sv, install_method_name(system.install_method));
    if (first_is_one_of(system.install_method, InstallMethod::DEB, InstallMethod::RPM))
        add("Package target"sv, view(current_package_target()));
    // A Flatpak is a container of its own, which its runtime describes.
    if (system.install_method == InstallMethod::Flatpak)
        add("Flatpak runtime"sv, view(current_flatpak_runtime()));
    else
        add("Container"sv, current_container());

    add("Virtual machine"sv, view(current_virtual_machine()));
    add("Logical CPUs"sv, ByteString::number(Core::System::hardware_concurrency()));
    add("Memory"sv, describe_memory_size(Core::System::physical_memory_bytes()));

    auto desktop = desktop_from(Core::Environment::get("XDG_CURRENT_DESKTOP"sv).value_or({}));
    add("Desktop"sv, desktop);
    add("Session type"sv, current_session_type());
    add("UI toolkit"sv, *s_ui_toolkit);
    add("UI platform"sv, *s_ui_platform);
    return details;
}

void append_system_for_report(StringBuilder& builder, ReadonlySpan<ReportedSystemDetail> details)
{
    for (auto const& detail : details)
        builder.appendff("{}: {}\n", detail.field.name, detail.value);
}

void set_ui_toolkit_for_reports(String toolkit, String platform)
{
    *s_ui_toolkit = move(toolkit);
    *s_ui_platform = move(platform);
}

}
