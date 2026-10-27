/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/ReportReview.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WebCompatReportReview.h>

#include <QAbstractButton>
#include <QBuffer>
#include <QCheckBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace Ladybird {

using namespace ReportReview;

static constexpr QSize SCREENSHOT_PREVIEW_SIZE { 150, 184 };
static constexpr qreal SCREENSHOT_PREVIEW_RADIUS = 8;
static constexpr qreal SCREENSHOT_BADGE_SIZE = 44;
static constexpr qreal SCREENSHOT_SIZE_MARGIN = 0.9;
static constexpr int MAXIMUM_SCREENSHOT_ENCODINGS = 4;

// The screenshot as a card that opens it in full, and says so while it is hovered or focused.
class ScreenshotThumbnail final : public QAbstractButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ScreenshotThumbnail(QWidget* parent)
        : QAbstractButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setToolTip(tr("View the screenshot in full"));
    }

    void set_screenshot(QImage const& screenshot)
    {
        if (screenshot.isNull()) {
            m_pixmap = {};
            return;
        }
        auto size = (QSizeF(screenshot.size()) / devicePixelRatioF()).toSize();
        if (size.width() > SCREENSHOT_PREVIEW_SIZE.width() || size.height() > SCREENSHOT_PREVIEW_SIZE.height())
            size = size.scaled(SCREENSHOT_PREVIEW_SIZE, Qt::KeepAspectRatio);
        m_pixmap = QPixmap::fromImage(screenshot.scaled(size * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_pixmap.setDevicePixelRatio(devicePixelRatioF());
        setFixedSize(size);
        update();
    }

protected:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF bounds = rect();

        QPainterPath corners;
        corners.addRoundedRect(bounds, SCREENSHOT_PREVIEW_RADIUS, SCREENSHOT_PREVIEW_RADIUS);
        painter.setClipPath(corners);
        painter.drawPixmap(0, 0, m_pixmap);

        // A dark badge keeps the magnifying glass legible whatever the screenshot shows behind it.
        if (underMouse() || hasFocus()) {
            painter.fillRect(bounds, QColor(0, 0, 0, 40));

            QRectF badge { 0, 0, SCREENSHOT_BADGE_SIZE, SCREENSHOT_BADGE_SIZE };
            badge.moveCenter(bounds.center());
            painter.setPen(QPen(QColor(255, 255, 255, 72), 1));
            painter.setBrush(QColor(0, 0, 0, 160));
            painter.drawEllipse(badge);

            auto magnifier = create_chrome_icon_pixmap(ChromeIcon::Search, Qt::white, devicePixelRatioF());
            auto magnifier_size = QSizeF(magnifier.size()) / magnifier.devicePixelRatio();
            painter.drawPixmap(badge.center() - QPointF(magnifier_size.width() / 2, magnifier_size.height() / 2), magnifier);
        }
        painter.setClipping(false);

        auto border = palette().color(QPalette::WindowText);
        border.setAlpha(40);
        auto pen = hasFocus() ? QPen(palette().color(QPalette::Highlight), 2) : QPen(border, 1);
        auto inset = pen.widthF() / 2;
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(bounds.adjusted(inset, inset, -inset, -inset), SCREENSHOT_PREVIEW_RADIUS, SCREENSHOT_PREVIEW_RADIUS);
    }

private:
    QPixmap m_pixmap;
};

