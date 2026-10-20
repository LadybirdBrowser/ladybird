/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/CrashReportReviewWidget.h>
#include <UI/Qt/ReportReview.h>
#include <UI/Qt/StringUtils.h>

#include <QCheckBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace Ladybird {

using namespace ReportReview;

CrashReportReviewWidget::CrashReportReviewWidget(QString exit_text, QWidget* parent, WebView::CrashReportStore& store)
    : QWidget(parent)
    , m_review(store)
{
    setObjectName("LadybirdReportReview");

    // Only the page on screen is laid out, so the widget is only as tall as the page it shows.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_review_page = new QWidget(this);
    auto* review_layout = new QVBoxLayout(m_review_page);
    review_layout->setContentsMargins(0, 0, 0, 0);
    review_layout->setSpacing(group_spacing);

    auto* description = new QWidget(m_review_page);
    auto* description_layout = create_group_layout(description);
    description_layout->addWidget(create_label(tr("What were you doing when it crashed? (optional)"), description));
    m_description_edit = new QPlainTextEdit(description);
    m_description_edit->setObjectName("CrashReportDescription");
    m_description_edit->setPlaceholderText(tr("A few words about what you were doing can help us reproduce it."));
    m_description_edit->setTabChangesFocus(true);
    m_description_edit->setFixedHeight(m_description_edit->fontMetrics().lineSpacing() * 5);
    description_layout->addWidget(m_description_edit);
    review_layout->addWidget(description);

    m_website_option = new QWidget(m_review_page);
    auto* website_layout = create_group_layout(m_website_option);
    m_include_website = new QCheckBox(tr("Include this website URL in the report"), m_website_option);
    m_include_website->setObjectName("CrashReportIncludeWebsite");
    m_website = new QLineEdit(m_website_option);
    m_website->setObjectName("CrashReportWebsite");
    m_website->setMaxLength(WebView::CrashReportReview::maximum_url_bytes);
    m_website->setEnabled(false);
    QObject::connect(m_include_website, &QCheckBox::toggled, m_website, &QLineEdit::setEnabled);
    // The URL lines up with the checkbox's label, under the option it belongs to.
    auto website_indent = style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, m_include_website)
        + style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, m_include_website);
    auto* website_row = new QHBoxLayout;
    website_row->addSpacing(website_indent);
    website_row->addWidget(m_website);
    website_layout->addWidget(m_include_website);
    website_layout->addLayout(website_row);
    review_layout->addWidget(m_website_option);

    auto* contents = new QWidget(m_review_page);
    auto* contents_layout = create_group_layout(contents);
    auto* contents_summary = create_label(tr("Includes technical crash details, your Ladybird version and platform."), contents);
    contents_summary->setForegroundRole(QPalette::PlaceholderText);
    contents_layout->addWidget(contents_summary);
    m_details = create_details_panel(contents);
    auto* details_layout = static_cast<QVBoxLayout*>(m_details->layout());
    m_fields = create_details_form();
    details_layout->addLayout(m_fields);
    details_layout->addSpacing(item_spacing);
    m_show_report_button = new QPushButton(tr("View full report"), m_details);
    m_show_report_button->setAutoDefault(false);
    m_show_report_button->setObjectName("CrashReportShowReportButton");
    QObject::connect(m_show_report_button, &QPushButton::clicked, this, [this] { show_report_text(); });
    details_layout->addWidget(m_show_report_button, 0, Qt::AlignLeft);
    auto* details_toggle = create_disclosure(tr("Report details"), m_details, contents);
    // The toggle comes before what it shows, although it is created after it.
    QWidget::setTabOrder(details_toggle, m_show_report_button);
    contents_layout->addWidget(details_toggle);
    contents_layout->addWidget(m_details);
    review_layout->addWidget(contents);

    m_validation = create_label({}, m_review_page);
    m_validation->setObjectName("CrashReportValidation");
    m_validation->hide();
    review_layout->addWidget(m_validation);

    auto* send_button = new QPushButton(tr("Send report"), m_review_page);
    send_button->setObjectName("CrashReportSendButton");
    send_button->setDefault(true);
    auto* decline_button = new QPushButton(tr("Don’t send"), m_review_page);
    decline_button->setObjectName("CrashReportDeclineButton");
    auto* review_buttons = new QHBoxLayout;
    review_buttons->setSpacing(button_spacing);
    review_buttons->addWidget(send_button);
    review_buttons->addWidget(decline_button);
    review_buttons->addStretch();
    auto* review_exit_button = new QPushButton(exit_text, m_review_page);
    review_exit_button->setObjectName("CrashReportReviewExitButton");
    review_buttons->addWidget(review_exit_button);
    // Keyboard focus starts on leaving, so a stray key press never sends a report.
    setFocusProxy(review_exit_button);
    review_layout->addLayout(review_buttons);
    review_layout->addStretch();
    layout->addWidget(m_review_page);

    m_status_page = new QWidget(this);
    auto* status_layout = new QVBoxLayout(m_status_page);
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(group_spacing);

    // The outcome is a section of the crash screen rather than a headline of its own.
    auto* outcome = new QWidget(m_status_page);
    auto* outcome_layout = create_group_layout(outcome);
    m_status_title = create_heading({}, 1, outcome);
    m_status_title->setObjectName("CrashReportStatusTitle");
    outcome_layout->addWidget(m_status_title);
    m_progress = new QProgressBar(outcome);
    m_progress->setObjectName("CrashReportProgress");
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    outcome_layout->addWidget(m_progress);
    m_status_message = create_label({}, outcome);
    m_status_message->setObjectName("CrashReportStatusMessage");
    m_status_message->setForegroundRole(QPalette::PlaceholderText);
    outcome_layout->addWidget(m_status_message);
    status_layout->addWidget(outcome);

    m_retry_button = new QPushButton(tr("Try sending again"), m_status_page);
    m_retry_button->setObjectName("CrashReportRetryButton");
    m_cancel_button = new QPushButton(tr("Cancel"), m_status_page);
    m_cancel_button->setObjectName("CrashReportCancelButton");
    m_next_button = new QPushButton(tr("Review the next crash report"), m_status_page);
    m_next_button->setObjectName("CrashReportNextButton");
    m_exit_button = new QPushButton(exit_text, m_status_page);
    m_exit_button->setObjectName("CrashReportExitButton");
    auto* status_buttons = new QHBoxLayout;
    status_buttons->setSpacing(button_spacing);
    status_buttons->addWidget(m_retry_button);
    status_buttons->addWidget(m_cancel_button);
    status_buttons->addWidget(m_next_button);
    status_buttons->addWidget(m_exit_button);
    status_buttons->addStretch();
    status_layout->addLayout(status_buttons);
    status_layout->addStretch();
    m_status_page->hide();
    layout->addWidget(m_status_page);

    auto report_text_page = create_report_text_page(this);
    m_report_text_page = report_text_page.page;
    m_report_text = report_text_page.text;
    QObject::connect(report_text_page.back_button, &QPushButton::clicked, this, [this] {
        show_page(*m_review_page);
        m_show_report_button->setFocus(Qt::OtherFocusReason);
    });
    layout->addWidget(m_report_text_page);

    QObject::connect(send_button, &QPushButton::clicked, this, [this] { send(); });
    QObject::connect(decline_button, &QPushButton::clicked, this, [this] { show_declined(); });
    QObject::connect(m_retry_button, &QPushButton::clicked, this, [this] {
        show_progress(WebView::ReportSubmission::Stage::Preparing);
        VERIFY(!m_review.send(m_description, m_url).has_value());
    });
    QObject::connect(m_cancel_button, &QPushButton::clicked, this, [this] {
        m_review.cancel_sending();
        show_page(*m_review_page);
        m_description_edit->setFocus(Qt::OtherFocusReason);
    });
    QObject::connect(m_next_button, &QPushButton::clicked, this, [this] {
        if (auto result = open_report(); result.is_error()) {
            warnln("Could not open the next crash report: {}", result.error());
            if (on_exit)
                on_exit();
        }
    });
    for (auto* button : { review_exit_button, m_exit_button }) {
        QObject::connect(button, &QPushButton::clicked, this, [this] {
            if (on_exit)
                on_exit();
        });
    }

    m_review.on_progress = [this](auto stage) { show_progress(stage); };
    m_review.on_retry = [this](String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds) {
        show_retry(reason, retry_number, maximum_retries, delay_seconds);
    };
    m_review.on_sent = [this] { show_sent(); };
    m_review.on_failed = [this](auto failure, String const& reason) { show_failure(failure, reason); };

    update_chrome_style();
}

