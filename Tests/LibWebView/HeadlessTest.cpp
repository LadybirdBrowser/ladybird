/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibFileSystem/FileSystem.h>
#include <time.h>
#include <unistd.h>

namespace Test {

HeadlessApplication::HeadlessApplication(Optional<ByteString> ladybird_binary_path)
    : WebView::Application(move(ladybird_binary_path))
{
}

void HeadlessApplication::create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options)
{
    browser_options.headless_mode = WebView::HeadlessMode::Test;
    browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
    web_content_options.is_test_mode = WebView::IsTestMode::Yes;
}

HeadlessTestEnvironment::HeadlessTestEnvironment(StringView name, unsigned timeout_seconds)
    : m_directory(ByteString::formatted("{}/Ladybird-{}-{}", Core::StandardPaths::tempfile_directory(), name, generate_random_uuid()))
{
    // Each source suite runs in a fresh process. Keep its deadline even if its event loop stops making progress.
    alarm(timeout_seconds);

    MUST(Core::Directory::create(m_directory, Core::Directory::CreateDirectories::Yes));
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, m_directory, Core::Environment::Overwrite::Yes));

    tzset();
    if (auto result = Core::System::set_resource_limits(RLIMIT_NOFILE, 65536); result.is_error())
        warnln("Unable to increase open file limit: {}", result.error());
}

HeadlessTestEnvironment::~HeadlessTestEnvironment()
{
    Core::ThreadEventQueue::destroy_current();
    MUST(FileSystem::remove(m_directory, FileSystem::RecursionMode::Allowed));
    alarm(0);
}

Optional<ByteString> HeadlessTestEnvironment::ladybird_binary_path()
{
#if defined(LADYBIRD_BINARY_PATH)
    return LADYBIRD_BINARY_PATH;
#else
    return {};
#endif
}

}
