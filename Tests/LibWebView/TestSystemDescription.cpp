/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <LibWebView/SystemDescription.h>

using WebView::InstallMethod;

static constexpr Array environment_variables { "XDG_CURRENT_DESKTOP"sv, "XDG_SESSION_TYPE"sv, "container"sv, "DISPLAY"sv, "WAYLAND_DISPLAY"sv };

// Puts back what a test changes of the environment, the resource root and the UI toolkit.
struct RestoreSystem {
    RestoreSystem()
    {
        for (auto name : environment_variables)
            saved.append(Core::Environment::get(name).map([](auto value) { return ByteString { value }; }));
    }

    ~RestoreSystem()
    {
        for (size_t i = 0; i < environment_variables.size(); ++i) {
            if (saved[i].has_value())
                MUST(Core::Environment::set(environment_variables[i], *saved[i], Core::Environment::Overwrite::Yes));
            else
                MUST(Core::Environment::unset(environment_variables[i]));
        }
        WebView::s_ladybird_resource_root = resource_root;
        WebView::set_ui_toolkit_for_reports({}, {});
    }

    Vector<Optional<ByteString>> saved;
    ByteString resource_root { WebView::s_ladybird_resource_root };
};

static Optional<String> reported_value(StringView name)
{
    for (auto const& detail : WebView::describe_system_for_report()) {
        if (detail.field.name == name)
            return detail.value;
    }
    return {};
}

TEST_CASE(os_release_gives_the_id_and_version_of_the_system)
{
    auto release = WebView::parse_os_release(R"~~~(# A comment
NAME="Fedora Linux"
ID=fedora
VERSION_ID=44
PRETTY_NAME="Fedora Linux 44 (Workstation Edition)"
)~~~"sv);
    EXPECT_EQ(release.id, "fedora"sv);
    EXPECT_EQ(release.version_id, "44"sv);

    release = WebView::parse_os_release("ID='ubuntu'\nVERSION_ID=\"26.04\"\nVERSION_CODENAME=resolute\n"sv);
    EXPECT_EQ(release.id, "ubuntu"sv);
    EXPECT_EQ(release.version_id, "26.04"sv);

    release = WebView::parse_os_release("NAME=Something\n"sv);
    EXPECT(!release.id.has_value());
    EXPECT(!release.version_id.has_value());
}

TEST_CASE(a_package_names_its_format)
{
    EXPECT(WebView::parse_package_format("DEB\n"sv) == InstallMethod::DEB);
    EXPECT(WebView::parse_package_format("RPM"sv) == InstallMethod::RPM);
    EXPECT(WebView::parse_package_format("AppImage\n"sv) == InstallMethod::AppImage);
    EXPECT(!WebView::parse_package_format("Flatpak\n"sv).has_value());
    EXPECT(!WebView::parse_package_format(""sv).has_value());
}

TEST_CASE(a_flatpak_names_its_runtime_and_branch)
{
    EXPECT_EQ(WebView::flatpak_runtime_from(R"~~~([Application]
name=org.ladybird.Ladybird
runtime=runtime/org.kde.Platform/x86_64/6.10

[Instance]
runtime=runtime/org.freedesktop.Platform/x86_64/25.08
)~~~"sv),
        "org.kde.Platform 6.10"sv);

    EXPECT_EQ(WebView::flatpak_runtime_from("[Application]\nruntime=runtime/org.kde.Platform/x86_64/alice-test\n"sv), "org.kde.Platform other"sv);
    EXPECT_EQ(WebView::flatpak_runtime_from("[Application]\nruntime=runtime/com.example.Alice/x86_64/6.10\n"sv), "other"sv);
    EXPECT(!WebView::flatpak_runtime_from("[Instance]\nruntime=runtime/org.kde.Platform/x86_64/6.10\n"sv).has_value());
    EXPECT(!WebView::flatpak_runtime_from("[Application]\nruntime=org.kde.Platform\n"sv).has_value());
    EXPECT(!WebView::flatpak_runtime_from(""sv).has_value());
}

TEST_CASE(a_package_target_names_the_release_that_built_it)
{
    EXPECT_EQ(WebView::package_target_from("ID=ubuntu\nVERSION_ID=\"24.04\"\n"sv), "ubuntu 24.04"sv);
    EXPECT_EQ(WebView::package_target_from("ID=fedora\nVERSION_ID=44\n"sv), "fedora 44"sv);
    EXPECT_EQ(WebView::package_target_from("ID=ubuntu\nVERSION_ID=24.04-lab17\n"sv), "ubuntu"sv);
    EXPECT_EQ(WebView::package_target_from("ID=arch\n"sv), "arch"sv);
    EXPECT(!WebView::package_target_from("VERSION_ID=24.04\n"sv).has_value());
    EXPECT(!WebView::package_target_from(""sv).has_value());
}