bool CrashReportReviewWidget::focusNextPrevChild(bool next)
{
    if (focus_next_control(*this, next))
        return true;
    return QWidget::focusNextPrevChild(next);
}

bool CrashReportReviewWidget::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();
    return QWidget::event(event);
}

void CrashReportReviewWidget::update_chrome_style()
{
    // Restyling changes the palette, which would bring us back here.
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;

    QWidget* fields[] = { m_description_edit, m_website, m_report_text };
    apply_style(*this, fields);

    m_is_updating_chrome_style = false;
}

ErrorOr<void> CrashReportReviewWidget::open_report(Optional<ByteString> const& name, Optional<String> const& website)
{
    auto report = TRY(m_review.open(name));

    set_details(*m_fields, report.fields, m_details);

    // Without a website to offer, the option has nothing to include.
    m_website_option->setVisible(website.has_value());
    m_website->setText(website.has_value() ? qstring_from_ak_string(*website) : QString {});
    m_include_website->setChecked(false);
    m_website->setEnabled(false);

    m_description_edit->clear();
    m_validation->hide();
    show_page(*m_review_page);
    return {};
}

void CrashReportReviewWidget::send()
{
    m_description = ak_string_from_qstring(m_description_edit->toPlainText());
    m_url.clear();
    if (m_include_website->isChecked()) {
        m_website->setText(m_website->text().trimmed());
        m_url = ak_string_from_qstring(m_website->text());
    }

    Optional<StringView> url;
    if (m_url.has_value())
        url = m_url->bytes_as_string_view();
    // Checked before anything changes on screen, as the review checks the same when it sends.
    if (auto error = WebView::CrashReportReview::validate(m_description, url); error.has_value()) {
        m_validation->setText(qstring_from_ak_string(*error));
        m_validation->show();
        return;
    }

    m_validation->hide();
    show_progress(WebView::ReportSubmission::Stage::Preparing);
    VERIFY(!m_review.send(m_description, m_url).has_value());
}

