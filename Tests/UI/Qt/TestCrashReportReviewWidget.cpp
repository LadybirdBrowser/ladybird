/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/CrashReportReviewWidget.h>
#include <UI/Qt/StringUtils.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyleHints>
#include <unistd.h>

using Ladybird::CrashReportReviewWidget;
using WebView::CrashReportSubmission;

static constexpr auto report_text = "Ladybird crash report, format 1\n"
                                    "Process: WebContent\n"
                                    "Version: 1.0\n"
                                    "Termination signal: SIGSEGV\n"
                                    "Termination signal number: 11\n"
                                    "\n"
                                    "Native stack (binary build ID, object address):\n"
                                    "#0 abcdef 0x1234 Web::Page::crash()\n"sv;

static ByteString test_directory()
{
    auto name = ByteString::formatted("test-crash-report-review-widget-{}", getpid());
    return LexicalPath::join(Core::StandardPaths::tempfile_directory(), name).string();
}

static void cleanup()
{
    if (FileSystem::exists(test_directory()))
        MUST(FileSystem::remove(test_directory(), FileSystem::RecursionMode::Allowed));
}

static ByteString write_report(StringView timestamp)
{
    auto name = ByteString::formatted("{}-WebContent-abc123.txt", timestamp);
    auto directory = MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    auto file = MUST(directory.open(name, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(report_text.bytes()));
    return name;
}

template<typename T>
static T& child(QWidget& widget, char const* name)
{
    auto* child = widget.findChild<T*>(name);
    VERIFY(child);
    return *child;
}

static QString status_title(QWidget& widget)
{
    return child<QLabel>(widget, "CrashReportStatusTitle").text();
}

TEST_CASE(a_crash_screen_offers_the_website_without_including_it)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReviewWidget widget { CrashReportReviewWidget::Mode::Tab, nullptr, store };
    MUST(widget.open_report(name, "https://example.com/page"_string));
    widget.show();

    // The crash screen around the review already names what crashed.
    EXPECT(!child<QLabel>(widget, "CrashReportTitle").isVisible());
    auto& website = child<QLineEdit>(widget, "CrashReportWebsite");
    auto& include_website = child<QCheckBox>(widget, "CrashReportIncludeWebsite");
    EXPECT_EQ(website.text(), "https://example.com/page");
    EXPECT(!include_website.isChecked());
    EXPECT(!website.isEnabled());
    include_website.setChecked(true);
    EXPECT(website.isEnabled());

    child<QPushButton>(widget, "CrashReportDeclineButton").click();
    EXPECT_EQ(status_title(widget), "Report not sent");
    EXPECT(!child<QPushButton>(widget, "CrashReportNextButton").isVisible());
    EXPECT(!child<QPushButton>(widget, "CrashReportCloseButton").isVisible());
}

TEST_CASE(a_crash_screen_offers_to_reload_the_page_before_and_after_answering)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReviewWidget widget { CrashReportReviewWidget::Mode::Tab, nullptr, store };
    auto reloads = 0;
    widget.on_reload = [&] { ++reloads; };
    MUST(widget.open_report(name));
    widget.show();

    auto& review_reload = child<QPushButton>(widget, "CrashReportReviewReloadButton");
    EXPECT(review_reload.isVisible());
    EXPECT(child<QPushButton>(widget, "CrashReportSendButton").isDefault());
    review_reload.click();
    EXPECT_EQ(reloads, 1);

    // Once the report is answered, reloading is what is left to do.
    widget.show_sent();
    auto& reload = child<QPushButton>(widget, "CrashReportReloadButton");
    EXPECT(reload.isVisible());
    EXPECT(reload.isDefault());
    reload.click();
    EXPECT_EQ(reloads, 2);

    // A report that may still be sent offers both.
    widget.show_failure(CrashReportSubmission::Failure::Sending, "Network error while contacting the report server."_string);
    EXPECT(child<QPushButton>(widget, "CrashReportRetryButton").isVisible());
    EXPECT(reload.isVisible());
    EXPECT(!reload.isDefault());
}

