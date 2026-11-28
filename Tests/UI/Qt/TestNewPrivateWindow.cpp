/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/StringUtils.h>

#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QtCore/qtestsupport_core.h>

struct BrowserSession {
    QTemporaryDir profile;
    QString file_path = profile.filePath("tab.html");
    QString page_url = QUrl::fromLocalFile(file_path).toString(QUrl::FullyEncoded) + "?";
    // Tuple origins let the local test page use localStorage across launches.
    QStringList options { "--profile-path=" + profile.path(), "--tuple-file-origins" };
    QProcess browser;

    ErrorOr<void> start_regular_session(bool restore_previous_session = false)
    {
        auto file = TRY(Core::File::open(ak_byte_string_from_qstring(file_path), Core::File::OpenMode::Write));
        TRY(file->write_until_depleted(R"(<script>onpageshow = () => {
            let page = location.search.slice(1) || "file";
            if (page === "requested" || page === "file")
                page += " " + document.visibilityState;
            const storage = localStorage.length === 0 ? "empty localStorage" : `localStorage.session=${localStorage.getItem("session")}`;
            console.log(`[${page} ${storage}] test page`);
            if (page === "setup") {
                localStorage.session = "regular";
                location.replace("?saved");
            }
        };</script>)"sv));

        auto settings = WebView::Settings::create(ak_byte_string_from_qstring(profile.filePath("config/Settings.json")));
        TRY(settings.complete_first_run());
        settings.set_background_networking_enabled(false);
        settings.set_restore_previous_session(restore_previous_session);
        settings.set_new_tab_page_url(ak_url_from_qurl(QUrl(page_url + "new-tab")));

        auto environment = QProcessEnvironment::systemEnvironment();
        for (auto variable : { "XDG_DATA_HOME", "XDG_CACHE_HOME", "LOCALAPPDATA" })
            environment.insert(variable, profile.path());
#ifndef AK_OS_LINUX
        // Only the Linux test build imports the offscreen plugin into the browser.
        environment.remove("QT_QPA_PLATFORM");
#endif
#if defined(AK_OS_MACOS) && defined(HAS_ADDRESS_SANITIZER)
        // When AppKit lays out the title bar, ASan's strict string checks read past CoreGraphics' strndup length.
        environment.insert("ASAN_OPTIONS", environment.value("ASAN_OPTIONS") + ":strict_string_checks=0");
#endif
        browser.setProcessEnvironment(environment);
        // Let the saved page read the value back before a test can kill the browser.
        browser.start(LADYBIRD_EXECUTABLE, options + QStringList { page_url + "setup" });
        expect_pages({ "setup empty localStorage", "saved localStorage.session=regular" });
        return {};
    }

    void expect_pages(QList<QByteArray> const& expected)
    {
        QByteArray output;
        (void)QTest::qWaitFor([&] {
            output += browser.readAllStandardError();
            return output.count("] test page") >= expected.size() || browser.state() == QProcess::NotRunning;
        },
            15'000);
        EXPECT_EQ(output.count("] test page"), expected.size());
        for (auto const& page : expected) {
            if (!output.contains('[' + page + "] test page"))
                FAIL(ByteString::formatted("Missing page '{}': {}", page.constData(), output.constData()));
        }
    }

    void wait_for_exit()
    {
        EXPECT(QTest::qWaitFor([&] { return browser.state() == QProcess::NotRunning; }, 5'000));
#ifdef LADYBIRD_HUB_IN_OWN_PROCESS
        // The Hub releases the profile after it learns the UI is gone.
        EXPECT(QTest::qWaitFor([&] { return !QFileInfo::exists(profile.filePath("runtime/Ladybird.pid")); }, 5'000));
#endif
    }

    ~BrowserSession()
    {
        if (!profile.isValid())
            return;
        browser.kill();
        wait_for_exit();
        // The Hub can still unlink its pending crash report while we remove the profile.
        EXPECT(QTest::qWaitFor([&] { return profile.remove(); }, 5'000));
    }
};

TEST_CASE(running_browser_opens_supplied_urls_in_one_private_window)
{
    BrowserSession session;
    TRY_OR_FAIL(session.start_regular_session());

    // With URLs, -n --private opens them together and selects the first tab.
    QProcess launcher;
    launcher.setProcessEnvironment(session.browser.processEnvironment());
    launcher.start(LADYBIRD_EXECUTABLE, session.options + QStringList { "-n", "--private", session.page_url + "requested", session.file_path });
    EXPECT(launcher.waitForFinished(15'000));
    EXPECT(launcher.exitStatus() == QProcess::NormalExit);
    EXPECT_EQ(launcher.exitCode(), 0);
    session.expect_pages({ "requested visible empty localStorage", "file hidden empty localStorage" });
}

TEST_CASE(private_launch_restores_saved_tabs_in_a_regular_window)
{
    BrowserSession session;
    TRY_OR_FAIL(session.start_regular_session(/* restore_previous_session */ true));
    session.browser.terminate();
    session.wait_for_exit();
    EXPECT(session.browser.exitStatus() == QProcess::NormalExit);
    EXPECT_EQ(session.browser.exitCode(), 0);

    // After a clean exit the saved tab gets its regular storage and the private URL starts empty.
    session.browser.start(LADYBIRD_EXECUTABLE, session.options + QStringList { "--new-window", "--private", session.page_url + "after-restart" });
    session.expect_pages({ "saved localStorage.session=regular", "after-restart empty localStorage" });
}

TEST_CASE(private_launch_after_a_crash_opens_a_regular_recovery_window)
{
    BrowserSession session;
    TRY_OR_FAIL(session.start_regular_session());
    session.browser.kill();
    session.wait_for_exit();

    // After an unclean exit both the private window and the regular recovery window open the new-tab page.
    session.browser.start(LADYBIRD_EXECUTABLE, session.options + QStringList { "--new-window", "--private" });
    session.expect_pages({ "new-tab empty localStorage", "new-tab localStorage.session=regular" });
}
