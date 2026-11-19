/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Settings.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WebCompatReportReview.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QCoreApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>

using Ladybird::WebCompatReportReview;
using WebView::ReportSubmission;
using WebView::WebCompatReport;

static WebCompatReport::Page test_page()
{
    return {
        .url = URL::Parser::basic_parse("https://example.com/broken"sv).release_value(),
        .user_agent = "Mozilla/5.0 Ladybird/1.0"_string,
        .viewport_size = { 1280, 720 },
        .device_pixel_ratio = 2,
        .zoom_level = 1,
    };
}

static QImage test_screenshot()
{
    QImage image(400, 200, QImage::Format_RGB32);
    image.fill(Qt::red);
    return image;
}

template<typename T>
static T& child(QWidget& widget, char const* name)
{
    auto* child = widget.findChild<T*>(name);
    VERIFY(child);
    return *child;
}

TEST_CASE(the_review_shows_the_website_and_the_selected_screenshot)
{
    WebCompatReportReview review { test_page(), test_screenshot(), nullptr };
    review.show();

    EXPECT_EQ(child<QLabel>(review, "WebCompatReportWebsite").text(), "example.com/broken");
    EXPECT(child<QWidget>(review, "WebCompatReportScreenshot").isVisible());
    EXPECT_EQ(child<QPushButton>(review, "WebCompatReportSelectScreenshotButton").text(), "Retake");
    EXPECT(child<QPushButton>(review, "WebCompatReportRemoveScreenshotButton").isVisible());
}

TEST_CASE(the_screenshot_opens_in_full)
{
    WebCompatReportReview review { test_page(), test_screenshot(), nullptr };
    Optional<QImage> viewed;
    review.on_view_screenshot = [&](QImage const& screenshot) { viewed = screenshot; };
    review.show();

    EXPECT(!child<QLabel>(review, "WebCompatReportNoScreenshotNote").isVisible());
    child<QAbstractButton>(review, "WebCompatReportScreenshot").click();
    EXPECT(viewed.has_value());
    EXPECT_EQ(viewed->size(), QSize(400, 200));
}

TEST_CASE(a_screenshot_can_be_removed_and_taken_again)
{
    WebCompatReportReview review { test_page(), test_screenshot(), nullptr };
    review.show();

    auto& remove_button = child<QPushButton>(review, "WebCompatReportRemoveScreenshotButton");
    remove_button.click();
    EXPECT(!child<QWidget>(review, "WebCompatReportScreenshot").isVisible());
    EXPECT(!remove_button.isVisible());
    EXPECT(child<QLabel>(review, "WebCompatReportNoScreenshotNote").isVisible());
    EXPECT_EQ(child<QPushButton>(review, "WebCompatReportSelectScreenshotButton").text(), "Take a screenshot");
}

TEST_CASE(a_report_without_a_screenshot_offers_to_take_one)
{
    WebCompatReportReview review { test_page(), {}, nullptr };
    auto selections = 0;
    review.on_select_screenshot = [&] { ++selections; };
    review.show();

    EXPECT(!child<QWidget>(review, "WebCompatReportScreenshot").isVisible());
    EXPECT(!child<QPushButton>(review, "WebCompatReportRemoveScreenshotButton").isVisible());
    auto& select_button = child<QPushButton>(review, "WebCompatReportSelectScreenshotButton");
    EXPECT_EQ(select_button.text(), "Take a screenshot");
    select_button.click();
    EXPECT_EQ(selections, 1);

    review.set_screenshot(test_screenshot());
    EXPECT(child<QWidget>(review, "WebCompatReportScreenshot").isVisible());
    EXPECT_EQ(select_button.text(), "Retake");
}

TEST_CASE(page_dumps_can_be_included_once_they_have_arrived)
{
    WebCompatReportReview review { test_page(), {}, nullptr };
    review.show();

    auto& include_page_dumps = child<QCheckBox>(review, "WebCompatReportIncludePageDumps");
    EXPECT(!include_page_dumps.isEnabled());
    EXPECT(!include_page_dumps.isChecked());
    review.set_page_dumps("Layout tree:\nViewport\n"_string);
    EXPECT(include_page_dumps.isEnabled());
    EXPECT(!include_page_dumps.isChecked());
}

