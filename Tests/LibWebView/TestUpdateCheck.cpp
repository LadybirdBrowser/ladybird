/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/UpdateCheck.h>

using WebView::Architecture;
using WebView::InstallMethod;
using WebView::SystemDescription;

static constexpr auto RELEASES = R"~~~({
  "format": 1,
  "releases": [
    {
      "version": "2026.47.0",
      "published": "2026-11-20T12:00:00Z",
      "downloads": [
        { "platform": "source", "name": "ladybird-2026.47.0-source.tar.xz" }
      ],
      "repositories": {
        "apt": {
          "systems": [
            { "system": "Ubuntu 26.10", "id": "ubuntu", "version_id": "26.10", "suite": "stonking", "architectures": ["amd64"] }
          ]
        }
      }
    },
    {
      "version": "2026.46.10",
      "published": "2026-11-10T12:00:00Z",
      "downloads": [
        { "platform": "source", "name": "ladybird-2026.46.10-source.tar.xz" },
        { "platform": "macOS", "architecture": "arm64", "name": "ladybird-2026.46.10-macOS-arm64.dmg", "minimum_macos_version": "15.0" }
      ],
      "repositories": {
        "apt": {
          "systems": [
            { "system": "Ubuntu 26.04", "id": "ubuntu", "version_id": "26.04", "suite": "resolute", "architectures": ["amd64", "arm64"] }
          ]
        },
        "rpm": {
          "systems": [
            { "system": "Fedora 44", "id": "fedora", "version_id": "44", "architectures": ["x86_64"] }
          ]
        },
        "flatpak": { "architectures": ["x86_64"] }
      }
    },
    {
      "version": "2026.46.9",
      "downloads": [
        { "platform": "macOS", "architecture": "arm64", "name": "ladybird-2026.46.9-macOS-arm64.dmg", "minimum_macos_version": "14.4" }
      ],
      "repositories": {
        "apt": {
          "systems": [
            { "system": "Ubuntu 26.04", "id": "ubuntu", "version_id": "26.04", "suite": "resolute", "architectures": ["amd64", "arm64"] }
          ]
        },
        "rpm": {
          "systems": [
            { "system": "Fedora 44", "id": "fedora", "version_id": "44", "architectures": ["x86_64", "aarch64"] }
          ]
        },
        "flatpak": { "architectures": ["x86_64", "aarch64"] }
      }
    }
  ]
})~~~"sv;

static Optional<String> newest_for(SystemDescription const& system)
{
    return MUST(WebView::newest_release_for_system(RELEASES, system));
}

static SystemDescription linux_system(InstallMethod install_method, Architecture architecture, StringView id = {}, StringView version_id = {})
{
    SystemDescription system { .install_method = install_method, .architecture = architecture, .os_release = {}, .macos_version = {} };
    if (!id.is_empty())
        system.os_release = { .id = MUST(String::from_utf8(id)), .version_id = MUST(String::from_utf8(version_id)) };
    return system;
}

TEST_CASE(versions_compare_by_their_numbers)
{
    EXPECT(WebView::is_newer_version("2026.46.10"sv, "2026.46.9"sv));
    EXPECT(WebView::is_newer_version("2026.47.0"sv, "2026.46.10"sv));
    EXPECT(WebView::is_newer_version("2027.1.0"sv, "2026.52.3"sv));
    EXPECT(!WebView::is_newer_version("2026.46.2"sv, "2026.46.2"sv));
    EXPECT(!WebView::is_newer_version("2026.46.1"sv, "2026.46.2"sv));
    EXPECT(WebView::is_newer_version("14.4.1"sv, "14.4"sv));
    EXPECT(!WebView::is_newer_version("14.4.0"sv, "14.4"sv));
}

