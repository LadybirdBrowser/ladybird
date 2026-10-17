/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/ContentBlockingPopover.h>
#include <UI/Qt/StringUtils.h>

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScreen>
#include <QSignalBlocker>
#include <QStylePainter>
#include <QVBoxLayout>

namespace Ladybird {

class ContentBlockingSwitch final : public QCheckBox {
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit ContentBlockingSwitch(QWidget* parent)
        : QCheckBox("Block ads on this site", parent)
    {
        setAttribute(Qt::WA_MacShowFocusRect, false);
        setFocusPolicy(Qt::StrongFocus);
    }

    virtual QSize sizeHint() const override
    {
        return { fontMetrics().horizontalAdvance(text()) + 56, qMax(28, fontMetrics().height() + 8) };
    }

private:
    virtual bool hitButton(QPoint const& position) const override { return rect().contains(position); }

    virtual void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto track = QStyle::visualRect(layoutDirection(), rect(), QRect(width() - 43, (height() - 22) / 2, 40, 22));
        // Match the switch colors in Base/res/ladybird/ladybird.css, including disabled states.
        auto dark = ChromeStyle::is_dark(palette());
        auto track_color = isChecked() ? ChromeStyle::chrome_accent() : QColor(dark ? 0x6b6b6b : 0x8f8f8f);
        auto thumb_color = QColor(Qt::white);
        if (!isEnabled()) {
            track_color = QColor(dark ? 0x353535 : 0xe0e0e0);
            thumb_color = QColor(dark ? 0x7a7a7a : 0xfafafa);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(track_color);
        painter.drawRoundedRect(track, 11, 11);

        auto thumb_offset = isChecked() != (layoutDirection() == Qt::RightToLeft) ? 21 : 3;
        painter.setBrush(thumb_color);
        painter.drawEllipse(QRect(track.x() + thumb_offset, track.y() + 3, 16, 16));

        auto text_rect = QStyle::visualRect(layoutDirection(), rect(), QRect(0, 0, width() - 56, height()));
        painter.drawItemText(text_rect, Qt::AlignLeading | Qt::AlignVCenter, palette(), isEnabled(), text(), QPalette::WindowText);

        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(ChromeStyle::chrome_accent(), 2));
            painter.drawRoundedRect(QRectF(track).adjusted(-2, -2, 2, 2), 13, 13);
        }
    }
};

ContentBlockingPopover::ContentBlockingPopover(QWidget* parent)
    : Popover(parent)
{
    setAttribute(Qt::WA_NoMouseReplay);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName("Ad blocking controls");
    card().setObjectName("LadybirdContentBlockingPopover");

    auto* card_layout = new QVBoxLayout(&card());
    card_layout->setContentsMargins(0, 0, 0, 0);
    card_layout->setSpacing(0);

    auto* layout = new QVBoxLayout;
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);
    card_layout->addLayout(layout);

    auto* heading = new QHBoxLayout;

    m_favicon = new QLabel(&card());
    m_favicon->setFixedSize(24, 24);
    m_favicon->setAlignment(Qt::AlignCenter);
    heading->addWidget(m_favicon, 0, Qt::AlignVCenter);

    m_site = new QLabel(&card());
    m_site->setObjectName("LadybirdContentBlockingSite");
    m_site->setTextFormat(Qt::PlainText);
    m_site->setAlignment(Qt::AlignLeading | Qt::AlignVCenter);
    m_site->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    heading->addWidget(m_site, 1);
    layout->addLayout(heading);

    m_switch = new ContentBlockingSwitch(&card());
    m_switch->setObjectName("ContentBlockingSiteSwitch");
    m_switch->setAccessibleName("Block ads on this site");
    m_switch->setFocusPolicy(Qt::StrongFocus);
    QObject::connect(m_switch, &QCheckBox::toggled, this, [this](bool enabled) {
        if (on_toggle)
            on_toggle(enabled);
    });
    layout->addWidget(m_switch);

    m_count = new QLabel(&card());
    m_count->setObjectName("LadybirdContentBlockingCount");
    m_count->setTextFormat(Qt::PlainText);
    m_count->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_count);

    m_error = new QLabel(&card());
    m_error->setTextFormat(Qt::PlainText);
    m_error->hide();
    layout->addWidget(m_error);

    m_note = new QLabel(&card());
    m_note->setObjectName("LadybirdContentBlockingNote");
    m_note->setTextFormat(Qt::PlainText);
    m_note->setAlignment(Qt::AlignCenter);
    card_layout->addWidget(m_note);
}

