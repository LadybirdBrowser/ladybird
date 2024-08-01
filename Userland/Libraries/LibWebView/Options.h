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

enum class DisableSQLDatabase {
    No,
    Yes,
};

struct ChromeOptions {
    Vector<URL::URL> urls;
    Vector<ByteString> raw_urls;
    URL::URL new_tab_page_url;
    Vector<ByteString> certificates {};
    NewWindow new_window { NewWindow::No };
    ForceNewProcess force_new_process { ForceNewProcess::No };
    AllowPopups allow_popups { AllowPopups::No };
    DisableSQLDatabase disable_sql_database { DisableSQLDatabase::No };
    Optional<ProcessType> debug_helper_process {};
    Optional<ProcessType> profile_helper_process {};
    Optional<ByteString> webdriver_content_ipc_path {};
};

enum class IsLayoutTestMode {
    No,
    Yes,
};

enum class UseLagomNetworking {
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

enum class ExposeInternalsObject {
    No,
    Yes,
};

struct WebContentOptions {
    String command_line;
    String executable_path;
    Optional<ByteString> config_path {};
    IsLayoutTestMode is_layout_test_mode { IsLayoutTestMode::No };
    UseLagomNetworking use_lagom_networking { UseLagomNetworking::Yes };
    LogAllJSExceptions log_all_js_exceptions { LogAllJSExceptions::No };
    EnableIDLTracing enable_idl_tracing { EnableIDLTracing::No };
    EnableHTTPCache enable_http_cache { EnableHTTPCache::No };
    ExposeInternalsObject expose_internals_object { ExposeInternalsObject::No };
};

}