void CrashReportReviewWidget::show_page(QWidget& page)
{
    m_review_page->setVisible(&page == m_review_page);
    m_status_page->setVisible(&page == m_status_page);
    m_report_text_page->setVisible(&page == m_report_text_page);
    if (on_report_text_visibility_change)
        on_report_text_visibility_change(&page == m_report_text_page);
}

void CrashReportReviewWidget::show_report_text()
{
    auto text = m_review.report_text();
    if (text.is_error()) {
        m_validation->setText(tr("The crash report could not be read."));
        m_validation->show();
        warnln("Could not read the crash report: {}", text.error());
        return;
    }

    m_report_text->setPlainText(qstring_from_ak_string(String::from_utf8_with_replacement_character(text.value().view())));
    show_page(*m_report_text_page);
    m_report_text->setFocus(Qt::OtherFocusReason);
}

void CrashReportReviewWidget::show_status(QString const& title, QString const& message,
    Vector<QPushButton*> const& actions, QPushButton* primary)
{
    m_status_title->setText(title);
    m_status_message->setText(message);
    show_page(*m_status_page);
    QPushButton* all_actions[] = { m_retry_button, m_cancel_button, m_next_button, m_exit_button };
    show_actions(all_actions, actions, primary);
}

void CrashReportReviewWidget::show_progress(WebView::ReportSubmission::Stage stage)
{
    // Until the report is sent, the user can stop sending it and return to the review.
    auto is_sending = stage == WebView::ReportSubmission::Stage::Sending;
    show_status(tr("Sending crash report"), is_sending ? tr("Sending crash report…") : tr("Preparing report…"), { m_cancel_button });
    m_progress->setValue(progress_percent(stage));
    m_progress->show();
}

void CrashReportReviewWidget::show_retry(String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds)
{
    m_status_message->setText(retry_message(reason, retry_number, maximum_retries, delay_seconds));
}

void CrashReportReviewWidget::show_sent()
{
    m_progress->setValue(100);
    show_outcome(tr("Report sent"), tr("Thank you for helping Ladybird."));
}

void CrashReportReviewWidget::show_declined()
{
    m_progress->hide();
    show_outcome(tr("Report not sent"), tr("This report stays on your device and is not sent."));
}

void CrashReportReviewWidget::show_failure(WebView::ReportSubmission::Failure failure, String const& reason)
{
    m_progress->hide();

    // Only sending can succeed on another attempt. A report that could not be prepared would fail the same way again.
    if (failure == WebView::ReportSubmission::Failure::Sending) {
        show_status(tr("Couldn’t send report"), qstring_from_ak_string(reason), { m_retry_button, m_cancel_button },
            m_retry_button);
        return;
    }
    show_outcome(tr("Couldn’t send report"), qstring_from_ak_string(reason));
}

// Once the report is answered, the review moves on to the next one waiting, or is left.
void CrashReportReviewWidget::show_outcome(QString const& title, QString const& message)
{
    if (on_answered)
        on_answered();

    if (m_review.has_next_report())
        show_status(title, message, { m_next_button, m_exit_button }, m_next_button);
    else
        show_status(title, message, { m_exit_button }, m_exit_button);
}

}