TEST_CASE(the_full_report_is_shown_in_place_of_the_review)
{
    WebCompatReportReview review { test_page(), {}, nullptr };
    Vector<bool> text_visibility;
    review.on_report_text_visibility_change = [&](bool visible) { text_visibility.append(visible); };
    review.show();
    review.set_page_dumps("Layout tree:\nBlockContainer <html>\n"_string);

    auto& send = child<QPushButton>(review, "WebCompatReportSendButton");
    auto& show_report = child<QPushButton>(review, "WebCompatReportShowReportButton");
    auto& text = child<QPlainTextEdit>(review, "ReportText");
    show_report.click();
    EXPECT(text.isVisible());
    EXPECT(!send.isVisible());
    EXPECT(text.toPlainText().contains("https://example.com/broken"));
    EXPECT(!text.toPlainText().contains("BlockContainer <html>"));

    child<QPushButton>(review, "ReportTextBackButton").click();
    EXPECT(!text.isVisible());
    EXPECT(send.isVisible());
    EXPECT_EQ(text_visibility, (Vector<bool> { true, false }));

    child<QCheckBox>(review, "WebCompatReportIncludePageDumps").setChecked(true);
    show_report.click();
    EXPECT(text.toPlainText().contains("BlockContainer <html>"));
}

TEST_CASE(a_report_needs_a_description)
{
    WebCompatReportReview review { test_page(), {}, nullptr };
    review.show();

    child<QPushButton>(review, "WebCompatReportSendButton").click();
    auto& validation = child<QLabel>(review, "WebCompatReportValidation");
    EXPECT(validation.isVisible());
    EXPECT_EQ(validation.text(), "Describe what is wrong with this website.");
    EXPECT(!child<QLabel>(review, "WebCompatReportStatusTitle").isVisible());
}

TEST_CASE(leaving_the_review_closes_its_tab)
{
    WebCompatReportReview review { test_page(), {}, nullptr };
    auto exits = 0;
    review.on_exit = [&] { ++exits; };
    review.show();

    auto& review_close = child<QPushButton>(review, "WebCompatReportReviewCloseButton");
    EXPECT_EQ(review_close.text(), "Cancel");
    review_close.click();
    EXPECT_EQ(exits, 1);

    review.show_sent();
    auto& back_to_site = child<QPushButton>(review, "WebCompatReportBackToSiteButton");
    EXPECT_EQ(back_to_site.text(), "Back to the site");
    EXPECT(back_to_site.isVisible());
    back_to_site.click();
    EXPECT_EQ(exits, 2);

    // Closing the tab once the report is sent does not have to return to the site.
    auto closes = 0;
    review.on_close_tab = [&] { ++closes; };
    auto& close = child<QPushButton>(review, "WebCompatReportCloseButton");
    EXPECT_EQ(close.text(), "Close tab");
    close.click();
    EXPECT_EQ(exits, 2);
    EXPECT_EQ(closes, 1);
}