TEST_CASE(a_choice_that_cannot_be_sent_is_explained_in_place)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReviewWidget widget { CrashReportReviewWidget::Mode::Tab, nullptr, store };
    MUST(widget.open_report(name, "https://example.com/"_string));
    widget.show();

    child<QCheckBox>(widget, "CrashReportIncludeWebsite").setChecked(true);
    child<QLineEdit>(widget, "CrashReportWebsite").setText("  ");
    child<QPushButton>(widget, "CrashReportSendButton").click();

    auto& validation = child<QLabel>(widget, "CrashReportValidation");
    EXPECT(validation.isVisible());
    EXPECT_EQ(validation.text(), "Enter a website URL or leave the option unchecked.");
    EXPECT(child<QPushButton>(widget, "CrashReportSendButton").isVisible());
}

TEST_CASE(the_dialog_walks_through_every_waiting_report)
{
    cleanup();
    ScopeGuard guard = cleanup;

    write_report("2026-10-01T12-00-00Z"sv);
    write_report("2026-10-01T13-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReviewWidget widget { CrashReportReviewWidget::Mode::Dialog, nullptr, store };
    auto closed = false;
    widget.on_close = [&] { closed = true; };
    MUST(widget.open_report());
    widget.show();

    auto& title = child<QLabel>(widget, "CrashReportTitle");
    EXPECT(title.isVisible());
    EXPECT(!widget.findChild<QPushButton*>("CrashReportReviewReloadButton"));
    EXPECT(!widget.findChild<QPushButton*>("CrashReportReloadButton"));
    EXPECT_EQ(title.text(), "A web page crashed");

    // Without a website to offer, there is nothing to include.
    EXPECT(!child<QCheckBox>(widget, "CrashReportIncludeWebsite").isVisible());

    child<QPushButton>(widget, "CrashReportDeclineButton").click();
    auto& next = child<QPushButton>(widget, "CrashReportNextButton");
    EXPECT(next.isVisible());

    next.click();
    EXPECT(child<QPushButton>(widget, "CrashReportSendButton").isVisible());
    EXPECT(!store.has_pending_reports());

    child<QPushButton>(widget, "CrashReportDeclineButton").click();
    EXPECT(!next.isVisible());
    auto& close = child<QPushButton>(widget, "CrashReportCloseButton");
    EXPECT(close.isVisible());

    close.click();
    EXPECT(closed);
}

TEST_CASE(sending_shows_its_progress_and_outcome)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReviewWidget widget { CrashReportReviewWidget::Mode::Dialog, nullptr, store };
    MUST(widget.open_report(name));
    widget.show();

    auto& message = child<QLabel>(widget, "CrashReportStatusMessage");
    auto& retry = child<QPushButton>(widget, "CrashReportRetryButton");

    widget.show_progress(CrashReportSubmission::Stage::SolvingProof);
    EXPECT_EQ(status_title(widget), "Sending crash report");
    EXPECT_EQ(message.text(), "Preparing report…");

    widget.show_retry("Report server returned 503 while sending the report."_string, 2, 5, 3);
    EXPECT_EQ(message.text(), "Report server returned 503 while sending the report. Trying again in 3 seconds (2 of 5)…");

    // Only a report that failed to reach the server can be sent again.
    widget.show_failure(CrashReportSubmission::Failure::Sending, "Network error while contacting the report server."_string);
    EXPECT_EQ(status_title(widget), "Couldn’t send report");
    EXPECT(retry.isVisible());
    EXPECT(!child<QPushButton>(widget, "CrashReportCloseButton").isVisible());

    widget.show_failure(CrashReportSubmission::Failure::Preparation, "Could not prepare this report."_string);
    EXPECT(!retry.isVisible());
    EXPECT(child<QPushButton>(widget, "CrashReportCloseButton").isVisible());

    widget.show_sent();
    EXPECT_EQ(status_title(widget), "Report sent");
    EXPECT_EQ(message.text(), "Thank you for helping Ladybird.");
}

TEST_CASE(the_details_toggle_is_written_like_the_text_around_it)
{
    cleanup();
    ScopeGuard guard = cleanup;

    // Styles such as macOS's give some buttons a smaller font of their own.
    auto label_font = QApplication::font("QLabel");
    auto button_font = label_font;
    button_font.setPointSizeF(label_font.pointSizeF() - 2);
    QApplication::setFont(button_font, "QPushButton");
    ScopeGuard restore_font = [&] { QApplication::setFont(label_font, "QPushButton"); };

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReviewWidget widget { CrashReportReviewWidget::Mode::Tab, nullptr, store };
    MUST(widget.open_report(name));

    QPushButton* toggle = nullptr;
    for (auto* button : widget.findChildren<QPushButton*>()) {
        if (button->text().endsWith("Report details"))
            toggle = button;
    }
    VERIFY(toggle);
    EXPECT_EQ(toggle->font().pointSizeF(), child<QLabel>(widget, "CrashReportValidation").font().pointSizeF());
}

