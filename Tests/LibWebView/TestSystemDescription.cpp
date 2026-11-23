/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/SystemDescription.h>

using WebView::InstallMethod;

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