TEST_CASE(musl_is_known_by_its_dynamic_loader)
{
    EXPECT_EQ(WebView::c_library_from_loaded_object("/lib/ld-musl-x86_64.so.1"sv), "musl"sv);
    EXPECT_EQ(WebView::c_library_from_loaded_object("/lib/ld-musl-aarch64.so.1"sv), "musl"sv);
    EXPECT(!WebView::c_library_from_loaded_object("/lib64/ld-linux-x86-64.so.2"sv).has_value());
    EXPECT(!WebView::c_library_from_loaded_object("/usr/lib/libc.so.6"sv).has_value());
    EXPECT(!WebView::c_library_from_loaded_object("/opt/ld-musl-tools/ld-linux-x86-64.so.2"sv).has_value());
    EXPECT(!WebView::c_library_from_loaded_object(""sv).has_value());
}

TEST_CASE(a_version_is_only_reported_as_a_number)
{
    EXPECT_EQ(WebView::version_number_from("26.04"sv), "26.04"sv);
    EXPECT_EQ(WebView::version_number_from("44"sv), "44"sv);
    EXPECT_EQ(WebView::version_number_from("26.04-lab17"sv), ""sv);
    EXPECT_EQ(WebView::version_number_from("26.04-17"sv), ""sv);
    EXPECT_EQ(WebView::version_number_from("alice"sv), ""sv);
    EXPECT_EQ(WebView::version_number_from(".1"sv), ""sv);
    EXPECT_EQ(WebView::version_number_from(""sv), ""sv);
}

TEST_CASE(firmware_names_the_hypervisor_of_a_virtual_machine)
{
    EXPECT_EQ(WebView::hypervisor_from_dmi("Libkrun"sv, "libkrun Virtual Machine"sv), "Libkrun"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("QEMU"sv, "QEMU Virtual Machine"sv), "QEMU"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("QEMU"sv, "Standard PC (Q35 + ICH9, 2009)"sv), "QEMU"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("Apple Inc."sv, "Apple Virtualization Generic Platform"sv), "Apple Virtualization"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("VMware, Inc."sv, "VMware20,1"sv), "VMware"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("innotek GmbH"sv, "VirtualBox"sv), "VirtualBox"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("Parallels International GmbH."sv, "Parallels ARM Virtual Machine"sv), "Parallels"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("Microsoft Corporation"sv, "Virtual Machine"sv), "Hyper-V"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("Vendor/Name"sv, "Some Virtual Platform"sv), "yes"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("Alice Smith"sv, "Custom Virtual Machine"sv), "yes"sv);

    // Firmware spells names however it likes.
    EXPECT_EQ(WebView::hypervisor_from_dmi("qemu"sv, ""sv), "QEMU"sv);
    EXPECT_EQ(WebView::hypervisor_from_dmi("Some Vendor"sv, "custom virtual machine"sv), "yes"sv);

    // A physical machine's vendor and model are never reported.
    EXPECT(!WebView::hypervisor_from_dmi("Microsoft Corporation"sv, "Surface Laptop 5"sv).has_value());
    EXPECT(!WebView::hypervisor_from_dmi("LENOVO"sv, "21HM"sv).has_value());
    EXPECT(!WebView::hypervisor_from_dmi(""sv, ""sv).has_value());
}

TEST_CASE(a_session_without_a_type_is_named_by_its_display)
{
    EXPECT_EQ(WebView::session_type_from("wayland"sv, ""sv, ":0"sv), "wayland"sv);
    EXPECT_EQ(WebView::session_type_from("x11"sv, "wayland-0"sv, ""sv), "x11"sv);
    EXPECT_EQ(WebView::session_type_from(""sv, ""sv, ":0"sv), "x11"sv);
    EXPECT_EQ(WebView::session_type_from(""sv, "wayland-0"sv, ":0"sv), "wayland"sv);
    EXPECT_EQ(WebView::session_type_from("tty"sv, ""sv, ":0"sv), "x11"sv);
    EXPECT_EQ(WebView::session_type_from("tty"sv, ""sv, ""sv), "tty"sv);
    EXPECT_EQ(WebView::session_type_from(""sv, ""sv, ""sv), ""sv);
    EXPECT_EQ(WebView::session_type_from("Alice"sv, ""sv, ""sv), ""sv);
}

TEST_CASE(a_desktop_or_container_is_named_only_from_known_names)
{
    EXPECT_EQ(WebView::desktop_from("ubuntu:GNOME"sv), "ubuntu:GNOME"sv);
    EXPECT_EQ(WebView::desktop_from("kde"sv), "KDE"sv);
    EXPECT_EQ(WebView::desktop_from("Alice-Smith"sv), "other"sv);
    EXPECT_EQ(WebView::desktop_from("ubuntu:Alice"sv), "ubuntu:other"sv);
    EXPECT_EQ(WebView::desktop_from(""sv), ""sv);

    EXPECT_EQ(WebView::container_kind_from("podman"sv), "podman"sv);
    EXPECT_EQ(WebView::container_kind_from("Alice"sv), "other"sv);
    EXPECT_EQ(WebView::container_kind_from(""sv), ""sv);
}