TEST_CASE(versions_that_are_not_numbers_are_never_newer)
{
    EXPECT(!WebView::parse_version(""sv).has_value());
    EXPECT(!WebView::parse_version("2026..1"sv).has_value());
    EXPECT(!WebView::parse_version("2026.46.1-beta"sv).has_value());
    EXPECT(!WebView::parse_version("+2026.46.1"sv).has_value());
    EXPECT(!WebView::is_newer_version("2026.46.2"sv, "1.0-dev"sv));
    EXPECT(!WebView::is_newer_version("next"sv, "2026.46.2"sv));
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

TEST_CASE(the_mac_app_takes_the_newest_disk_image_that_runs_on_its_macos)
{
    SystemDescription system { .install_method = InstallMethod::MacOSApp, .architecture = Architecture::AArch64, .os_release = {}, .macos_version = "15.1"_string };
    EXPECT_EQ(newest_for(system), "2026.46.10"sv);

    system.macos_version = "14.6.1"_string;
    EXPECT_EQ(newest_for(system), "2026.46.9"sv);

    system.macos_version = "14.3"_string;
    EXPECT(!newest_for(system).has_value());

    system.macos_version = "15.1"_string;
    system.architecture = Architecture::X86_64;
    EXPECT(!newest_for(system).has_value());
}

TEST_CASE(a_package_names_its_format)
{
    EXPECT(WebView::parse_package_format("DEB\n"sv) == InstallMethod::DEB);
    EXPECT(WebView::parse_package_format("RPM"sv) == InstallMethod::RPM);
    EXPECT(!WebView::parse_package_format("Flatpak\n"sv).has_value());
    EXPECT(!WebView::parse_package_format(""sv).has_value());
}

TEST_CASE(packages_match_the_distribution_and_its_version)
{
    EXPECT_EQ(newest_for(linux_system(InstallMethod::DEB, Architecture::X86_64, "ubuntu"sv, "26.04"sv)), "2026.46.10"sv);
    EXPECT_EQ(newest_for(linux_system(InstallMethod::DEB, Architecture::X86_64, "ubuntu"sv, "26.10"sv)), "2026.47.0"sv);
    EXPECT(!newest_for(linux_system(InstallMethod::DEB, Architecture::AArch64, "ubuntu"sv, "26.10"sv)).has_value());
    EXPECT(!newest_for(linux_system(InstallMethod::DEB, Architecture::X86_64, "debian"sv, "26.04"sv)).has_value());
    EXPECT(!newest_for(linux_system(InstallMethod::DEB, Architecture::X86_64)).has_value());

    EXPECT_EQ(newest_for(linux_system(InstallMethod::RPM, Architecture::X86_64, "fedora"sv, "44"sv)), "2026.46.10"sv);
    EXPECT_EQ(newest_for(linux_system(InstallMethod::RPM, Architecture::AArch64, "fedora"sv, "44"sv)), "2026.46.9"sv);
    EXPECT(!newest_for(linux_system(InstallMethod::RPM, Architecture::X86_64, "fedora"sv, "45"sv)).has_value());
}

TEST_CASE(flatpaks_and_builds_from_source_ignore_the_distribution)
{
    EXPECT_EQ(newest_for(linux_system(InstallMethod::Flatpak, Architecture::X86_64)), "2026.46.10"sv);
    EXPECT_EQ(newest_for(linux_system(InstallMethod::Flatpak, Architecture::AArch64)), "2026.46.9"sv);
    EXPECT_EQ(newest_for(linux_system(InstallMethod::Source, Architecture::Other)), "2026.47.0"sv);
}

TEST_CASE(a_release_counts_once_it_was_published_before_the_given_time)
{
    auto system = linux_system(InstallMethod::DEB, Architecture::X86_64, "ubuntu"sv, "26.04"sv);
    auto newest_before = [&](i32 year, u8 month, u8 day) {
        return MUST(WebView::newest_release_for_system(RELEASES, system, UnixDateTime::from_unix_time_parts(year, month, day, 0, 0, 0, 0)));
    };
    EXPECT_EQ(newest_before(2026, 12, 1), "2026.46.10"sv);
    EXPECT(!newest_before(2026, 11, 1).has_value());

    system = linux_system(InstallMethod::Source, Architecture::Other);
    EXPECT_EQ(newest_before(2026, 12, 1), "2026.47.0"sv);
    EXPECT_EQ(newest_before(2026, 11, 15), "2026.46.10"sv);
}

TEST_CASE(a_feed_without_releases_is_an_error)
{
    SystemDescription system;
    EXPECT(WebView::newest_release_for_system("not json"sv, system).is_error());
    EXPECT(WebView::newest_release_for_system("[]"sv, system).is_error());
    EXPECT(WebView::newest_release_for_system(R"({"format": 1})"sv, system).is_error());
    EXPECT(!MUST(WebView::newest_release_for_system(R"({"releases": []})"sv, system)).has_value());
}

TEST_CASE(a_check_is_due_once_the_interval_has_passed)
{
    auto interval = AK::Duration::from_seconds(4 * 60 * 60);
    auto now = UnixDateTime::from_seconds_since_epoch(1'800'000'000);

    EXPECT_EQ(WebView::time_until_next_update_check(UnixDateTime::epoch(), now, interval), AK::Duration::zero());
    EXPECT_EQ(WebView::time_until_next_update_check(now - AK::Duration::from_seconds(60 * 60), now, interval), AK::Duration::from_seconds(3 * 60 * 60));
    EXPECT_EQ(WebView::time_until_next_update_check(now - interval, now, interval), AK::Duration::zero());

    // A last check in the future means that the clock was turned back since.
    EXPECT_EQ(WebView::time_until_next_update_check(now + AK::Duration::from_seconds(60), now, interval), AK::Duration::zero());
}

TEST_CASE(the_release_page_is_the_tag_on_github)
{
    EXPECT_EQ(WebView::release_page_url("2026.46.2"sv), "https://github.com/LadybirdBrowser/ladybird/releases/tag/2026.46.2"sv);
}
