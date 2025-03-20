/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

enum class NewWindow {
    No,
    Yes,
};

enum class ForceNewProcess {
    No,
    Yes,
};

enum class AllowPopups {
    No,
    Yes,
};

enum class DisableScripting {
    No,
    Yes,
};

enum class DisableSQLDatabase {
    No,
    Yes,
};

enum class EnableAutoplay {
    No,
    Yes,
};

struct SystemDNS { };
struct DNSOverTLS {
    ByteString server_address;
    u16 port;
};
struct DNSOverUDP {
    ByteString server_address;
    u16 port;
};

using DNSSettings = Variant<SystemDNS, DNSOverTLS, DNSOverUDP>;

constexpr inline u16 default_devtools_port = 6000;

struct BrowserOptions {
    Vector<URL::URL> urls;
    Vector<ByteString> raw_urls;
    Vector<ByteString> certificates {};
    NewWindow new_window { NewWindow::No };
    ForceNewProcess force_new_process { ForceNewProcess::No };
    AllowPopups allow_popups { AllowPopups::No };
    DisableScripting disable_scripting { DisableScripting::No };
    DisableSQLDatabase disable_sql_database { DisableSQLDatabase::No };
    Optional<ProcessType> debug_helper_process {};
    Optional<ProcessType> profile_helper_process {};
    Optional<ByteString> webdriver_content_ipc_path {};
    DNSSettings dns_settings { SystemDNS {} };
    u16 devtools_port { default_devtools_port };
};

enum class IsLayoutTestMode {
    No,
    Yes,
};

enum class LogAllJSExceptions {
    No,
    Yes,
};

enum class EnableIDLTracing {
    No,
    Yes,
};

enum class EnableHTTPCache {
    No,
    Yes,
};

enum class DisableSiteIsolation {
    No,
    Yes,
};

enum class ExposeInternalsObject {
    No,
    Yes,
};

enum class ForceCPUPainting {
    No,
    Yes,
};

enum class ForceFontconfig {
    No,
    Yes,
};

enum class CollectGarbageOnEveryAllocation {
    No,
    Yes,
};

enum class IsHeadless {
    No,
    Yes,
};

enum class PaintViewportScrollbars {
    Yes,
    No,
};

struct WebContentOptions {
    String command_line;
    String executable_path;
    Optional<ByteString> config_path {};
    Optional<StringView> user_agent_preset {};
    IsLayoutTestMode is_layout_test_mode { IsLayoutTestMode::No };
    LogAllJSExceptions log_all_js_exceptions { LogAllJSExceptions::No };
    DisableSiteIsolation disable_site_isolation { DisableSiteIsolation::No };
    EnableIDLTracing enable_idl_tracing { EnableIDLTracing::No };
    EnableHTTPCache enable_http_cache { EnableHTTPCache::No };
    ExposeInternalsObject expose_internals_object { ExposeInternalsObject::No };
    ForceCPUPainting force_cpu_painting { ForceCPUPainting::No };
    ForceFontconfig force_fontconfig { ForceFontconfig::No };
    EnableAutoplay enable_autoplay { EnableAutoplay::No };
    CollectGarbageOnEveryAllocation collect_garbage_on_every_allocation { CollectGarbageOnEveryAllocation::No };
    Optional<u16> echo_server_port {};
    IsHeadless is_headless { IsHeadless::No };
    PaintViewportScrollbars paint_viewport_scrollbars { PaintViewportScrollbars::Yes };
};

}