TEST_CASE(sending_can_be_canceled_until_the_report_is_sent)
{
    WebCompatReportReview review { test_page(), {}, nullptr };
    review.show();

    auto& description = child<QPlainTextEdit>(review, "WebCompatReportDescription");
    description.setPlainText("The menu does not open");
    review.show_progress(ReportSubmission::Stage::SolvingProof);
    auto& title = child<QLabel>(review, "WebCompatReportStatusTitle");
    auto& retry = child<QPushButton>(review, "WebCompatReportRetryButton");
    auto& cancel = child<QPushButton>(review, "WebCompatReportCancelButton");
    auto& close = child<QPushButton>(review, "WebCompatReportCloseButton");
    EXPECT_EQ(title.text(), "Sending report");
    EXPECT(!retry.isVisible());
    EXPECT(cancel.isVisible());
    EXPECT(!cancel.isDefault());
    EXPECT(!close.isVisible());

    review.show_failure(ReportSubmission::Failure::Sending, "Network error while contacting the report server."_string);
    EXPECT_EQ(title.text(), "Couldn’t send report");
    EXPECT(retry.isVisible());
    EXPECT(retry.isDefault());
    EXPECT(cancel.isVisible());
    EXPECT(!close.isVisible());

    cancel.click();
    EXPECT(child<QPushButton>(review, "WebCompatReportSendButton").isVisible());
    EXPECT_EQ(description.toPlainText(), "The menu does not open");

    review.show_sent();
    EXPECT_EQ(title.text(), "Report sent");
    EXPECT(!retry.isVisible());
    EXPECT(!cancel.isVisible());
    EXPECT(close.isVisible());
    EXPECT(!close.isDefault());
    EXPECT(child<QPushButton>(review, "WebCompatReportBackToSiteButton").isVisible());
}

TEST_CASE(a_screenshot_is_shrunk_to_fit_a_report)
{
    EXPECT(!WebCompatReportReview::encode_screenshot({}).has_value());

    auto small = WebCompatReportReview::encode_screenshot(test_screenshot());
    EXPECT(small.has_value());
    auto decoded_small = QImage::fromData(small->bytes().data(), small->size(), "PNG");
    EXPECT_EQ(decoded_small.size(), QSize(400, 200));

    QImage large(4000, 3000, QImage::Format_RGB32);
    large.fill(Qt::blue);
    auto encoded = WebCompatReportReview::encode_screenshot(large);
    EXPECT(encoded.has_value());
    EXPECT(encoded->size() <= WebCompatReport::maximum_screenshot_bytes);
    auto decoded = QImage::fromData(encoded->bytes().data(), encoded->size(), "PNG");
    EXPECT(static_cast<size_t>(decoded.width()) * decoded.height() <= WebCompatReport::maximum_screenshot_pixels);
    EXPECT(decoded.width() > 3000);
}

// The offscreen platform keeps Qt's light palette whatever color scheme is asked for, as some platform themes do.
TEST_CASE(the_review_takes_the_dark_interface_theme_even_where_qt_keeps_a_light_palette)
{
    Ladybird::Theme::set_interface_theme(WebView::ThemePreference::Dark);
    WebCompatReportReview review { test_page(), test_screenshot(), nullptr };
    review.show();

    auto const& website = child<QLabel>(review, "WebCompatReportWebsite");
    EXPECT(review.palette().color(QPalette::Window).lightness() < 128);
    EXPECT(website.palette().color(website.foregroundRole()).lightness() > 128);

    // A switch of the interface theme restyles every widget, which takes the review back to Qt's palette.
    Ladybird::Theme::set_interface_theme(WebView::ThemePreference::Light);
    QEvent palette_change(QEvent::PaletteChange);
    QCoreApplication::sendEvent(&review, &palette_change);
    EXPECT(review.palette().color(QPalette::Window).lightness() > 128);
    EXPECT(website.palette().color(website.foregroundRole()).lightness() < 128);

    Ladybird::Theme::set_interface_theme(WebView::ThemePreference::System);
}

// A review is shown over the page in an overlay that has the interface theme's colors already.
TEST_CASE(a_review_inside_a_dark_overlay_keeps_its_labels_light_where_qt_keeps_a_light_palette)
{
    Ladybird::Theme::set_interface_theme(WebView::ThemePreference::Dark);
    QWidget overlay;
    overlay.setPalette(Ladybird::Theme::interface_palette(overlay.palette()));
    auto* review = new WebCompatReportReview { test_page(), test_screenshot(), &overlay };
    overlay.show();

    auto const& website = child<QLabel>(*review, "WebCompatReportWebsite");
    EXPECT(website.palette().color(website.foregroundRole()).lightness() > 128);

    Ladybird::Theme::set_interface_theme(WebView::ThemePreference::System);
}
