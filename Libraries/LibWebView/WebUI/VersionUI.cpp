/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/NeverDestroyed.h>
#include <AK/String.h>
#include <LibCore/System.h>
#include <LibWeb/Loader/UserAgent.h>
#include <LibWebView/Application.h>
#include <LibWebView/BuildInformation.h>
#include <LibWebView/WebUI/VersionUI.h>

namespace WebView {

void VersionUI::register_interfaces()
{
    register_interface("loadVersionInfo"sv, [this](auto const&) {
        load_version_info();
    });
}

void VersionUI::load_version_info()
{
    static NeverDestroyed<String> browser_name = String::from_utf8_without_validation({ BROWSER_NAME, __builtin_strlen(BROWSER_NAME) });
    static NeverDestroyed<String> browser_version = String::from_utf8_without_validation({ BROWSER_VERSION, __builtin_strlen(BROWSER_VERSION) });
    static NeverDestroyed<String> browser_commit = String::from_utf8_without_validation(LADYBIRD_COMMIT.bytes());
    static NeverDestroyed<String> arch = String::from_utf8_without_validation({ CPU_STRING, __builtin_strlen(CPU_STRING) });
    static NeverDestroyed<String> platform_name = String::from_utf8_without_validation({ OS_STRING, __builtin_strlen(OS_STRING) });
    static NeverDestroyed<String> command_line = MUST(String::join(' ', Application::the().command_line_arguments().strings));
    static NeverDestroyed<String> executable_path = MUST(String::from_byte_string(MUST(Core::System::current_executable_path())));

    JsonObject version_info;
    version_info.set("browserName"_string, *browser_name);
    version_info.set("browserVersion"_string, *browser_version);
    version_info.set("browserCommit"_string, *browser_commit);
    version_info.set("arch"_string, *arch);
    version_info.set("platformName"_string, *platform_name);
    version_info.set("commandLine"_string, *command_line);
    version_info.set("executablePath"_string, *executable_path);

    async_send_message("renderVersionInfo"sv, version_info);
}

}
