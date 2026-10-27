/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWebView/ReportSubmission.h>
#include <LibWebView/WebCompatReport.h>

#include <QImage>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace Ladybird {

class ScreenshotThumbnail;

class WebCompatReportReview final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    static Optional<ByteBuffer> encode_screenshot(QImage const&);

    WebCompatReportReview(WebView::WebCompatReport::Page, QImage screenshot, QWidget* parent);

    void set_screenshot(QImage);
    void set_page_dumps(String);

    void show_progress(WebView::ReportSubmission::Stage);
    void show_retry(String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds);
    void show_sent();
    void show_failure(WebView::ReportSubmission::Failure, String const& reason);

    Function<void()> on_select_screenshot;
    Function<void(QImage const&)> on_view_screenshot;
    // Leaving the review returns to the site it is about; closing the tab once the report is sent need not.
    Function<void()> on_exit;
    Function<void()> on_close_tab;
    Function<void(bool)> on_report_text_visibility_change;

protected:
    virtual bool event(QEvent*) override;
    virtual bool focusNextPrevChild(bool next) override;

private:
    void send();
    void exit();
    void show_report_text();
    void show_page(QWidget&);
    void show_status(QString const& title, QString const& message, Vector<QPushButton*> const& actions,
        QPushButton* primary = nullptr);
    void update_chrome_style();

    WebView::WebCompatReport::IncludePageDumps include_page_dumps() const;

    WebView::WebCompatReport m_report;
    QImage m_screenshot;

    QWidget* m_review_page { nullptr };
    QWidget* m_status_page { nullptr };
    QWidget* m_report_text_page { nullptr };

    QPlainTextEdit* m_description_edit { nullptr };
    ScreenshotThumbnail* m_screenshot_preview { nullptr };
    QLabel* m_no_screenshot_note { nullptr };
    QLabel* m_selected_area_label { nullptr };
    QPushButton* m_select_screenshot_button { nullptr };
    QPushButton* m_remove_screenshot_button { nullptr };
    QCheckBox* m_include_page_dumps { nullptr };
    QPushButton* m_show_report_button { nullptr };
    QPlainTextEdit* m_report_text { nullptr };
    QLabel* m_validation { nullptr };

    QLabel* m_status_icon { nullptr };
    QLabel* m_status_title { nullptr };
    QProgressBar* m_progress { nullptr };
    QLabel* m_status_message { nullptr };
    QPushButton* m_retry_button { nullptr };
    QPushButton* m_cancel_button { nullptr };
    QPushButton* m_back_to_site_button { nullptr };
    QPushButton* m_close_button { nullptr };

    bool m_is_updating_chrome_style { false };
};

}