TEST_CASE(tab_moves_through_every_control_and_wraps_around)
{
    cleanup();
    ScopeGuard guard = cleanup;

    // As on macOS unless its keyboard navigation setting is on, Tab only moves between text fields, and buttons take focus
    // from Tab alone.
    auto tab_focus_behavior = QGuiApplication::styleHints()->tabFocusBehavior();
    QGuiApplication::styleHints()->setTabFocusBehavior(Qt::TabFocusTextControls);
    ScopeGuard restore_tab_focus_behavior = [&] { QGuiApplication::styleHints()->setTabFocusBehavior(tab_focus_behavior); };

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReviewWidget widget { CrashReportReviewWidget::Mode::Tab, nullptr, store };
    MUST(widget.open_report(name, "https://example.com/"_string));
    for (auto* button : widget.findChildren<QAbstractButton*>())
        button->setFocusPolicy(Qt::TabFocus);
    widget.show();

    auto press = [&](Qt::Key key) {
        auto* focused = widget.window()->focusWidget();
        VERIFY(focused);
        QKeyEvent event(QEvent::KeyPress, key, key == Qt::Key_Backtab ? Qt::ShiftModifier : Qt::NoModifier);
        QApplication::sendEvent(focused, &event);
        return widget.window()->focusWidget();
    };

    auto& description = child<QPlainTextEdit>(widget, "CrashReportDescription");
    auto& include_website = child<QCheckBox>(widget, "CrashReportIncludeWebsite");
    auto& send = child<QPushButton>(widget, "CrashReportSendButton");
    auto& reload = child<QPushButton>(widget, "CrashReportReviewReloadButton");
    description.setFocus();

    // The URL field is skipped while its option is off, and the details' controls while they are collapsed.
    EXPECT_EQ(press(Qt::Key_Tab), &include_website);
    auto* toggle = press(Qt::Key_Tab);
    EXPECT(static_cast<QPushButton*>(toggle)->text().endsWith("Report details"));
    EXPECT_EQ(press(Qt::Key_Tab), &send);
    EXPECT_EQ(press(Qt::Key_Tab), &child<QPushButton>(widget, "CrashReportDeclineButton"));
    EXPECT_EQ(press(Qt::Key_Tab), &reload);
    EXPECT_EQ(press(Qt::Key_Tab), &description);
    EXPECT_EQ(press(Qt::Key_Backtab), &reload);

    // The details' controls follow the toggle that shows them.
    static_cast<QPushButton*>(toggle)->click();
    toggle->setFocus();
    EXPECT_EQ(press(Qt::Key_Tab), &child<QPushButton>(widget, "CrashReportShowReportButton"));
    EXPECT_EQ(press(Qt::Key_Tab), &send);

    include_website.setChecked(true);
    include_website.setFocus();
    EXPECT_EQ(press(Qt::Key_Tab), &child<QLineEdit>(widget, "CrashReportWebsite"));
}

TEST_CASE(answering_the_report_is_announced_once_the_choice_is_made)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto name = write_report("2026-10-01T12-00-00Z"sv);
    WebView::CrashReportStore store { test_directory() };
    CrashReportReviewWidget widget { CrashReportReviewWidget::Mode::Tab, nullptr, store };
    auto answers = 0;
    widget.on_answered = [&] { ++answers; };
    MUST(widget.open_report(name, "https://example.com/"_string));
    widget.show();

    // A choice that cannot be sent is not an answer yet.
    child<QCheckBox>(widget, "CrashReportIncludeWebsite").setChecked(true);
    child<QLineEdit>(widget, "CrashReportWebsite").clear();
    child<QPushButton>(widget, "CrashReportSendButton").click();
    EXPECT_EQ(answers, 0);

    child<QPushButton>(widget, "CrashReportDeclineButton").click();
    EXPECT_EQ(answers, 1);
}
