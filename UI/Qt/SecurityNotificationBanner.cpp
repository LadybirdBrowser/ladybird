/*
 * Copyright (c) 2025, Robert Smith <rbsmith4@example.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/Icon.h>
#include <UI/Qt/SecurityNotificationBanner.h>
#include <UI/Qt/StringUtils.h>

#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>

namespace Ladybird {

SecurityNotificationBanner::SecurityNotificationBanner(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(BANNER_HEIGHT);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setVisible(false);

    // Create layout
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(15, 10, 15, 10);
    layout->setSpacing(10);

    // Icon label
    m_icon_label = new QLabel(this);
    m_icon_label->setFixedSize(32, 32);
    m_icon_label->setScaledContents(true);

    // Text layout (vertical)
    auto* text_layout = new QVBoxLayout();
    text_layout->setSpacing(2);

    m_message_label = new QLabel(this);
    m_message_label->setStyleSheet("font-weight: bold; color: white;");
    m_message_label->setWordWrap(false);

    m_details_label = new QLabel(this);
    m_details_label->setStyleSheet("color: white; font-size: 11px;");
    m_details_label->setWordWrap(false);

    text_layout->addWidget(m_message_label);
    text_layout->addWidget(m_details_label);

    // Action buttons
    m_view_policy_button = new QPushButton("View Policy", this);
    m_view_policy_button->setStyleSheet(
        "QPushButton {"
        "  background-color: rgba(255, 255, 255, 0.9);"
        "  border: none;"
        "  border-radius: 3px;"
        "  padding: 5px 15px;"
        "  color: #333;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: rgba(255, 255, 255, 1.0);"
        "}"
    );
    m_view_policy_button->setFixedHeight(30);

    m_dismiss_button = new QPushButton("Dismiss", this);
    m_dismiss_button->setStyleSheet(
        "QPushButton {"
        "  background-color: rgba(255, 255, 255, 0.7);"
        "  border: none;"
        "  border-radius: 3px;"
        "  padding: 5px 15px;"
        "  color: #333;"
        "}"
        "QPushButton:hover {"
        "  background-color: rgba(255, 255, 255, 0.9);"
        "}"
    );
    m_dismiss_button->setFixedHeight(30);

    // Assemble layout
    layout->addWidget(m_icon_label);
    layout->addLayout(text_layout, 1);
    layout->addWidget(m_view_policy_button);
    layout->addWidget(m_dismiss_button);

    // Connect signals
    QObject::connect(m_view_policy_button, &QPushButton::clicked, this, [this] {
        if (m_current_notification.has_value() && m_current_notification->policy_id.has_value()) {
            emit view_policy_clicked(qstring_from_ak_string(m_current_notification->policy_id.value()));
        }
        dismiss_current_notification();
    });

    QObject::connect(m_dismiss_button, &QPushButton::clicked, this, [this] {
        dismiss_current_notification();
    });

    // Setup animation
    m_slide_animation = new QPropertyAnimation(this, "pos");
    m_slide_animation->setDuration(ANIMATION_DURATION_MS);
    m_slide_animation->setEasingCurve(QEasingCurve::OutCubic);

    QObject::connect(m_slide_animation, &QPropertyAnimation::finished, this, [this] {
        m_is_animating = false;

        // If sliding out, hide and show next notification
        if (!isVisible() || pos().y() < 0) {
            setVisible(false);
            m_current_notification.clear();
            display_next_notification();
        } else {
            // Just finished sliding in, start auto-dismiss timer
            if (m_auto_dismiss_timeout > 0) {
                m_auto_dismiss_timer->start(m_auto_dismiss_timeout);
            }
        }
    });

    // Setup auto-dismiss timer
    m_auto_dismiss_timer = new QTimer(this);
    m_auto_dismiss_timer->setSingleShot(true);
    QObject::connect(m_auto_dismiss_timer, &QTimer::timeout, this, [this] {
        dismiss_current_notification();
    });
}

SecurityNotificationBanner::~SecurityNotificationBanner() = default;

void SecurityNotificationBanner::show_notification(Notification const& notification)
{
    m_notification_queue.enqueue(notification);

    // If not currently showing a notification, display immediately
    if (!m_current_notification.has_value() && !m_is_animating) {
        display_next_notification();
    }
}

void SecurityNotificationBanner::display_next_notification()
{
    if (m_notification_queue.is_empty() || m_is_animating) {
        return;
    }

    m_current_notification = m_notification_queue.dequeue();
    auto const& notification = m_current_notification.value();

    // Update UI content
    m_message_label->setText(qstring_from_ak_string(notification.message));
    m_details_label->setText(qstring_from_ak_string(notification.details));

    // Set icon
    m_icon_label->setPixmap(icon_for_type(notification.type).pixmap(32, 32));

    // Show/hide view policy button
    m_view_policy_button->setVisible(notification.policy_id.has_value());

    // Update background color
    auto bg_color = background_color_for_type(notification.type);
    setStyleSheet(QString("QWidget { background-color: %1; border-radius: 5px; }")
        .arg(bg_color.name()));

    // Slide in from top
    slide_in();
}

void SecurityNotificationBanner::dismiss_current_notification()
{
    if (!m_current_notification.has_value()) {
        return;
    }

    // Stop auto-dismiss timer
    m_auto_dismiss_timer->stop();

    // Slide out
    slide_out();
}

void SecurityNotificationBanner::slide_in()
{
    if (!parentWidget()) {
        return;
    }

    m_is_animating = true;
    setVisible(true);

    // Start position: above the parent widget (hidden)
    QPoint start_pos(0, -BANNER_HEIGHT);

    // End position: top of parent widget (visible)
    QPoint end_pos(0, 0);

    move(start_pos);
    m_slide_animation->setStartValue(start_pos);
    m_slide_animation->setEndValue(end_pos);
    m_slide_animation->start();
}

void SecurityNotificationBanner::slide_out()
{
    m_is_animating = true;

    QPoint start_pos = pos();
    QPoint end_pos(0, -BANNER_HEIGHT);

    m_slide_animation->setStartValue(start_pos);
    m_slide_animation->setEndValue(end_pos);
    m_slide_animation->start();
}

QColor SecurityNotificationBanner::background_color_for_type(NotificationType type) const
{
    switch (type) {
    case NotificationType::Block:
        return QColor(211, 47, 47); // Red
    case NotificationType::Quarantine:
        return QColor(245, 124, 0); // Orange
    case NotificationType::PolicyCreated:
        return QColor(56, 142, 60); // Green
    case NotificationType::RuleUpdated:
        return QColor(25, 118, 210); // Blue
    default:
        return QColor(117, 117, 117); // Gray fallback
    }
}

QIcon SecurityNotificationBanner::icon_for_type(NotificationType type) const
{
    switch (type) {
    case NotificationType::Block:
        return load_icon_from_uri("resource://icons/16x16/close.png"sv);
    case NotificationType::Quarantine:
        return load_icon_from_uri("resource://icons/16x16/warning.png"sv);
    case NotificationType::PolicyCreated:
        return load_icon_from_uri("resource://icons/16x16/checkmark.png"sv);
    case NotificationType::RuleUpdated:
        return load_icon_from_uri("resource://icons/16x16/app-settings.png"sv);
    default:
        return load_icon_from_uri("resource://icons/16x16/app-browser.png"sv);
    }
}

String SecurityNotificationBanner::action_text_for_type(NotificationType type) const
{
    switch (type) {
    case NotificationType::Block:
        return "blocked"_string;
    case NotificationType::Quarantine:
        return "quarantined"_string;
    case NotificationType::PolicyCreated:
        return "policy created"_string;
    case NotificationType::RuleUpdated:
        return "rules updated"_string;
    default:
        return "notification"_string;
    }
}

void SecurityNotificationBanner::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
}

}