// A screenshot is shrunk until it fits within the pixels and bytes a report may carry.
Optional<ByteBuffer> WebCompatReportReview::encode_screenshot(QImage const& screenshot)
{
    if (screenshot.isNull())
        return {};

    auto image = screenshot;
    auto pixels = static_cast<qreal>(image.width()) * image.height();
    if (pixels > WebView::WebCompatReport::maximum_screenshot_pixels) {
        auto scale = AK::sqrt(WebView::WebCompatReport::maximum_screenshot_pixels / pixels);
        image = image.scaled(image.size() * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    for (int encoding = 0; encoding < MAXIMUM_SCREENSHOT_ENCODINGS && !image.isNull(); ++encoding) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG"))
            return {};
        if (static_cast<size_t>(png.size()) <= WebView::WebCompatReport::maximum_screenshot_bytes)
            return MUST(ByteBuffer::copy(png.constData(), png.size()));

        // A PNG grows about as much as its pixels do, so the next one is scaled to fit, with a little room to spare.
        auto scale = AK::sqrt(static_cast<double>(WebView::WebCompatReport::maximum_screenshot_bytes) / png.size()) * SCREENSHOT_SIZE_MARGIN;
        image = image.scaled(image.size() * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return {};
}

WebCompatReportReview::WebCompatReportReview(WebView::WebCompatReport::Page page, QImage screenshot, QWidget* parent)
    : QWidget(parent)
    , m_report(AK::move(page))
{
    setObjectName("LadybirdReportReview");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_review_page = new QWidget(this);
    auto* review_layout = new QVBoxLayout(m_review_page);
    review_layout->setContentsMargins(0, 0, 0, 0);
    review_layout->setSpacing(group_spacing);

    auto* heading = new QWidget(m_review_page);
    auto* heading_layout = create_group_layout(heading);
    heading_layout->addWidget(create_title(tr("Report a website issue"), heading));
    auto* website = create_label(qstring_from_ak_string(WebView::url_for_display(m_report.page().url)), heading);
    website->setObjectName("WebCompatReportWebsite");
    website->setForegroundRole(QPalette::PlaceholderText);
    heading_layout->addWidget(website);
    review_layout->addWidget(heading);

    auto* description = new QWidget(m_review_page);
    auto* description_layout = create_group_layout(description);
    description_layout->addWidget(create_heading(tr("What’s wrong with this site?"), 1.0, description));
    m_description_edit = new QPlainTextEdit(description);
    m_description_edit->setObjectName("WebCompatReportDescription");
    m_description_edit->setPlaceholderText(tr("What did you expect to see or do, and what happened instead?"));
    m_description_edit->setTabChangesFocus(true);
    m_description_edit->setFixedHeight(76);
    description_layout->addWidget(m_description_edit);
    review_layout->addWidget(description);

    auto* screenshot_group = new QWidget(m_review_page);
    auto* screenshot_layout = create_group_layout(screenshot_group);
    screenshot_layout->addWidget(create_heading(tr("Screenshot"), 1.0, screenshot_group));
    // The screenshot sits beside its size and what can be done with it.
    auto* screenshot_row = new QHBoxLayout;
    screenshot_row->setSpacing(16);
    m_screenshot_preview = new ScreenshotThumbnail(screenshot_group);
    m_screenshot_preview->setObjectName("WebCompatReportScreenshot");
    QObject::connect(m_screenshot_preview, &QAbstractButton::clicked, this, [this] {
        if (on_view_screenshot)
            on_view_screenshot(m_screenshot);
    });
    screenshot_row->addWidget(m_screenshot_preview, 0, Qt::AlignTop);
    auto* screenshot_side = new QVBoxLayout;
    screenshot_side->setSpacing(8);
    m_selected_area_label = create_label(tr("Selected area"), screenshot_group);
    m_selected_area_label->setForegroundRole(QPalette::PlaceholderText);
    screenshot_side->addWidget(m_selected_area_label);
    m_no_screenshot_note = create_label(tr("The report will not include a screenshot."), screenshot_group);
    m_no_screenshot_note->setObjectName("WebCompatReportNoScreenshotNote");
    m_no_screenshot_note->setForegroundRole(QPalette::PlaceholderText);
    screenshot_side->addWidget(m_no_screenshot_note);
    m_select_screenshot_button = new QPushButton(screenshot_group);
    m_select_screenshot_button->setObjectName("WebCompatReportSelectScreenshotButton");
    m_select_screenshot_button->setAutoDefault(false);
    m_remove_screenshot_button = new QPushButton(tr("Remove"), screenshot_group);
    m_remove_screenshot_button->setObjectName("WebCompatReportRemoveScreenshotButton");
    m_remove_screenshot_button->setAutoDefault(false);
    auto* screenshot_buttons = new QHBoxLayout;
    screenshot_buttons->setSpacing(button_spacing);
    screenshot_buttons->addWidget(m_select_screenshot_button);
    screenshot_buttons->addWidget(m_remove_screenshot_button);
    screenshot_buttons->addStretch();
    screenshot_side->addLayout(screenshot_buttons);
    screenshot_side->addStretch();
    screenshot_row->addLayout(screenshot_side, 1);
    screenshot_layout->addLayout(screenshot_row);
    review_layout->addWidget(screenshot_group);

    auto* page_dumps = new QWidget(m_review_page);
    auto* page_dumps_layout = create_group_layout(page_dumps);
    m_include_page_dumps = new QCheckBox(tr("Include layout and painting information"), page_dumps);
    m_include_page_dumps->setObjectName("WebCompatReportIncludePageDumps");
    m_include_page_dumps->setEnabled(false);
    page_dumps_layout->addWidget(m_include_page_dumps);
    auto* page_dumps_note = create_label(tr("Helps us fix rendering bugs. It can contain text from the page."), page_dumps);
    page_dumps_note->setForegroundRole(QPalette::PlaceholderText);
    page_dumps_layout->addWidget(page_dumps_note);
    review_layout->addWidget(page_dumps);

    m_validation = create_label({}, m_review_page);
    m_validation->setObjectName("WebCompatReportValidation");
    m_validation->hide();
    review_layout->addWidget(m_validation);

    // The footer says what else the report carries, with a way to see all of it, beside the buttons that leave or send it.
    // It stays at the bottom of the tab.
    review_layout->addStretch(1);
    auto* footer_line = new QFrame(m_review_page);
    footer_line->setFrameShape(QFrame::HLine);
    footer_line->setObjectName("WebCompatReportFooterLine");
    review_layout->addWidget(footer_line);
    auto* contents_summary = create_label(tr("Also sends the address and your browser setup."), m_review_page);
    contents_summary->setForegroundRole(QPalette::PlaceholderText);
    contents_summary->setWordWrap(false);
    m_show_report_button = new QPushButton(tr("See everything"), m_review_page);
    m_show_report_button->setObjectName("WebCompatReportShowReportButton");
    m_show_report_button->setAutoDefault(false);
    m_show_report_button->setFlat(true);
    m_show_report_button->setCursor(Qt::PointingHandCursor);
    QObject::connect(m_show_report_button, &QPushButton::clicked, this, [this] { show_report_text(); });
    // The footer is set smaller than the review above it.
    for (QWidget* widget : { static_cast<QWidget*>(contents_summary), static_cast<QWidget*>(m_show_report_button) }) {
        auto font = contents_summary->font();
        font.setPixelSize(12);
        widget->setFont(font);
    }

    auto* send_button = new QPushButton(tr("Send report"), m_review_page);
    send_button->setObjectName("WebCompatReportSendButton");
    send_button->setDefault(true);
    auto* review_close_button = new QPushButton(tr("Cancel"), m_review_page);
    review_close_button->setObjectName("WebCompatReportReviewCloseButton");
    review_close_button->setAutoDefault(false);
    auto* review_buttons = new QHBoxLayout;
    review_buttons->setSpacing(button_spacing);
    // NB: The link continues the sentence.
    auto* contents = new QHBoxLayout;
    contents->setSpacing(4);
    contents->addWidget(contents_summary);
    contents->addWidget(m_show_report_button);
    contents->addStretch(1);
    review_buttons->addLayout(contents, 1);
    review_buttons->addWidget(review_close_button);
    review_buttons->addWidget(send_button);
    review_layout->addLayout(review_buttons);
    layout->addWidget(m_review_page);

    // The outcome of sending the report stands in the middle of the tab.
    m_status_page = new QWidget(this);
    auto* status_layout = new QVBoxLayout(m_status_page);
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(group_spacing);
    status_layout->addStretch();
    auto* outcome = new QWidget(m_status_page);
    auto* outcome_layout = create_group_layout(outcome);
    m_status_icon = new QLabel(outcome);
    m_status_icon->setAlignment(Qt::AlignHCenter);
    m_status_icon->hide();
    outcome_layout->addWidget(m_status_icon);
    m_status_title = create_title({}, outcome);
    m_status_title->setObjectName("WebCompatReportStatusTitle");
    m_status_title->setAlignment(Qt::AlignHCenter);
    outcome_layout->addWidget(m_status_title);
    m_progress = new QProgressBar(outcome);
    m_progress->setObjectName("WebCompatReportProgress");
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    outcome_layout->addWidget(m_progress);
    m_status_message = create_label({}, outcome);
    m_status_message->setObjectName("WebCompatReportStatusMessage");
    m_status_message->setForegroundRole(QPalette::PlaceholderText);
    m_status_message->setAlignment(Qt::AlignHCenter);
    outcome_layout->addWidget(m_status_message);
    status_layout->addWidget(outcome);

    m_retry_button = new QPushButton(tr("Try sending again"), m_status_page);
    m_retry_button->setObjectName("WebCompatReportRetryButton");
    m_cancel_button = new QPushButton(tr("Cancel"), m_status_page);
    m_cancel_button->setObjectName("WebCompatReportCancelButton");
    m_back_to_site_button = new QPushButton(tr("Back to the site"), m_status_page);
    m_back_to_site_button->setObjectName("WebCompatReportBackToSiteButton");
    m_close_button = new QPushButton(tr("Close tab"), m_status_page);
    m_close_button->setObjectName("WebCompatReportCloseButton");
    auto* status_buttons = new QHBoxLayout;
    status_buttons->setSpacing(button_spacing);
    status_buttons->addStretch();
    status_buttons->addWidget(m_retry_button);
    status_buttons->addWidget(m_cancel_button);
    status_buttons->addWidget(m_back_to_site_button);
    status_buttons->addWidget(m_close_button);
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

    QObject::connect(m_select_screenshot_button, &QPushButton::clicked, this, [this] {
        if (on_select_screenshot)
            on_select_screenshot();
    });
    QObject::connect(m_remove_screenshot_button, &QPushButton::clicked, this, [this] {
        set_screenshot({});
        m_select_screenshot_button->setFocus(Qt::OtherFocusReason);
    });
    QObject::connect(send_button, &QPushButton::clicked, this, [this] { send(); });
    QObject::connect(review_close_button, &QPushButton::clicked, this, [this] { exit(); });
    QObject::connect(m_retry_button, &QPushButton::clicked, this, [this] {
        show_progress(WebView::ReportSubmission::Stage::Preparing);
        m_report.retry();
    });
    QObject::connect(m_cancel_button, &QPushButton::clicked, this, [this] {
        m_report.cancel_sending();
        show_page(*m_review_page);
        m_description_edit->setFocus(Qt::OtherFocusReason);
    });
    QObject::connect(m_back_to_site_button, &QPushButton::clicked, this, [this] { exit(); });
    QObject::connect(m_close_button, &QPushButton::clicked, this, [this] {
        if (on_close_tab)
            on_close_tab();
        else
            exit();
    });

    m_report.on_progress = [this](auto stage) { show_progress(stage); };
    m_report.on_retry = [this](String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds) {
        show_retry(reason, retry_number, maximum_retries, delay_seconds);
    };
    m_report.on_sent = [this] { show_sent(); };
    m_report.on_failed = [this](auto failure, String const& reason) { show_failure(failure, reason); };

    set_screenshot(AK::move(screenshot));
    update_chrome_style();
    setFocusProxy(m_description_edit);
}

void WebCompatReportReview::set_screenshot(QImage screenshot)
{
    m_screenshot = AK::move(screenshot);

    auto has_screenshot = !m_screenshot.isNull();
    m_screenshot_preview->set_screenshot(m_screenshot);
    m_screenshot_preview->setVisible(has_screenshot);
    m_remove_screenshot_button->setVisible(has_screenshot);
    m_select_screenshot_button->setText(has_screenshot ? tr("Retake") : tr("Take a screenshot"));
    m_no_screenshot_note->setVisible(!has_screenshot);
    m_selected_area_label->setVisible(has_screenshot);
}

void WebCompatReportReview::set_page_dumps(String page_dumps)
{
    m_report.set_page_dumps(AK::move(page_dumps));
    m_include_page_dumps->setEnabled(true);
}

WebView::WebCompatReport::IncludePageDumps WebCompatReportReview::include_page_dumps() const
{
    return m_include_page_dumps->isChecked() ? WebView::WebCompatReport::IncludePageDumps::Yes : WebView::WebCompatReport::IncludePageDumps::No;
}

void WebCompatReportReview::send()
{
    auto description = ak_string_from_qstring(m_description_edit->toPlainText());
    if (auto error = WebView::WebCompatReport::validate(description); error.has_value()) {
        m_validation->setText(qstring_from_ak_string(*error));
        m_validation->show();
        m_description_edit->setFocus(Qt::OtherFocusReason);
        return;
    }

    Optional<ByteBuffer> screenshot;
    if (!m_screenshot.isNull()) {
        screenshot = encode_screenshot(m_screenshot);
        if (!screenshot.has_value()) {
            m_validation->setText(tr("This screenshot could not be included. Remove it or retake it."));
            m_validation->show();
            return;
        }
    }

    m_validation->hide();
    show_progress(WebView::ReportSubmission::Stage::Preparing);
    if (auto error = m_report.send(description, AK::move(screenshot), include_page_dumps()); error.has_value()) {
        m_validation->setText(qstring_from_ak_string(*error));
        m_validation->show();
        show_page(*m_review_page);
    }
}

void WebCompatReportReview::exit()
{
    if (on_exit)
        on_exit();
}

// The report is shown with the choices made so far, such as whether it includes layout and painting information.
void WebCompatReportReview::show_report_text()
{
    m_report_text->setPlainText(qstring_from_ak_string(String::from_utf8_with_replacement_character(m_report.diagnostics(include_page_dumps()))));
    show_page(*m_report_text_page);
    m_report_text->setFocus(Qt::OtherFocusReason);
}

void WebCompatReportReview::show_page(QWidget& page)
{
    m_review_page->setVisible(&page == m_review_page);
    m_status_page->setVisible(&page == m_status_page);
    m_report_text_page->setVisible(&page == m_report_text_page);
    if (on_report_text_visibility_change)
        on_report_text_visibility_change(&page == m_report_text_page);
}

void WebCompatReportReview::show_status(QString const& title, QString const& message, Vector<QPushButton*> const& actions,
    QPushButton* primary)
{
    m_status_title->setText(title);
    m_status_message->setText(message);
    m_status_icon->hide();
    show_page(*m_status_page);
    QPushButton* all_actions[] = { m_retry_button, m_cancel_button, m_back_to_site_button, m_close_button };
    show_actions(all_actions, actions, primary);
}

void WebCompatReportReview::show_progress(WebView::ReportSubmission::Stage stage)
{
    // Until the report is sent, the user can stop sending it and return to the review.
    auto is_sending = stage == WebView::ReportSubmission::Stage::Sending;
    show_status(tr("Sending report"), is_sending ? tr("Sending report…") : tr("Preparing report…"), { m_cancel_button });
    m_progress->setValue(progress_percent(stage));
    m_progress->show();
}

void WebCompatReportReview::show_retry(String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds)
{
    m_status_message->setText(retry_message(reason, retry_number, maximum_retries, delay_seconds));
}

void WebCompatReportReview::show_sent()
{
    m_progress->setValue(100);
    show_status(tr("Report sent"), tr("Thanks for helping make the web work in Ladybird."), { m_back_to_site_button, m_close_button }, nullptr);
    m_progress->hide();
    auto sent_color = Theme::is_dark() ? QColor(0x30b14f) : QColor(0x208a3a);
    m_status_icon->setPixmap(create_chrome_icon(ChromeIcon::CheckCircle, sent_color).pixmap(QSize(40, 40), devicePixelRatioF()));
    m_status_icon->show();
}

void WebCompatReportReview::show_failure(WebView::ReportSubmission::Failure, String const& reason)
{
    m_progress->hide();
    show_status(tr("Couldn’t send report"), qstring_from_ak_string(reason), { m_retry_button, m_cancel_button }, m_retry_button);
}

bool WebCompatReportReview::focusNextPrevChild(bool next)
{
    if (focus_next_control(*this, next))
        return true;
    return QWidget::focusNextPrevChild(next);
}

bool WebCompatReportReview::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();
    return QWidget::event(event);
}

void WebCompatReportReview::update_chrome_style()
{
    // Restyling changes the palette, which would bring us back here.
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;

    QWidget* fields[] = { m_description_edit, m_report_text };
    apply_style(*this, fields);

    m_is_updating_chrome_style = false;
}

}
