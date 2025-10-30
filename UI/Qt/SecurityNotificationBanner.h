/*
 * Copyright (c) 2025, Robert Smith <rbsmith4@example.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Queue.h>
#include <AK/String.h>
#include <QLabel>
#include <QPushButton>
#include <QPropertyAnimation>
#include <QTimer>
#include <QWidget>

namespace Ladybird {

class SecurityNotificationBanner final : public QWidget {
    Q_OBJECT

public:
    enum class NotificationType {
        Block,      // Red background - download blocked
        Quarantine, // Orange background - download quarantined
        PolicyCreated, // Green background - policy auto-created
        RuleUpdated    // Blue background - YARA rule updated
    };

    struct Notification {
        NotificationType type;
        String message;
        String details; // e.g., domain/filename
        Optional<String> policy_id; // For linking to about:security
    };

    explicit SecurityNotificationBanner(QWidget* parent = nullptr);
    virtual ~SecurityNotificationBanner() override;

    // Queue a notification to be shown
    void show_notification(Notification const& notification);

    // Set auto-dismiss timeout (milliseconds, 0 to disable)
    void set_auto_dismiss_timeout(int milliseconds) { m_auto_dismiss_timeout = milliseconds; }

signals:
    void view_policy_clicked(QString policy_id);

private:
    virtual void paintEvent(QPaintEvent*) override;

    void display_next_notification();
    void dismiss_current_notification();
    void slide_in();
    void slide_out();

    QColor background_color_for_type(NotificationType type) const;
    QIcon icon_for_type(NotificationType type) const;
    String action_text_for_type(NotificationType type) const;

    // UI Components
    QLabel* m_icon_label { nullptr };
    QLabel* m_message_label { nullptr };
    QLabel* m_details_label { nullptr };
    QPushButton* m_view_policy_button { nullptr };
    QPushButton* m_dismiss_button { nullptr };

    // Animation
    QPropertyAnimation* m_slide_animation { nullptr };
    QTimer* m_auto_dismiss_timer { nullptr };

    // State
    Queue<Notification> m_notification_queue;
    Optional<Notification> m_current_notification;
    bool m_is_animating { false };
    int m_auto_dismiss_timeout { 5000 }; // 5 seconds default

    static constexpr int ANIMATION_DURATION_MS = 300;
    static constexpr int BANNER_HEIGHT = 80;
};

}