TEST_CASE(memory_is_given_in_whole_gibibytes)
{
    EXPECT_EQ(WebView::describe_memory_size(16 * GiB), "16 GiB"sv);
    EXPECT_EQ(WebView::describe_memory_size(15 * GiB + 600 * MiB), "16 GiB"sv);
    EXPECT_EQ(WebView::describe_memory_size(24 * GiB + 100 * MiB), "24 GiB"sv);
    EXPECT_EQ(WebView::describe_memory_size(300 * MiB), "1 GiB"sv);
}

TEST_CASE(a_report_names_the_desktop_session_and_ui_toolkit)
{
    RestoreSystem restore;
    MUST(Core::Environment::set("XDG_CURRENT_DESKTOP"sv, "ubuntu:GNOME"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("XDG_SESSION_TYPE"sv, "wayland"sv, Core::Environment::Overwrite::Yes));
    WebView::set_ui_toolkit_for_reports("Qt 6.10.1"_string, "wayland"_string);

    EXPECT_EQ(reported_value("Desktop"sv), "ubuntu:GNOME"sv);
    EXPECT_EQ(reported_value("Session type"sv), "wayland"sv);
    EXPECT_EQ(reported_value("UI toolkit"sv), "Qt 6.10.1"sv);
    EXPECT_EQ(reported_value("UI platform"sv), "wayland"sv);
    EXPECT(reported_value("Install method"sv).has_value());
    EXPECT(reported_value("Logical CPUs"sv).has_value());
    auto memory = reported_value("Memory"sv);
    EXPECT(memory.has_value() && memory->bytes_as_string_view().ends_with(" GiB"sv));
#if defined(AK_OS_MACOS)
    EXPECT(reported_value("OS version"sv).has_value());
    EXPECT(reported_value("Virtual machine"sv).has_value());

    // Containers that leave no file of their own behind name themselves in the environment.
    MUST(Core::Environment::set("container"sv, "systemd-nspawn"sv, Core::Environment::Overwrite::Yes));
    EXPECT_EQ(reported_value("Container"sv), "systemd-nspawn"sv);
#endif
}

TEST_CASE(only_a_package_reports_its_target)
{
    RestoreSystem restore;
    Test::TemporaryDirectory directory { "TestSystemDescription"sv };
    auto write = [&](StringView name, StringView contents) {
        auto file = MUST(Core::File::open(directory.path().append(name).string(), Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(contents.bytes()));
    };
    WebView::s_ladybird_resource_root = directory.path().string();

    write("package-target"sv, "ID=ubuntu\nVERSION_ID=\"24.04\"\n"sv);
    EXPECT(!reported_value("Package target"sv).has_value());

    write("package-format"sv, "DEB\n"sv);
#if defined(AK_OS_MACOS)
    EXPECT(!reported_value("Package target"sv).has_value());
#else
    EXPECT_EQ(reported_value("Install method"sv), "deb"sv);
    EXPECT_EQ(reported_value("Package target"sv), "ubuntu 24.04"sv);
#endif
}

TEST_CASE(a_report_names_a_c_library_it_can_identify)
{
    auto c_library = reported_value("C library"sv);
#if defined(__GLIBC__)
    VERIFY(c_library.has_value());
    auto value = c_library->bytes_as_string_view();
    VERIFY(value.starts_with("glibc "sv));
    auto version = value.substring_view("glibc "sv.length());
    EXPECT(!version.is_empty() && all_of(version, [](char c) { return is_ascii_digit(c) || c == '.'; }));
#elif defined(AK_OS_MACOS) || defined(AK_OS_WINDOWS)
    EXPECT(!c_library.has_value());
#endif
}

TEST_CASE(a_report_carries_nothing_personal_from_the_environment)
{
    RestoreSystem restore;
    MUST(Core::Environment::unset("DISPLAY"sv));
    MUST(Core::Environment::unset("WAYLAND_DISPLAY"sv));
    MUST(Core::Environment::set("XDG_CURRENT_DESKTOP"sv, "/home/someone/.local/desktop"sv, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("XDG_SESSION_TYPE"sv, ByteString::repeated('x', 65), Core::Environment::Overwrite::Yes));
    WebView::set_ui_toolkit_for_reports("Qt 6.10.1\nSomeone"_string, {});

    EXPECT_EQ(reported_value("Desktop"sv), "other"sv);
    EXPECT(!reported_value("Session type"sv).has_value());
    EXPECT(!reported_value("UI toolkit"sv).has_value());
    EXPECT(!reported_value("UI platform"sv).has_value());
}

TEST_CASE(a_report_trims_its_values)
{
    RestoreSystem restore;
    WebView::set_ui_toolkit_for_reports("  Qt 6.10.1  "_string, "   "_string);

    EXPECT_EQ(reported_value("UI toolkit"sv), "Qt 6.10.1"sv);
    EXPECT(!reported_value("UI platform"sv).has_value());
}