void ContentBlockingPopover::set_favicon(QIcon const& favicon)
{
    m_favicon->setPixmap(favicon.pixmap(m_favicon->size(), devicePixelRatioF()));
}

void ContentBlockingPopover::set_snapshot(WebView::ContentBlockingSnapshot const& snapshot)
{
    auto site = snapshot.site.has_value() ? qstring_from_ak_string(*snapshot.site) : QString("This page");
    m_site->setToolTip(site);
    m_site->setAccessibleName(site);

    QSignalBlocker blocker(m_switch);
    m_switch->setChecked(snapshot.enabled());
    m_switch->setEnabled(snapshot.can_toggle());
    m_switch->setToolTip(snapshot.state == WebView::ContentBlockingState::Disabled ? "Ad blocking is disabled in Settings." : QString {});

    m_count->setText(qformatted("{} request{} blocked on this page", snapshot.blocked_request_count, snapshot.blocked_request_count == 1 ? ""sv : "s"sv));
    m_count->setVisible(snapshot.blocked_request_count > 0);

    m_note->setText("If this site appears broken, try disabling ad block.");
    m_error->hide();

    update_size();
}

void ContentBlockingPopover::update_chrome_style(QPalette const& palette)
{
    auto active_palette = palette;
    active_palette.setCurrentColorGroup(QPalette::Active);
    setPalette(active_palette);
    setStyleSheet(ChromeStyle::content_blocking_popover_style_sheet(active_palette));
    update_size();
}

void ContentBlockingPopover::show_error(QString const& message)
{
    m_error->setText(message);
    m_error->show();
    update_size();
}

void ContentBlockingPopover::focus_switch()
{
    if (m_switch->isEnabled())
        m_switch->setFocus(Qt::PopupFocusReason);
    else
        setFocus(Qt::PopupFocusReason);
}

void ContentBlockingPopover::mousePressEvent(QMouseEvent* event)
{
    // The transparent shadow margin can overlap the toolbar button. Treat it as outside the card.
    if (!card().geometry().contains(event->position().toPoint())) {
        close();
        event->accept();
        return;
    }

    Popover::mousePressEvent(event);
}

void ContentBlockingPopover::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
        if (on_keyboard_dismiss)
            on_keyboard_dismiss();
        event->accept();
        return;
    }

    Popover::keyPressEvent(event);
}

void ContentBlockingPopover::update_size()
{
    auto count_font = m_count->font();
    count_font.setPixelSize(16);
    m_count->setFont(count_font);
    m_count->ensurePolished();

    auto available_width = qMax(160, screen()->availableGeometry().width() - 48);
    auto count_width = m_count->fontMetrics().horizontalAdvance(m_count->text());
    auto count_padding = m_count->contentsMargins().left() + m_count->contentsMargins().right();
    auto available_count_width = available_width - 34 - count_padding;
    if (count_width > available_count_width) {
        // Keep the exact total on one line even on a narrow screen.
        count_font.setPixelSize(qMax(10, 16 * available_count_width / (count_width + 1)));
        m_count->setFont(count_font);
    }

    auto preferred_width = 340;
    for (auto* label : { m_count, m_error }) {
        if (label->isHidden())
            continue;
        label->ensurePolished();
        preferred_width = qMax(preferred_width, label->sizeHint().width() + 34);
    }
    m_note->ensurePolished();
    preferred_width = qMax(preferred_width, m_note->sizeHint().width() + 2);
    card().setFixedWidth(qMin(available_width, preferred_width));

    m_site->setText(m_site->fontMetrics().elidedText(m_site->toolTip(), Qt::ElideRight, card().width() - 80));

    // Refresh the minimum sizes before resizing after rows are hidden or shown.
    card().layout()->activate();
    layout()->activate();
    adjustSize();
}

}
