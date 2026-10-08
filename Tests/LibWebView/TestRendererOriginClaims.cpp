/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/Page/NavigationTarget.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace {

// Neither WebDriver nor the test harness drives this application, so its renderers get no HTTP-like cookie access.
class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

}

// A navigation's initiator origin is its source document's origin, or a new opaque origin without a source document.
// The UI process holds every document's origin, so a renderer claiming another origin for a navigation it starts has
// the navigation refused.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestRendererOriginClaims-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto& stub = static_cast<WebContentClientStub&>(view->page().client());
    auto page_id = view->page().id();
    auto& traversable = view->traversable();

    auto start_navigation = [&](URL::Origin initiator_origin) {
        auto uuid = generate_random_uuid();
        auto navigation_id = Utf16String::from_utf8_without_validation(uuid.bytes());
        Web::HTML::NavigationStartRequest request {
            .navigable_id = traversable.id(),
            .url = URL::about_blank(),
            .document_resource = {},
            .request_referrer = Web::Fetch::Infrastructure::RequestReferrer::Client,
            .request_referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::EmptyString,
            .initiator_origin = move(initiator_origin),
            .initiator_base_url = {},
            .navigable_target_name = {},
            .source_snapshot_params = Web::HTML::create_navigation_source_snapshot_without_a_source_document(),
            .target_snapshot_params = {},
            .csp_navigation_type = Web::ContentSecurityPolicy::Directives::NavigationType::Other,
            .history_handling = Web::Bindings::NavigationHistoryBehavior::Replace,
            .user_involvement = Web::HTML::UserNavigationInvolvement::None,
            .navigation_id = navigation_id,
            .classic_history_api_state = {},
            .navigation_api_state = {},
            .navigation_api_key = {},
            .navigation_api_id = {},
        };
        stub.did_request_navigation_start(page_id, traversable.id(), Web::NavigationTarget::TopLevel, URL::about_blank(), navigation_id, move(request));
        return traversable.navigation_with_id(navigation_id) != nullptr;
    };

    // An about:blank document takes its initiator origin, so a victim's would give the renderer a document of its origin.
    auto victim_origin = URL::Parser::basic_parse("https://victim.example/"sv)->origin();
    VERIFY(!start_navigation(victim_origin));

    // So would the origin of a document the UI process already holds.
    VERIFY(!start_navigation(traversable.active_document().origin()));

    // A new opaque origin is what a navigation without a source document has.
    VERIFY(start_navigation(URL::Origin::create_opaque()));

    outln("PASS: renderers cannot start a navigation as another origin");
    return 0;
}
