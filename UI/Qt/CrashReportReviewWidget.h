/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibWebView/CrashReportReview.h>
#include <LibWebView/ReportSubmission.h>

#include <QWidget>

class QCheckBox;
class QFormLayout;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace Ladybird {

class CrashReportReviewWidget final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    // The exit button leaves the review, for example by reloading the crashed page.
    CrashReportReviewWidget(QString exit_text, QWidget* parent, WebView::CrashReportStore& = WebView::CrashReportStore::the());

    ErrorOr<void> open_report(Optional<ByteString> const& name = {}, Optional<String> const& website = {});

    void show_progress(WebView::ReportSubmission::Stage);
    void show_retry(String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds);
    void show_sent();
    void show_declined();
    void show_failure(WebView::ReportSubmission::Failure, String const& reason);

    Function<void()> on_exit;
    // The report was sent, declined or cannot be sent, so the screen around the review no longer needs to ask.
    Function<void()> on_answered;
    Function<void(bool)> on_report_text_visibility_change;

protected:
    virtual bool event(QEvent*) override;
    virtual bool focusNextPrevChild(bool next) override;

private:
    void send();
    void update_chrome_style();
    void show_page(QWidget&);
    void show_report_text();
    // Shows the given actions of the status page, with the primary one as the default and focused action.
    void show_status(QString const& title, QString const& message, Vector<QPushButton*> const& actions = {},
        QPushButton* primary = nullptr);
    void show_outcome(QString const& title, QString const& message);

    WebView::CrashReportReview m_review;

    String m_description;
    Optional<String> m_url;

    QWidget* m_review_page { nullptr };
    QWidget* m_status_page { nullptr };
    QWidget* m_report_text_page { nullptr };

    QPlainTextEdit* m_description_edit { nullptr };
    QWidget* m_website_option { nullptr };
    QCheckBox* m_include_website { nullptr };
    QLineEdit* m_website { nullptr };
    QFrame* m_details { nullptr };
    QFormLayout* m_fields { nullptr };
    QPushButton* m_show_report_button { nullptr };
    QPlainTextEdit* m_report_text { nullptr };
    QLabel* m_validation { nullptr };

    QLabel* m_status_title { nullptr };
    QProgressBar* m_progress { nullptr };
    QLabel* m_status_message { nullptr };
    QPushButton* m_retry_button { nullptr };
    QPushButton* m_cancel_button { nullptr };
    QPushButton* m_next_button { nullptr };
    QPushButton* m_exit_button { nullptr };

    bool m_is_updating_chrome_style { false };
};

}
