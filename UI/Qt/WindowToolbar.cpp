/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/kmalloc.h>
#include <LibWebView/Application.h>
#include <LibWebView/CrashReportOutbox.h>
#include <LibWebView/WebCompatReport.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/ApplicationActions.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/DownloadPresentation.h>
#include <UI/Qt/FeedbackPopover.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/LadybirdButton.h>
#include <UI/Qt/LocationEdit.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/Menu.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/PerformanceMonitorWidget.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/RemoteDownloads.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WebContentView.h>
#include <UI/Qt/WindowControlButton.h>
#include <UI/Qt/WindowToolbar.h>

#include <QApplication>
#include <QDynamicPropertyChangeEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWindow>

namespace Ladybird {

static Web::HTML::ActivateTab activate_tab_for_modifiers(Qt::KeyboardModifiers modifiers)
{
    return modifiers.testFlag(Qt::ShiftModifier) ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No;
}

class ToolbarButton : public QToolButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ToolbarButton(QWidget* parent)
        : QToolButton(parent)
    {
        QObject::connect(&m_icon_motion_animation, &QVariantAnimation::valueChanged, this, [this] { update(); });
        QObject::connect(&m_icon_motion_animation, &QVariantAnimation::finished, this, [this] { update(); });
    }

    Function<void(Web::HTML::ActivateTab)> on_open_in_new_tab;

    // NB: A style sheet that sets a button's minimum size resets it when the button is polished again, so the button
    //     keeps the size as its size hint as well.
    void set_size(QSize size)
    {
        m_size = size;
        setFixedSize(size);
    }

    virtual QSize sizeHint() const override { return m_size.isValid() ? m_size : QToolButton::sizeHint(); }

    bool click_opens_in_new_tab() const
    {
        return can_open_in_new_tab() && QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
    }

    // A click or a change of state moves the icon a little: back and forward nudge their way, and the downloads arrow
    // drops into place as the button appears and bobs once as a download finishes.
    enum class IconMotion : u8 {
        NudgeLeft,
        NudgeRight,
        DropIn,
        Bob,
    };

    void play_icon_motion(IconMotion motion)
    {
        if (SystemAppearance::prefers_reduced_motion())
            return;
        m_icon_motion = motion;
        // NB: A click can disable its button, as going back to the first page does with the back button. The motion
        //     plays out as the button looked when it started.
        m_icon_motion_is_hovered = underMouse();
        m_icon_motion_animation.stop();
        m_icon_motion_animation.setStartValue(0.0);
        m_icon_motion_animation.setEndValue(1.0);
        switch (motion) {
        case IconMotion::NudgeLeft:
        case IconMotion::NudgeRight:
            m_icon_motion_animation.setDuration(NUDGE_DURATION_MS);
            break;
        case IconMotion::DropIn:
            m_icon_motion_animation.setDuration(DROP_IN_DURATION_MS);
            break;
        case IconMotion::Bob:
            m_icon_motion_animation.setDuration(BOB_DURATION_MS);
            break;
        }
        m_icon_motion_animation.start();
    }

protected:
    virtual bool can_open_in_new_tab() const { return static_cast<bool>(on_open_in_new_tab); }

    QRect icon_rect() const
    {
        return QRect(QPoint((width() - iconSize().width()) / 2, (height() - iconSize().height()) / 2), iconSize());
    }

    QIcon::Mode icon_mode() const
    {
        return isEnabled() ? (underMouse() ? QIcon::Active : QIcon::Normal) : QIcon::Disabled;
    }

    virtual void paintEvent(QPaintEvent*) override
    {
        if (m_icon_motion_animation.state() == QAbstractAnimation::Running) {
            QPainter painter(this);
            if (isEnabled()) {
                ChromeStyle::paint_control_frame(painter, *this);
            } else if (m_icon_motion_is_hovered && underMouse()) {
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(Qt::NoPen);
                painter.setBrush(Theme::chrome(palette()).hover);
                painter.drawRoundedRect(QRectF(rect()), ChromeStyle::CONTROL_FRAME_RADIUS, ChromeStyle::CONTROL_FRAME_RADIUS);
                painter.setRenderHint(QPainter::Antialiasing, false);
            }
            auto mode = isEnabled() ? icon_mode() : (m_icon_motion_is_hovered && underMouse() ? QIcon::Active : QIcon::Normal);
            auto progress = m_icon_motion_animation.currentValue().toReal();
            switch (m_icon_motion) {
            case IconMotion::NudgeLeft:
                painter.translate(-NUDGE_DISTANCE * Motion::there_and_back(progress, NUDGE_PEAK_AT), 0);
                break;
            case IconMotion::NudgeRight:
                painter.translate(NUDGE_DISTANCE * Motion::there_and_back(progress, NUDGE_PEAK_AT), 0);
                break;
            case IconMotion::DropIn: {
                auto eased = Motion::enter_easing().valueForProgress(progress);
                painter.translate(0, -DROP_IN_DISTANCE * (1 - eased));
                painter.setOpacity(eased);
                break;
            }
            case IconMotion::Bob:
                painter.translate(0, BOB_DISTANCE * Motion::there_and_back(progress, BOB_PEAK_AT));
                break;
            }
            icon().paint(&painter, icon_rect(), Qt::AlignCenter, mode);
            return;
        }

        QStyleOptionToolButton option;
        initStyleOption(&option);

        QStylePainter painter(this);
        ChromeStyle::paint_control_frame(painter, *this);

        painter.drawComplexControl(QStyle::CC_ToolButton, option);
    }

    virtual void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::MiddleButton && can_open_in_new_tab())
            event->accept();
        else
            QToolButton::mousePressEvent(event);
    }

    virtual void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::MiddleButton && can_open_in_new_tab()) {
            if (rect().contains(event->position().toPoint()))
                on_open_in_new_tab(activate_tab_for_modifiers(event->modifiers()));
            event->accept();
        } else {
            QToolButton::mouseReleaseEvent(event);
        }
    }

    // QToolButton triggers its default action from here, once a click has completed.
    virtual void nextCheckState() override
    {
        if (click_opens_in_new_tab()) {
            on_open_in_new_tab(activate_tab_for_modifiers(QApplication::keyboardModifiers()));
            return;
        }
        QToolButton::nextCheckState();
    }

private:
    QSize m_size;
    static constexpr int NUDGE_DURATION_MS = 180;
    static constexpr qreal NUDGE_DISTANCE = 2;
    static constexpr qreal NUDGE_PEAK_AT = 0.4;
    static constexpr int DROP_IN_DURATION_MS = 220;
    static constexpr qreal DROP_IN_DISTANCE = 4;
    static constexpr int BOB_DURATION_MS = 260;
    static constexpr qreal BOB_DISTANCE = 2;
    static constexpr qreal BOB_PEAK_AT = 0.45;

    IconMotion m_icon_motion { IconMotion::NudgeLeft };
    bool m_icon_motion_is_hovered { false };
    QVariantAnimation m_icon_motion_animation;
};

// The reload button becomes a stop button while the page loads. So that one click does not do what the user did not aim
// at, a stop button under the pointer stays until the pointer leaves or a short delay passes, and a click on stop within
// the double-click interval of a reload click is ignored, so a double-click does not stop the reload it started. The
// reload action stays attached to the button while it shows stop, so its shortcuts still work during a load.
class ReloadButton final : public ToolbarButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ReloadButton(QWidget* parent)
        : ToolbarButton(parent)
    {
        m_icon_fade.setStartValue(0.0);
        m_icon_fade.setEndValue(1.0);
        QObject::connect(&m_icon_fade, &QVariantAnimation::valueChanged, this, [this] { update(); });

        m_double_click_timer.setSingleShot(true);
        m_double_click_timer.setInterval(QApplication::doubleClickInterval());

        m_stop_to_reload_timer.setSingleShot(true);
        m_stop_to_reload_timer.setInterval(STOP_TO_RELOAD_DELAY_MS);
        QObject::connect(&m_stop_to_reload_timer, &QTimer::timeout, this, [this] {
            change_mode(m_intended_mode, ForceChange::Yes);
        });

        QObject::connect(this, &QToolButton::pressed, this, [this] {
            if (m_visible_mode == Mode::Reload)
                m_double_click_timer.start();
        });

        // NB: A stop click that arrives after the load ended has nothing to stop, so show the reload button right away.
        QObject::connect(this, &QToolButton::clicked, this, [this] {
            if (m_visible_mode == Mode::Stop)
                change_mode(m_intended_mode, ForceChange::Yes);
        });
    }

    void set_actions(QAction& reload_action, QAction& reload_bypassing_cache_action, QAction& stop_action)
    {
        m_reload_action = &reload_action;
        m_reload_bypassing_cache_action = &reload_bypassing_cache_action;
        m_stop_action = &stop_action;
    }

    void set_loading(bool is_loading)
    {
        change_mode(is_loading ? Mode::Stop : Mode::Reload, ForceChange::No);
    }

    // Another tab's state is shown as it stands, without the motion of a change.
    void show_loading_at_once(bool is_loading)
    {
        m_icon_fade.stop();
        m_stop_to_reload_timer.stop();
        m_intended_mode = m_visible_mode = is_loading ? Mode::Stop : Mode::Reload;
        setDefaultAction(is_loading ? m_stop_action : m_reload_action);
        update();
    }

protected:
    virtual bool can_open_in_new_tab() const override
    {
        return m_visible_mode == Mode::Reload && ToolbarButton::can_open_in_new_tab();
    }

    virtual void leaveEvent(QEvent* event) override
    {
        ToolbarButton::leaveEvent(event);
        change_mode(m_intended_mode, ForceChange::Yes);
    }

    // A Shift-click on the reload button reloads bypassing the HTTP cache, and one on the stop button stops the load as
    // any other click there does.
    virtual void nextCheckState() override
    {
        if (m_visible_mode == Mode::Stop && m_double_click_timer.isActive())
            return;
        if (m_visible_mode == Mode::Reload && !click_opens_in_new_tab() && QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
            m_reload_bypassing_cache_action->trigger();
            return;
        }
        ToolbarButton::nextCheckState();
    }

private:
    static constexpr int STOP_TO_RELOAD_DELAY_MS = 1000;
    static constexpr int ARROW_TURN_DURATION_MS = 340;
    static constexpr int TURN_CROSS_FADE_DELAY_MS = 200;
    static constexpr int TURN_CROSS_FADE_DURATION_MS = 140;
    static constexpr int STOP_TURN_DELAY_MS = 180;
    static constexpr int STOP_TURN_DURATION_MS = 200;
    static constexpr int RELOAD_TO_STOP_DURATION_MS = STOP_TURN_DELAY_MS + STOP_TURN_DURATION_MS;
    static constexpr int ICON_CROSS_FADE_DURATION_MS = 160;

    enum class Mode {
        Reload,
        Stop,
    };

    enum class ForceChange {
        No,
        Yes,
    };

    void change_mode(Mode mode, ForceChange force_change)
    {
        m_intended_mode = mode;

        auto change_is_safe = mode == Mode::Stop || m_visible_mode != Mode::Stop;
        if (force_change == ForceChange::Yes || !underMouse() || change_is_safe) {
            m_stop_to_reload_timer.stop();
            if (m_visible_mode != mode && defaultAction() && !SystemAppearance::prefers_reduced_motion()) {
                m_previous_icon = defaultAction()->icon();
                // NB: The arrow turns into the stop icon only as the reload button is clicked. A load started otherwise
                //     cross-fades it, as does the end of a load.
                m_turns_icon = mode == Mode::Stop && m_double_click_timer.isActive();
                m_icon_fade.stop();
                m_icon_fade.setDuration(m_turns_icon ? RELOAD_TO_STOP_DURATION_MS : ICON_CROSS_FADE_DURATION_MS);
                m_icon_fade.start();
            }
            m_visible_mode = mode;
            setDefaultAction(mode == Mode::Stop ? m_stop_action : m_reload_action);
            return;
        }

        if (m_visible_mode == Mode::Stop && !m_stop_to_reload_timer.isActive())
            m_stop_to_reload_timer.start();
    }

    // As a click on it starts a load, the reload arrow turns half a turn, and over its second half cross-fades into the
    // stop icon, which turns into place. Otherwise, as when the load ends, the two icons only cross-fade.
    virtual void paintEvent(QPaintEvent* event) override
    {
        if (m_icon_fade.state() != QAbstractAnimation::Running) {
            ToolbarButton::paintEvent(event);
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        ChromeStyle::paint_control_frame(painter, *this);
        auto elapsed = m_icon_fade.currentValue().toReal() * m_icon_fade.duration();
        auto part = [&](qreal delay, qreal duration) { return clamp((elapsed - delay) / duration, 0.0, 1.0); };
        auto center = QRectF(icon_rect()).center();
        auto paint_turned = [&](QIcon const& icon, qreal opacity, qreal degrees) {
            painter.save();
            painter.setOpacity(opacity);
            painter.translate(center);
            painter.rotate(degrees);
            painter.translate(-center);
            icon.paint(&painter, icon_rect(), Qt::AlignCenter, icon_mode());
            painter.restore();
        };
        if (!m_turns_icon) {
            auto fade = part(0, ICON_CROSS_FADE_DURATION_MS);
            paint_turned(m_previous_icon, 1 - fade, 0);
            paint_turned(icon(), fade, 0);
            return;
        }
        static auto const easing = Motion::enter_easing();
        auto fade = part(TURN_CROSS_FADE_DELAY_MS, TURN_CROSS_FADE_DURATION_MS);
        paint_turned(m_previous_icon, 1 - fade, 180 * easing.valueForProgress(part(0, ARROW_TURN_DURATION_MS)));
        paint_turned(icon(), fade, -90 * (1 - easing.valueForProgress(part(STOP_TURN_DELAY_MS, STOP_TURN_DURATION_MS))));
    }

    QAction* m_reload_action { nullptr };
    QAction* m_reload_bypassing_cache_action { nullptr };
    QAction* m_stop_action { nullptr };
    QIcon m_previous_icon;
    QVariantAnimation m_icon_fade;
    bool m_turns_icon { false };
    Mode m_visible_mode { Mode::Reload };
    Mode m_intended_mode { Mode::Reload };
    QTimer m_double_click_timer;
    QTimer m_stop_to_reload_timer;
};

static constexpr QSize TOOLBAR_BUTTON_SIZE { 32, 28 };

// The downloads button appears at the end of the toolbar once there are downloads, and opens the list of them. While
// they are under way, a ring around it fills as they progress, or turns while their progress is not known. A failed
// download marks it with a dot until the list is looked at.
class DownloadsButton final : public ToolbarButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit DownloadsButton(QWidget* parent)
        : ToolbarButton(parent)
    {
        setObjectName("LadybirdDownloadsButton");
        setAccessibleName("Downloads");
        // NB: The button stays checked while the list of downloads is open.
        setCheckable(true);
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setIconSize({ TOOLBAR_ICON_SIZE, TOOLBAR_ICON_SIZE });
        setFixedSize(TOOLBAR_BUTTON_SIZE);
        setAttribute(Qt::WA_LayoutUsesWidgetRect);
        hide();

        m_ring_animation.setDuration(ChromeStyle::LOADING_RING_PERIOD_MS);
        m_ring_animation.setStartValue(0.0);
        m_ring_animation.setEndValue(1.0);
        m_ring_animation.setLoopCount(-1);
        QObject::connect(&m_ring_animation, &QVariantAnimation::valueChanged, this, [this] { update(); });

        m_ring_fade.setDuration(RING_FADE_DURATION_MS);
        QObject::connect(&m_ring_fade, &QVariantAnimation::valueChanged, this, [this] { update(); });

        m_progress_animation.setDuration(PROGRESS_GLIDE_DURATION_MS);
        QObject::connect(&m_progress_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            m_shown_progress = value.toReal();
            update();
        });
    }

    void set_progress(bool downloading, Optional<double> progress)
    {
        if (m_downloading == downloading && m_progress == progress)
            return;
        // The ring fades in as downloads start, and out as they end, showing how far they had got.
        if (m_downloading != downloading) {
            m_ring_fade.stop();
            if (!SystemAppearance::prefers_reduced_motion() && isVisible()) {
                m_ring_fade.setStartValue(downloading ? 0.0 : 1.0);
                m_ring_fade.setEndValue(downloading ? 1.0 : 0.0);
                m_ring_fade.start();
            }
        }
        if (m_downloading && !downloading)
            m_progress_at_end = m_shown_progress;
        // The ring glides to each new amount rather than jumping to it.
        m_progress_animation.stop();
        if (progress.has_value() && downloading) {
            if (!m_downloading || !m_progress.has_value() || SystemAppearance::prefers_reduced_motion()) {
                m_shown_progress = *progress;
            } else {
                m_progress_animation.setStartValue(m_shown_progress);
                m_progress_animation.setEndValue(*progress);
                m_progress_animation.start();
            }
        }
        m_downloading = downloading;
        m_progress = progress;
        auto should_turn = downloading && !progress.has_value() && isVisible();
        if (should_turn && m_ring_animation.state() != QAbstractAnimation::Running)
            m_ring_animation.start();
        else if (!should_turn)
            m_ring_animation.stop();
        update();
    }

    void set_needs_attention(bool needs_attention)
    {
        if (m_needs_attention == needs_attention)
            return;
        m_needs_attention = needs_attention;
        update();
    }

private:
    virtual void paintEvent(QPaintEvent* event) override
    {
        ToolbarButton::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        auto palette = Theme::chrome_palette(*this);
        auto center = QRectF(rect()).center();
        auto ring_is_fading = m_ring_fade.state() == QAbstractAnimation::Running;
        if (m_downloading || ring_is_fading) {
            painter.save();
            if (ring_is_fading)
                painter.setOpacity(m_ring_fade.currentValue().toReal());
            auto progress = m_downloading ? (m_progress.has_value() ? Optional<double> { m_shown_progress } : Optional<double> {}) : m_progress_at_end;
            if (progress.has_value())
                ChromeStyle::paint_progress_ring(painter, palette, center, *progress);
            else
                ChromeStyle::paint_loading_ring(painter, palette, center, ChromeStyle::LoadingRingSize::ToolbarButton);
            painter.restore();
        }
        if (m_needs_attention) {
            QPointF dot_center { width() - 7.0, 6.0 };
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::chrome(palette).toolbar);
            painter.drawEllipse(dot_center, 5.5, 5.5);
            painter.setBrush(Theme::chrome(palette).warning);
            painter.drawEllipse(dot_center, 3.5, 3.5);
        }
    }

    static constexpr int RING_FADE_DURATION_MS = 200;
    static constexpr int PROGRESS_GLIDE_DURATION_MS = 300;

    bool m_downloading { false };
    Optional<double> m_progress;
    Optional<double> m_progress_at_end;
    double m_shown_progress { 0 };
    QVariantAnimation m_progress_animation;
    bool m_needs_attention { false };
    QVariantAnimation m_ring_animation;
    QVariantAnimation m_ring_fade;
};

// The feedback button ends the toolbar of builds that are still being tested, and opens the ways to send feedback. A
// dot marks it while crash reports wait to be reviewed.
class FeedbackButton final : public ToolbarButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit FeedbackButton(QWidget* parent)
        : ToolbarButton(parent)
    {
        setObjectName("LadybirdFeedbackButton");
        setToolTip("Send feedback");
        setAccessibleName("Send feedback");
        // NB: The button stays checked while the popover is open.
        setCheckable(true);
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setIconSize({ TOOLBAR_ICON_SIZE, TOOLBAR_ICON_SIZE });
        setFixedSize(TOOLBAR_BUTTON_SIZE);
        setAttribute(Qt::WA_LayoutUsesWidgetRect);

        m_dot_animation.setDuration(Motion::BASE);
        m_dot_animation.setEasingCurve(Motion::enter_easing());
        m_dot_animation.setStartValue(0.0);
        m_dot_animation.setEndValue(1.0);
        QObject::connect(&m_dot_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            m_dot_scale = value.toDouble();
            update();
        });
    }

    void set_crash_reports_waiting(bool crash_reports_waiting)
    {
        if (m_crash_reports_waiting == crash_reports_waiting)
            return;
        m_crash_reports_waiting = crash_reports_waiting;
        setAccessibleName(crash_reports_waiting ? "Send feedback, crash reports waiting" : "Send feedback");

        // A dot that is already there when a window opens was not just earned, so it does not animate.
        m_dot_animation.stop();
        m_dot_scale = 1;
        if (crash_reports_waiting && isVisible() && !SystemAppearance::prefers_reduced_motion()) {
            m_dot_scale = 0;
            m_dot_animation.start();
        }
        update();
    }

private:
    virtual void paintEvent(QPaintEvent* event) override
    {
        ToolbarButton::paintEvent(event);
        if (!m_crash_reports_waiting)
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        auto palette = Theme::chrome_palette(*this);
        QPointF dot_center { width() - 9.5, 7.5 };
        painter.setPen(Qt::NoPen);
        painter.setBrush(Theme::chrome(palette).toolbar);
        painter.drawEllipse(dot_center, 5.5 * m_dot_scale, 5.5 * m_dot_scale);
        // NB: In the icon's color, the dot would fuse with the glyph it sits on.
        painter.setBrush(ChromeStyle::crash_reports_dot_color(palette));
        painter.drawEllipse(dot_center, 3.5 * m_dot_scale, 3.5 * m_dot_scale);
    }

    bool m_crash_reports_waiting { false };
    QVariantAnimation m_dot_animation;
    qreal m_dot_scale { 1 };
};

template<typename ButtonType = ToolbarButton>
static ButtonType* create_toolbar_button(QWidget& parent, QAction& action)
{
    auto* button = new ButtonType(&parent);
    button->setDefaultAction(&action);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize({ TOOLBAR_ICON_SIZE, TOOLBAR_ICON_SIZE });
    button->setFixedSize(TOOLBAR_BUTTON_SIZE);
    // NB: The platform style pads tool buttons for its own frame, which would space them further apart than the design.
    button->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    // FIXME: In Menu.cpp, we set the initial visibility of the action before we've associated it with this QToolButton.
    //        It would be nicer if we didn't have to do this here.
    button->setVisible(action.isVisible());

    return button;
}

// The buttons in the toolbar sit close together, while the address field keeps a wider gap on either side.
static constexpr int TOOLBAR_SPACING = 2;
// The separator after the collapsed sidebar's buttons keeps a little more room on either side.
static constexpr int SIDEBAR_SLOT_SEPARATOR_MARGIN = 6;
// NB: With the toolbar's spacing, the address field starts 8 px after the reload button.
static constexpr int TOOLBAR_LOCATION_EDIT_MARGIN = 6;
// The address field's focus ring reaches 3 px past its edge. With the toolbar's spacing, this room also puts the
// downloads button 6 px after the field.
static constexpr int TOOLBAR_LOCATION_FOCUS_RING_ROOM = 4;
// In the open sidebar, the navigation buttons have a row of their own, above the address field, and the toolbar's other
// buttons sit in a footer at its bottom.
static constexpr QSize SIDEBAR_NAVIGATION_BUTTON_SIZE { 26, 28 };
static constexpr int NAVIGATION_BUTTON_COUNT = 3;
static constexpr int SIDEBAR_FIELD_TOP_MARGIN = 4;
static constexpr int SIDEBAR_FIELD_BOTTOM_MARGIN = 8;
static constexpr int SIDEBAR_FOOTER_HEIGHT = 40;
// NB: The container draws the line under the toolbar along its bottom edge, below the toolbar's full height.
static constexpr int TOOLBAR_BOTTOM_LINE_HEIGHT = 1;
static constexpr int PERFORMANCE_MONITOR_ROW_MARGIN = 2;
// The address field and its suggestions are one surface. While it lists suggestions, the field grows downward into it,
// as wide as the field in a toolbar row, but no narrower than the minimum, and this wide over the page in the sidebar. It
// stays this far from the window's edge.
static constexpr int FIELD_PANEL_WIDTH = 600;
static constexpr int FIELD_PANEL_MINIMUM_WIDTH = 100;
static constexpr int FIELD_PANEL_WINDOW_MARGIN = 8;
// A line inset this far from the surface's sides sets the suggestions apart from the field, and the list has this much
// room around it.
static constexpr int FIELD_PANEL_DIVIDER_INSET = 8;
static constexpr int FIELD_PANEL_LIST_PADDING = 6;

// The address field floats over its place in the toolbar, in a panel that is a native child of the window wherever the
// page stays native under it. So the panel stacks above the native views of the sidebar and the page, and can grow over
// the page, while the field keeps the keyboard focus. Elsewhere, the page steps aside while the panel is grown, and Qt
// paints them both.
class FieldPanel final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit FieldPanel(QWidget& window)
        : QWidget(&window)
    {
        setObjectName("LadybirdFieldPanel");
#if defined(AK_OS_MACOS) || defined(LADYBIRD_QT_USE_RHI_WIDGET)
        setAttribute(Qt::WA_DontCreateNativeAncestors);
        setAttribute(Qt::WA_NativeWindow);
#endif
    }

    bool is_expanded() const { return m_expanded; }
    void set_expanded(bool expanded)
    {
        if (m_expanded == expanded)
            return;
        m_expanded = expanded;
        // The surface casts the shadow of a popover, and the field's focus ring goes around all of it.
        if (expanded)
            m_shadow = &cast_floating_shadow(*this, ChromeStyle::LOCATION_EDIT_CORNER_RADIUS, FloatingShadowTone::FollowTheme, FloatingShadowKind::FocusedField);
        else
            delete m_shadow.data();
        round_corners();
        update();
    }

    void set_in_sidebar(bool in_sidebar)
    {
        if (m_in_sidebar == in_sidebar)
            return;
        m_in_sidebar = in_sidebar;
        update();
    }

    void set_suggestions_area(QWidget& suggestions_area) { m_suggestions_area = &suggestions_area; }

    void round_corners()
    {
#if defined(AK_OS_MACOS)
        // Asking for the native view would create the window's native window, even while the window is still being built.
        if (!internalWinId())
            return;
        if (m_expanded)
            round_appkit_view_corners(*this, ChromeStyle::LOCATION_EDIT_CORNER_RADIUS, Qt::transparent);
        else
            round_appkit_view_corners(*this, 0, Qt::transparent);
#endif
    }

private:
    virtual bool event(QEvent* event) override
    {
        if (event->type() == QEvent::WinIdChange)
            round_corners();
        return QWidget::event(event);
    }

    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        // At rest, the panel is the toolbar or the sidebar under the field.
        auto const& chrome = Theme::chrome(*this);
        if (!m_expanded) {
            painter.fillRect(rect(), m_in_sidebar ? chrome.sidebar : chrome.toolbar);
            return;
        }
#if defined(AK_OS_MACOS)
        // NB: The view rounds its corners itself (see round_corners()). Every pixel is painted, as one that isn't shows
        //     whatever was last drawn behind the native view.
        painter.fillRect(rect(), chrome.field_focused);
#else
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(chrome.field_focused);
        auto radius = static_cast<qreal>(ChromeStyle::LOCATION_EDIT_CORNER_RADIUS);
        painter.drawRoundedRect(QRectF(rect()), radius, radius);
#endif
        if (m_suggestions_area)
            painter.fillRect(QRect(FIELD_PANEL_DIVIDER_INSET, m_suggestions_area->y(), width() - (2 * FIELD_PANEL_DIVIDER_INSET), 1), Theme::panel().separator_soft);
    }

    bool m_expanded { false };
    bool m_in_sidebar { false };
    QPointer<QWidget> m_shadow;
    QPointer<QWidget> m_suggestions_area;
};

// NB: Window controls run to the window's edge. Without them, the address field ends the toolbar, and the room for its
//     focus ring takes the place of some of the toolbar's margin.
static constexpr int toolbar_right_margin(bool has_custom_window_controls)
{
    return has_custom_window_controls ? 0 : TOOLBAR_HORIZONTAL_MARGIN - TOOLBAR_LOCATION_FOCUS_RING_ROOM;
}
static constexpr int DOWNLOADS_POPOVER_WIDTH = 380;
static constexpr int DOWNLOADS_POPOVER_MAX_HEIGHT = 360;
class ElidedLabel final : public QLabel {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ElidedLabel(QString text, Qt::TextElideMode elide_mode, QWidget* parent = nullptr)
        : QLabel(text, parent)
        , m_elide_mode(elide_mode)
    {
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    virtual QSize sizeHint() const override
    {
        auto size = QLabel::sizeHint();
        size.setWidth(0);
        return size;
    }

    virtual QSize minimumSizeHint() const override
    {
        return { 0, QLabel::minimumSizeHint().height() };
    }

protected:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(rect(), alignment() | Qt::TextSingleLine, fontMetrics().elidedText(text(), m_elide_mode, width()));
    }

private:
    Qt::TextElideMode m_elide_mode { Qt::ElideRight };
};

// A small page with the file's extension on it.
class DownloadFileIcon final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit DownloadFileIcon(QWidget* parent)
        : QWidget(parent)
    {
        setFixedSize(28, 34);
    }

    void set_extension(QString extension)
    {
        m_extension = extension.toUpper().left(4);
        update();
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        auto const& panel = Theme::panel();
        painter.setBrush(panel.page);
        painter.setPen(QPen(panel.separator, 1));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);

        auto font = this->font();
        font.setPixelSize(8);
        font.setWeight(QFont::Bold);
        painter.setFont(font);
        painter.setPen(panel.muted);
        painter.drawText(rect().adjusted(0, 0, 0, -4), Qt::AlignHCenter | Qt::AlignBottom, m_extension);
    }

    QString m_extension;
};

class DownloadRow final : public QFrame {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit DownloadRow(WebView::FileDownloader::Download const& download, QWidget* parent)
        : QFrame(parent)
        , m_download_id(download.id)
    {
        setObjectName("LadybirdDownloadRow");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto* row_layout = new QHBoxLayout(this);
        row_layout->setContentsMargins(10, 8, 6, 8);
        row_layout->setSpacing(12);

        m_file_icon = new DownloadFileIcon(this);
        row_layout->addWidget(m_file_icon, 0, Qt::AlignVCenter);

        auto* details_layout = new QVBoxLayout;
        details_layout->setContentsMargins(0, 0, 0, 0);
        details_layout->setSpacing(5);

        m_file_name_label = new ElidedLabel({}, Qt::ElideMiddle, this);
        m_file_name_label->setObjectName("LadybirdDownloadFileName");
        details_layout->addWidget(m_file_name_label);

        m_progress_bar = new QProgressBar(this);
        m_progress_bar->setObjectName("LadybirdDownloadProgress");
        m_progress_bar->setRange(0, 1000);
        m_progress_bar->setTextVisible(false);
        details_layout->addWidget(m_progress_bar);

        m_status_label = new ElidedLabel({}, Qt::ElideRight, this);
        m_status_label->setObjectName("LadybirdDownloadStatus");
        details_layout->addWidget(m_status_label);

        row_layout->addLayout(details_layout, 1);

        auto make_action = [&](ChromeIcon icon, QString const& tooltip, auto callback) {
            auto* button = new ToolbarButton(this);
            button->setObjectName("LadybirdDownloadAction");
            button->setAutoRaise(true);
            button->setIcon(create_chrome_icon(icon, Theme::panel().body));
            button->setIconSize({ 16, 16 });
            button->setFixedSize(28, 28);
            button->setToolTip(tooltip);
            button->setAccessibleName(tooltip);
            button->setFocusPolicy(Qt::TabFocus);
            QObject::connect(button, &QToolButton::clicked, this, [this, callback] { callback(*this); });
            row_layout->addWidget(button, 0, Qt::AlignVCenter);
            return button;
        };

        m_pause_button = make_action(ChromeIcon::Pause, "Pause", [](DownloadRow& row) {
            if (row.m_is_paused) {
                if (row.on_resume_download)
                    row.on_resume_download(row.m_download_id);
            } else if (row.on_pause_download) {
                row.on_pause_download(row.m_download_id);
            }
        });
        m_cancel_button = make_action(ChromeIcon::Close, "Cancel", [](DownloadRow& row) {
            if (row.on_cancel_download)
                row.on_cancel_download(row.m_download_id);
        });
        m_retry_button = make_action(ChromeIcon::Reload, "Retry", [](DownloadRow& row) {
            if (row.on_resume_download)
                row.on_resume_download(row.m_download_id);
        });
#if defined(AK_OS_MACOS)
        auto show_in_folder_name = "Show in Finder";
#elif defined(AK_OS_WINDOWS)
        auto show_in_folder_name = "Show in Explorer";
#else
        auto show_in_folder_name = "Show in Files";
#endif
        m_show_in_folder_button = make_action(ChromeIcon::Folder, show_in_folder_name, [](DownloadRow& row) {
            if (row.on_show_in_folder)
                row.on_show_in_folder(row.m_download_id);
        });

        (void)update_download(download);
    }

    u64 id() const { return m_download_id; }

    bool update_download(WebView::FileDownloader::Download const& download)
    {
        VERIFY(download.id == m_download_id);

        auto basename = qstring_from_ak_string(download.destination.basename());
        m_file_name_label->setText(basename);
        m_file_name_label->setToolTip(qstring_from_ak_string(download.destination.string()));
        m_file_icon->set_extension(qstring_from_ak_string(download.destination.extension()));

        auto status_text = qstring_from_ak_string(download_status_text(download));
        m_status_label->setText(status_text);
        m_status_label->setToolTip(status_text);
        auto failed = download.status == WebView::FileDownloader::DownloadStatus::Failed;
        if (m_status_label->property("failed").toBool() != failed) {
            m_status_label->setProperty("failed", failed);
            m_status_label->style()->unpolish(m_status_label);
            m_status_label->style()->polish(m_status_label);
        }

        bool geometry_changed = false;
        auto set_visible = [&](QWidget& widget, bool visible) {
            if (widget.isHidden() == visible) {
                widget.setVisible(visible);
                geometry_changed = true;
            }
        };

        auto is_active = WebView::FileDownloader::status_is_active(download.status);
        auto progress = download.progress();
        set_visible(*m_progress_bar, is_active && progress.has_value());
        if (progress.has_value())
            m_progress_bar->setValue(static_cast<int>(*progress * 1000.0));

        m_is_paused = download.status == WebView::FileDownloader::DownloadStatus::Paused;
        m_pause_button->setIcon(create_chrome_icon(m_is_paused ? ChromeIcon::Download : ChromeIcon::Pause, Theme::chrome_palette(*this)));
        m_pause_button->setToolTip(m_is_paused ? "Resume" : "Pause");
        m_pause_button->setAccessibleName(m_pause_button->toolTip());

        set_visible(*m_pause_button, download.can_resume && is_active);
        set_visible(*m_cancel_button, is_active);
        set_visible(*m_retry_button, failed && download.can_resume);
        set_visible(*m_show_in_folder_button, download.status == WebView::FileDownloader::DownloadStatus::Completed);

        return geometry_changed;
    }

    Function<void(u64)> on_cancel_download;
    Function<void(u64)> on_pause_download;
    Function<void(u64)> on_resume_download;
    Function<void(u64)> on_show_in_folder;

private:
    u64 m_download_id { 0 };
    bool m_is_paused { false };
    DownloadFileIcon* m_file_icon { nullptr };
    ElidedLabel* m_file_name_label { nullptr };
    ElidedLabel* m_status_label { nullptr };
    QProgressBar* m_progress_bar { nullptr };
    QToolButton* m_pause_button { nullptr };
    QToolButton* m_cancel_button { nullptr };
    QToolButton* m_retry_button { nullptr };
    QToolButton* m_show_in_folder_button { nullptr };
};

class DownloadsPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit DownloadsPopover(QWidget* parent)
        : Popover(parent)
    {
        card().setObjectName("LadybirdDownloadsPopover");
        card().setFixedWidth(DOWNLOADS_POPOVER_WIDTH);

        auto* layout = new QVBoxLayout(&card());
        layout->setContentsMargins(6, 12, 6, 6);
        layout->setSpacing(0);

        auto* header = new QHBoxLayout;
        header->setContentsMargins(10, 0, 4, 6);
        auto* title = new QLabel("Downloads", this);
        title->setObjectName("LadybirdDownloadsPopoverTitle");
        header->addWidget(title, 1);
        auto* clear_button = new QPushButton("Clear finished", this);
        clear_button->setObjectName("LadybirdPopoverLink");
        clear_button->setCursor(Qt::PointingHandCursor);
        QObject::connect(clear_button, &QPushButton::clicked, this, [this] {
            if (on_clear_finished)
                on_clear_finished();
        });
        header->addWidget(clear_button);
        layout->addLayout(header);

        m_empty_label = new QLabel("No downloads", this);
        m_empty_label->setObjectName("LadybirdDownloadsEmpty");
        m_empty_label->setAlignment(Qt::AlignCenter);
        m_empty_label->setMinimumHeight(80);
        layout->addWidget(m_empty_label);

        m_scroll_area = new QScrollArea(this);
        m_scroll_area->setObjectName("LadybirdDownloadsPopoverScroll");
        m_scroll_area->setFrameShape(QFrame::NoFrame);
        m_scroll_area->setWidgetResizable(true);
        m_scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        m_rows_widget = new QWidget(m_scroll_area);
        m_rows_widget->setObjectName("LadybirdDownloadsPopoverRows");
        m_rows_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_rows_layout = new QVBoxLayout(m_rows_widget);
        m_rows_layout->setContentsMargins(0, 0, 0, 0);
        m_rows_layout->setSpacing(0);
        m_scroll_area->setWidget(m_rows_widget);
        layout->addWidget(m_scroll_area);

        auto* separator = new QFrame(this);
        separator->setObjectName("LadybirdPopoverSeparator");
        separator->setFixedHeight(1);
        auto* separator_row = new QHBoxLayout;
        separator_row->setContentsMargins(10, 4, 10, 4);
        separator_row->addWidget(separator);
        layout->addLayout(separator_row);

        auto* show_all_button = new QPushButton("Show all downloads", this);
        show_all_button->setObjectName("LadybirdPopoverRow");
        QObject::connect(show_all_button, &QPushButton::clicked, this, [this] {
            if (on_open_all_downloads)
                on_open_all_downloads();
        });
        layout->addWidget(show_all_button);
    }

    void update_chrome_style(QPalette const& palette)
    {
        setPalette(palette);
        setStyleSheet(ChromeStyle::downloads_popover_style_sheet(palette));
    }

    bool set_downloads(ReadonlySpan<WebView::FileDownloader::Download> downloads)
    {
        bool geometry_changed = false;
        auto visible_downloads = recent_downloads_for_popover(downloads);

        if (!rows_match(visible_downloads)) {
            rebuild_rows(visible_downloads);
            geometry_changed = true;
        } else {
            for (size_t i = 0; i < visible_downloads.size(); ++i) {
                if (m_download_rows[i]->update_download(*visible_downloads[i]))
                    geometry_changed = true;
            }
        }

        auto is_empty = visible_downloads.is_empty();
        auto empty_label_is_visible = !m_empty_label->isHidden();
        if (empty_label_is_visible != is_empty) {
            m_empty_label->setVisible(is_empty);
            geometry_changed = true;
        }
        auto scroll_area_is_visible = !m_scroll_area->isHidden();
        if (scroll_area_is_visible == is_empty) {
            m_scroll_area->setVisible(!is_empty);
            geometry_changed = true;
        }

        return geometry_changed;
    }

    Function<void(u64)> on_cancel_download;
    Function<void(u64)> on_pause_download;
    Function<void(u64)> on_resume_download;
    Function<void(u64)> on_show_in_folder;
    Function<void()> on_clear_finished;
    Function<void()> on_open_all_downloads;

private:
    bool rows_match(Vector<WebView::FileDownloader::Download const*> const& downloads) const
    {
        if (m_download_rows.size() != downloads.size())
            return false;

        for (size_t i = 0; i < downloads.size(); ++i) {
            if (m_download_rows[i]->id() != downloads[i]->id)
                return false;
        }

        return true;
    }

    void rebuild_rows(Vector<WebView::FileDownloader::Download const*> const& downloads)
    {
        clear_rows();
        m_download_rows.ensure_capacity(downloads.size());

        for (auto* download : downloads) {
            auto* row = new DownloadRow(*download, m_rows_widget);
            row->on_cancel_download = [this](u64 id) {
                if (on_cancel_download)
                    on_cancel_download(id);
            };
            row->on_pause_download = [this](u64 id) {
                if (on_pause_download)
                    on_pause_download(id);
            };
            row->on_resume_download = [this](u64 id) {
                if (on_resume_download)
                    on_resume_download(id);
            };
            row->on_show_in_folder = [this](u64 id) {
                if (on_show_in_folder)
                    on_show_in_folder(id);
            };
            m_rows_layout->addWidget(row);
            m_download_rows.append(row);
        }
    }

    void clear_rows()
    {
        while (auto* item = m_rows_layout->takeAt(0)) {
            if (auto* widget = item->widget())
                delete widget;
            delete item;
        }
        m_download_rows.clear();
    }

    QLabel* m_empty_label { nullptr };
    QScrollArea* m_scroll_area { nullptr };
    QWidget* m_rows_widget { nullptr };
    QVBoxLayout* m_rows_layout { nullptr };
    Vector<DownloadRow*> m_download_rows;
};

WindowToolbar::WindowToolbar(WebView::IsPrivate is_private, QWidget* parent)
    : QWidget(parent)
    , WebView::SettingsObserver(Application::settings())
    , m_is_private(is_private)
{
    auto& application = ApplicationActions::the();

    setObjectName("LadybirdToolbarContainer");
    // NB: The style sheet paints the toolbar's background, which a QWidget subclass only draws when asked to.
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName("LadybirdNavigationToolbar");
    m_toolbar->setFixedHeight(browser_chrome_layout_policy().toolbar_height);
    m_toolbar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);

    auto* toolbar_container_layout = new QVBoxLayout(this);
    toolbar_container_layout->setSpacing(0);
    toolbar_container_layout->setContentsMargins(0, 0, 0, TOOLBAR_BOTTOM_LINE_HEIGHT);

    m_downloads_button = new DownloadsButton(m_toolbar);
    QObject::connect(m_downloads_button, &QToolButton::clicked, this, [this] {
        // NB: The button only stays checked while the list is open, which showing it sees to.
        m_downloads_button->setChecked(m_downloads_popover && m_downloads_popover->isVisible());
        show_downloads_popover();
    });

    if constexpr (has_feedback_button) {
        m_feedback_button = new FeedbackButton(m_toolbar);
        QObject::connect(m_feedback_button, &QToolButton::clicked, this, [this] {
            // NB: The button only stays checked while the popover is open, which showing it sees to.
            m_feedback_button->setChecked(m_feedback_popover && m_feedback_popover->isVisible());
            show_feedback_popover();
        });
    }

    m_navigate_back_action.action().setEnabled(false);
    m_navigate_forward_action.action().setEnabled(false);
    // NB: The application's reload actions own the shortcuts, so the window and its View menu share one action for each.
    //     The window also stays shown while a page is full screen, which the toolbar doesn't.
    m_reload_action = create_application_action(*this, application.reload_action());
    m_reload_action->setShortcuts({});
    m_reload_bypassing_cache_action = create_application_action(*this, application.reload_bypassing_cache_action());
    m_reload_bypassing_cache_action->setShortcuts({});
    m_stop_action = new QAction("Stop", this);
    m_stop_action->setToolTip("Stop loading this page");
    QObject::connect(m_stop_action, &QAction::triggered, this, [this] {
        if (m_tab && m_tab->is_loaded())
            m_tab->view().stop_loading();
    });
    m_open_downloads_page_action = create_application_action(*this, application.open_downloads_page_action());

    m_window_controls_separator = new QWidget(m_toolbar);
    m_window_controls_separator->setObjectName("LadybirdToolbarWindowControlsSeparator");
    m_window_controls_separator->setFixedSize(1, 22);
    m_window_controls_separator->setAttribute(Qt::WA_TransparentForMouseEvents);

    if constexpr (use_native_macos_window_controls()) {
        auto const native_controls_width = NATIVE_MACOS_WINDOW_CONTROLS_WIDTH + NATIVE_MACOS_WINDOW_CONTROLS_GAP;
        m_window_controls = create_window_controls_spacer(*m_toolbar, "LadybirdToolbarWindowControls", { native_controls_width, 38 });
    } else {
        auto window_control_buttons = create_window_control_buttons(*m_toolbar, "LadybirdToolbarWindowControls", { 16, 16 }, { 38, 38 });
        m_window_controls = window_control_buttons.container;
        m_minimize_window_button = window_control_buttons.minimize;
        m_maximize_window_button = window_control_buttons.maximize;
        m_close_window_button = window_control_buttons.close;

        QObject::connect(m_minimize_window_button, &QToolButton::clicked, this, [this] {
            window()->showMinimized();
        });
        QObject::connect(m_maximize_window_button, &QToolButton::clicked, this, [this] {
            if (window()->isMaximized())
                window()->showNormal();
            else
                window()->showMaximized();
        });
        QObject::connect(m_close_window_button, &QToolButton::clicked, this, [this] {
            window()->close();
        });
    }

    auto* navigation_button_cluster = new QWidget(m_toolbar);
    navigation_button_cluster->setObjectName("LadybirdNavigationButtons");
    m_navigation_buttons = navigation_button_cluster;
    navigation_button_cluster->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    auto* navigation_button_layout = new QHBoxLayout(navigation_button_cluster);
    navigation_button_layout->setSpacing(TOOLBAR_SPACING);
    navigation_button_layout->setContentsMargins(0, 0, 0, 0);

    navigation_button_layout->addWidget(create_navigation_history_toolbar_button(*navigation_button_cluster, m_navigate_back_action.action(), -1));
    navigation_button_layout->addWidget(create_navigation_history_toolbar_button(*navigation_button_cluster, m_navigate_forward_action.action(), 1));
    m_reload_button = create_toolbar_button<ReloadButton>(*navigation_button_cluster, *m_reload_action);
    m_reload_button->set_actions(*m_reload_action, *m_reload_bypassing_cache_action, *m_stop_action);
    m_reload_button->on_open_in_new_tab = [this](auto activate_tab) {
        if (m_tab)
            browser_window().duplicate_tab(*m_tab, activate_tab);
    };
    navigation_button_layout->addWidget(m_reload_button);

    m_location_edit_container = new QWidget(m_toolbar);
    m_location_edit_container->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    m_location_edit_layout = new QHBoxLayout(m_location_edit_container);
    m_location_edit_layout->setSpacing(0);

    m_performance_monitor_row = new QWidget(this);
    m_performance_monitor_row->setAutoFillBackground(true);
    // NB: The monitor is wider than a narrow window, which it shouldn't hold open.
    m_performance_monitor_row->setMinimumWidth(1);
    auto* performance_monitor_row_layout = new QHBoxLayout(m_performance_monitor_row);
    performance_monitor_row_layout->setContentsMargins(TOOLBAR_HORIZONTAL_MARGIN, PERFORMANCE_MONITOR_ROW_MARGIN, TOOLBAR_HORIZONTAL_MARGIN, PERFORMANCE_MONITOR_ROW_MARGIN);
    m_performance_monitor = new PerformanceMonitorWidget(m_performance_monitor_row);
    performance_monitor_row_layout->addWidget(m_performance_monitor);
    performance_monitor_row_layout->addStretch(1);
    update_performance_monitor_row();

    m_sidebar_slot = new QWidget(m_toolbar);
    m_sidebar_slot->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    auto* sidebar_slot_layout = new QHBoxLayout(m_sidebar_slot);
    // The Ladybird button keeps as clear of the traffic lights as on the tab strip.
    auto sidebar_slot_leading_margin = 0;
    if constexpr (use_native_macos_window_controls())
        sidebar_slot_leading_margin = NATIVE_MACOS_WINDOW_CONTROLS_INSET + NATIVE_MACOS_WINDOW_CONTROLS_WIDTH + LADYBIRD_BUTTON_TRAFFIC_LIGHTS_GAP - LADYBIRD_BUTTON_FOCUS_RING_ROOM - (TOOLBAR_HORIZONTAL_MARGIN + NATIVE_MACOS_WINDOW_CONTROLS_WIDTH + NATIVE_MACOS_WINDOW_CONTROLS_GAP + TOOLBAR_SPACING);
    sidebar_slot_layout->setContentsMargins(sidebar_slot_leading_margin, 0, 0, 0);
    sidebar_slot_layout->setSpacing(TOOLBAR_SPACING);
    // NB: The separator sits where the open sidebar's edge does, so the toolbar's contents keep their place as it toggles.
    sidebar_slot_layout->addSpacing(SIDEBAR_SLOT_SEPARATOR_MARGIN - 1);
    auto* sidebar_slot_separator = new QWidget(m_sidebar_slot);
    sidebar_slot_separator->setObjectName("LadybirdToolbarWindowControlsSeparator");
    sidebar_slot_separator->setFixedSize(1, 16);
    sidebar_slot_separator->setAttribute(Qt::WA_TransparentForMouseEvents);
    sidebar_slot_layout->addWidget(sidebar_slot_separator, 0, Qt::AlignVCenter);
    sidebar_slot_layout->addSpacing(SIDEBAR_SLOT_SEPARATOR_MARGIN);
    m_sidebar_slot->hide();

    m_sidebar_footer = new QWidget(this);
    m_sidebar_footer->setObjectName("LadybirdSidebarFooter");
    m_sidebar_footer->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    auto* sidebar_footer_layout = new QHBoxLayout(m_sidebar_footer);
    sidebar_footer_layout->setContentsMargins(0, 0, 0, 0);
    sidebar_footer_layout->setSpacing(TOOLBAR_SPACING);
    sidebar_footer_layout->addStrut(SIDEBAR_FOOTER_HEIGHT);
    sidebar_footer_layout->addStretch(1);
    m_sidebar_footer->hide();

    lay_out_as_row();
    toolbar_container_layout->addWidget(m_toolbar);

    recreate_icons();
    update_chrome_style();
    set_window_controls_visible(false);
    update_feedback_button();
    config_variable_changed(WebView::ConfigVariableID::ShowFeedbackButton);
}

// NB: The tabs' address fields are in the field's container, which goes back to the toolbar, so it is torn down with it
//     as it is in a row. A window that was never shown deletes the panel first, and the container with it.
WindowToolbar::~WindowToolbar()
{
    qApp->removeEventFilter(this);
    if (!m_field_panel)
        return;
    m_location_edit_container->setParent(m_toolbar);
    delete m_field_panel.data();
}

int WindowToolbar::navigation_buttons_width_in_sidebar()
{
    return NAVIGATION_BUTTON_COUNT * SIDEBAR_NAVIGATION_BUTTON_SIZE.width();
}

int WindowToolbar::sidebar_footer_height()
{
    return SIDEBAR_FOOTER_HEIGHT;
}

void WindowToolbar::set_sidebar_footer_leading_room(int room)
{
    m_sidebar_footer_leading_room = room;
    auto margins = m_sidebar_footer->layout()->contentsMargins();
    margins.setLeft(room);
    m_sidebar_footer->layout()->setContentsMargins(margins);
}

void WindowToolbar::size_navigation_buttons(QSize size, int spacing)
{
    for (auto* button : m_navigation_buttons->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        static_cast<ToolbarButton*>(button)->set_size(size);
    m_navigation_buttons->layout()->setSpacing(spacing);
}

void WindowToolbar::lay_out_as_row()
{
    size_navigation_buttons(TOOLBAR_BUTTON_SIZE, TOOLBAR_SPACING);

    auto* toolbar_layout = new QHBoxLayout(m_toolbar);
    toolbar_layout->setSpacing(TOOLBAR_SPACING);
    toolbar_layout->setContentsMargins(TOOLBAR_HORIZONTAL_MARGIN, 0, toolbar_right_margin(false), 0);

    if constexpr (use_native_macos_window_controls())
        toolbar_layout->addWidget(m_window_controls, 0, Qt::AlignVCenter);
    toolbar_layout->addWidget(m_sidebar_slot, 0, Qt::AlignVCenter);
    toolbar_layout->addWidget(m_navigation_buttons, 0, Qt::AlignVCenter);
    m_navigation_buttons->show();
    toolbar_layout->addWidget(&field_slot(), 1);
    toolbar_layout->addWidget(m_downloads_button, 0, Qt::AlignVCenter);
    if (m_feedback_button)
        toolbar_layout->addWidget(m_feedback_button, 0, Qt::AlignVCenter);
    if constexpr (!use_native_macos_window_controls()) {
        toolbar_layout->addWidget(m_window_controls_separator, 0, Qt::AlignVCenter);
        toolbar_layout->addWidget(m_window_controls, 0, Qt::AlignVCenter);
    }

    host_field_in_panel();
    layout()->setContentsMargins(0, 0, 0, TOOLBAR_BOTTOM_LINE_HEIGHT);
}

// In the open sidebar, the address field takes the sidebar's width below its top row, which holds the navigation buttons.
// The other buttons move to the footer at the sidebar's bottom.
void WindowToolbar::lay_out_in_sidebar()
{
    size_navigation_buttons(SIDEBAR_NAVIGATION_BUTTON_SIZE, 0);

    auto* toolbar_layout = new QVBoxLayout(m_toolbar);
    toolbar_layout->setSpacing(0);
    toolbar_layout->setContentsMargins(0, 0, 0, 0);

    auto* field_row = new QHBoxLayout;
    // NB: The field's container leaves room for its focus ring all around, which the row's margins make up for.
    auto ring_room = TOOLBAR_LOCATION_FOCUS_RING_ROOM;
    field_row->setContentsMargins(m_sidebar_inset - ring_room, SIDEBAR_FIELD_TOP_MARGIN - ring_room, m_sidebar_inset - ring_room, SIDEBAR_FIELD_BOTTOM_MARGIN - ring_room);
    field_row->addWidget(&field_slot(), 1);
    toolbar_layout->addLayout(field_row);
    host_field_in_panel();

    // NB: What the sidebar has no room for stays hidden, as widgets left out of a layout would show where they last were.
    m_sidebar_slot->hide();
    m_window_controls->hide();
    m_window_controls_separator->hide();

    auto* footer_layout = static_cast<QBoxLayout*>(m_sidebar_footer->layout());
    footer_layout->addWidget(m_downloads_button, 0, Qt::AlignVCenter);
    if (m_feedback_button)
        footer_layout->addWidget(m_feedback_button, 0, Qt::AlignVCenter);
    footer_layout->setContentsMargins(m_sidebar_footer_leading_room, 0, m_sidebar_inset, 0);

    layout()->setContentsMargins(0, 0, 0, 0);
}

QWidget& WindowToolbar::field_slot()
{
    if (!m_field_panel)
        create_field_panel();
    return *m_field_slot;
}

// The field leaves room for its focus ring around it in its panel, except where the toolbar's margin already does.
QMargins WindowToolbar::field_margins_at_rest() const
{
    auto ring_room = TOOLBAR_LOCATION_FOCUS_RING_ROOM;
    if (m_arrangement == ToolbarArrangement::Sidebar)
        return { ring_room, ring_room, ring_room, ring_room };
    return { TOOLBAR_LOCATION_EDIT_MARGIN, ring_room, ring_room, ring_room };
}

void WindowToolbar::host_field_in_panel()
{
    m_location_edit_layout->setContentsMargins(field_margins_at_rest());
    static_cast<QBoxLayout*>(m_field_panel->layout())->insertWidget(0, m_location_edit_container);
    m_location_edit_container->show();
    m_field_slot->show();

    // NB: The panel follows the slot as the slot or any of its ancestors moves.
    for (QWidget* widget = m_field_slot; widget; widget = widget->parentWidget())
        widget->installEventFilter(this);
}

void WindowToolbar::create_field_panel()
{
    m_field_panel = new FieldPanel(*window());
    m_field_panel->setStyleSheet(styleSheet());
    m_field_panel->installEventFilter(this);
    auto* panel_layout = new QVBoxLayout(m_field_panel);
    panel_layout->setContentsMargins(0, 0, 0, 0);
    panel_layout->setSpacing(0);

    // The suggestions are below the line under the field.
    m_suggestions_area = new QWidget(m_field_panel);
    auto* suggestions_layout = new QVBoxLayout(m_suggestions_area);
    auto padding = FIELD_PANEL_LIST_PADDING;
    suggestions_layout->setContentsMargins(padding, 1 + padding, padding, padding);
    suggestions_layout->setSpacing(0);
    panel_layout->addWidget(m_suggestions_area);
    m_suggestions_area->hide();
    m_field_panel->set_suggestions_area(*m_suggestions_area);

    m_field_slot = new QWidget(m_toolbar);
    m_field_slot->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    m_field_slot->hide();
}

// The field grows into the panel while it lists suggestions. Without any, it stays where it is.
bool WindowToolbar::field_panel_should_expand() const
{
    return m_location_edit && m_location_edit->is_editing_location() && m_location_edit->isVisible()
        && m_location_edit->property("autocompleteOpen").toBool();
}

// NB: The sidebar's native view is raised as the sidebar is laid out. Raising the panel as a widget does nothing while it
//     is already the window's topmost child, so its native view is raised too.
void WindowToolbar::raise_field_panel()
{
    m_field_panel->raise();
    if (auto* handle = m_field_panel->windowHandle()) {
        handle->raise();
        // NB: This lets the panel's shadow follow it.
        QEvent z_order_change(QEvent::ZOrderChange);
        QCoreApplication::sendEvent(m_field_panel, &z_order_change);
    }
}

void WindowToolbar::update_field_panel()
{
    if (!m_field_panel)
        return;

    auto expanded = field_panel_should_expand();
    if (expanded != m_field_panel->is_expanded()) {
        m_field_panel->set_expanded(expanded);
        m_suggestions_area->setVisible(expanded);
        // NB: The grown panel is the field's surface, so the field fills its top row, where it was.
        m_location_edit_layout->setContentsMargins(expanded ? QMargins() : field_margins_at_rest());
        // A click outside the grown panel shrinks it back.
        if (expanded)
            qApp->installEventFilter(this);
        else
            qApp->removeEventFilter(this);
    }

    m_field_panel->set_in_sidebar(m_arrangement == ToolbarArrangement::Sidebar);

    // NB: The slot keeps the field's place in the toolbar.
    auto margins_at_rest = field_margins_at_rest();
    auto field_height = m_location_edit_container->sizeHint().height() - m_location_edit_layout->contentsMargins().top() - m_location_edit_layout->contentsMargins().bottom();
    m_field_slot->setFixedHeight(field_height + margins_at_rest.top() + margins_at_rest.bottom());
    if (!m_field_slot->isVisible()) {
        m_field_panel->hide();
        return;
    }

    auto& window = *m_field_panel->parentWidget();
    auto position = m_field_slot->mapTo(&window, QPoint());
    QSize size = m_field_slot->size();
    // NB: While the sidebar opens or closes, the slot reaches past its edge. The panel ends there, as the sidebar does
    //     not clip a native view that lies over it.
    QRect visible_rect(QPoint(), size);
    for (QWidget* widget = m_field_slot; widget->parentWidget() && widget->parentWidget() != &window; widget = widget->parentWidget())
        visible_rect = visible_rect.translated(widget->pos()) & QRect(QPoint(), widget->parentWidget()->size());
    size.setWidth(max(0, min(size.width(), visible_rect.width())));
    if (m_field_clip.has_value() && !expanded) {
        auto clipped = QRect(position, size) & *m_field_clip;
        size.setHeight(max(0, clipped.bottom() + 1 - position.y()));
    }
    if (expanded) {
        auto field_rect = QRect(position, m_field_slot->size()).marginsRemoved(margins_at_rest);
        position = field_rect.topLeft();
        auto wanted_width = m_arrangement == ToolbarArrangement::Sidebar ? FIELD_PANEL_WIDTH : FIELD_PANEL_MINIMUM_WIDTH;
        auto width = max(field_rect.width(), min(wanted_width, window.width() - position.x() - FIELD_PANEL_WINDOW_MARGIN));
        size = QSize(width, min(m_field_panel->sizeHint().height(), window.height() - position.y()));
    }
    m_field_panel->setGeometry(QRect(position, size));
    m_field_panel->show();
    raise_field_panel();
}

bool WindowToolbar::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_field_panel)
        return false;

    // NB: While the panel is grown, this filters the application's events, to see clicks outside of it.
    auto type = event->type();
    if (type == QEvent::MouseButtonPress && m_field_panel->is_expanded() && !QApplication::activePopupWidget()) {
        auto global_position = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
        auto panel_rect = QRect(m_field_panel->mapToGlobal(QPoint()), m_field_panel->size());
        if (!panel_rect.contains(global_position) && m_location_edit && m_location_edit->hasFocus())
            m_location_edit->clearFocus();
        return false;
    }

    if (watched == m_location_edit && type == QEvent::DynamicPropertyChange) {
        if (static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "autocompleteOpen")
            update_field_panel();
        return false;
    }

    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || !(widget == m_field_panel || widget == m_field_slot || widget->isAncestorOf(m_field_slot)))
        return false;
    // NB: The slot's new ancestors are followed too, as the sidebar takes the toolbar in.
    if (type == QEvent::ParentChange && widget != m_field_panel) {
        for (QWidget* ancestor = m_field_slot; ancestor; ancestor = ancestor->parentWidget())
            ancestor->installEventFilter(this);
    }
    if (first_is_one_of(type, QEvent::Move, QEvent::Resize, QEvent::Show, QEvent::Hide, QEvent::LayoutRequest, QEvent::ParentChange))
        update_field_panel();
    // NB: The panel stays above the sidebar as the sidebar is raised. Its own change of order comes from raising it.
    else if (type == QEvent::ZOrderChange && widget != m_field_panel && m_field_panel->isVisible())
        raise_field_panel();
    return false;
}

void WindowToolbar::set_arrangement(ToolbarArrangement arrangement, int sidebar_inset)
{
    if (m_arrangement == arrangement && m_sidebar_inset == sidebar_inset)
        return;
    m_arrangement = arrangement;
    m_sidebar_inset = sidebar_inset;

    // NB: Deleting the layout leaves the widgets it held in place, ready for the next one.
    delete m_toolbar->layout();
    if (arrangement == ToolbarArrangement::Row) {
        lay_out_as_row();
        m_sidebar_footer->hide();
        set_window_controls_visible(m_window_controls_visible);
    } else {
        lay_out_in_sidebar();
        m_toolbar->setMinimumHeight(0);
        m_toolbar->setMaximumHeight(QWIDGETSIZE_MAX);
        m_sidebar_footer->show();
    }

    if (m_location_edit) {
        m_location_edit->set_shows_host_only(arrangement == ToolbarArrangement::Sidebar);
        m_location_edit->set_suggestions_host(m_suggestions_area);
    }
    update_field_panel();
    apply_contents_opacity();

    setProperty("inSidebar", arrangement == ToolbarArrangement::Sidebar);
    style()->unpolish(this);
    style()->polish(this);
    updateGeometry();
    update();
}

int WindowToolbar::height_in_row() const
{
    return toolbar_height(m_window_controls_visible) + TOOLBAR_BOTTOM_LINE_HEIGHT;
}

// NB: This is known before the toolbar is laid out in the sidebar, so the sidebar can make room for it as it opens.
int WindowToolbar::height_in_sidebar() const
{
    auto field_margins = m_location_edit_layout->contentsMargins();
    auto field_height = m_location_edit_container->sizeHint().height() - field_margins.top() - field_margins.bottom();
    return SIDEBAR_FIELD_TOP_MARGIN + field_height + SIDEBAR_FIELD_BOTTOM_MARGIN;
}

void WindowToolbar::set_contents_opacity(qreal opacity)
{
    if (m_contents_opacity == opacity)
        return;
    m_contents_opacity = opacity;
    apply_contents_opacity();
}

void WindowToolbar::apply_contents_opacity()
{
    // NB: The effect draws the widget off screen, so it is only there while the contents fade.
    auto fade = [](QWidget& widget, qreal opacity) {
        auto* effect = qobject_cast<QGraphicsOpacityEffect*>(widget.graphicsEffect());
        if (opacity >= 1) {
            if (effect)
                widget.setGraphicsEffect(nullptr);
            return;
        }
        if (!effect) {
            effect = new QGraphicsOpacityEffect(&widget);
            widget.setGraphicsEffect(effect);
        }
        effect->setOpacity(opacity);
    };
    fade(*m_toolbar, m_contents_opacity);
    fade(*m_sidebar_footer, m_contents_opacity);
    // NB: In the sidebar, the navigation buttons are in its top row rather than in the toolbar.
    fade(*m_navigation_buttons, m_arrangement == ToolbarArrangement::Sidebar ? m_contents_opacity : 1);
    // NB: The field is in its panel rather than in the toolbar. The panel at rest looks like the toolbar under it, so only
    //     the field fades.
    fade(*m_location_edit_container, m_contents_opacity);
}

void WindowToolbar::set_field_clip(Optional<QRect> clip)
{
    if (m_field_clip == clip)
        return;
    m_field_clip = clip;
    update_field_panel();
}

int WindowToolbar::toolbar_height(bool)
{
    return browser_chrome_layout_policy().toolbar_height;
}

BrowserWindow& WindowToolbar::browser_window() const
{
    return as<BrowserWindow>(*window());
}

void WindowToolbar::bind(Tab* tab)
{
    if (m_tab)
        QObject::disconnect(m_tab, nullptr, this, nullptr);
    m_tab = tab;

    // A tab that hasn't loaded has no page yet, so it is shown once it has one.
    if (!tab || !tab->is_loaded()) {
        show_location_edit(nullptr);
        if (tab)
            QObject::connect(tab, &Tab::loaded, this, [this, tab] { show_tab(*tab); }, Qt::SingleShotConnection);
        return;
    }
    show_tab(*tab);
}

void WindowToolbar::show_tab(Tab& tab)
{
    auto& location_edit = tab.location_edit();
    if (m_location_edit_layout->indexOf(&location_edit) == -1)
        m_location_edit_layout->addWidget(&location_edit);
    show_location_edit(&location_edit);

    m_navigate_back_action.bind(tab.view().navigate_back_action());
    m_navigate_forward_action.bind(tab.view().navigate_forward_action());
    m_reload_button->show_loading_at_once(tab.is_loading());
    m_performance_monitor->set_stats(tab.performance_stats());

    QObject::connect(&tab, &Tab::loading_changed, this, [this] {
        m_reload_button->set_loading(m_tab->is_loading());
    });
    QObject::connect(&tab, &Tab::performance_stats_changed, this, [this] {
        m_performance_monitor->set_stats(m_tab->performance_stats());
    });
}

void WindowToolbar::show_location_edit(LocationEdit* location_edit)
{
    if (m_location_edit && m_location_edit != location_edit) {
        m_location_edit->hide();
        m_location_edit->set_suggestions_host(nullptr);
        m_location_edit->removeEventFilter(this);
        QObject::disconnect(m_location_edit, &LocationEdit::editing_location_changed, this, nullptr);
    }
    m_location_edit = location_edit;
    if (location_edit) {
        location_edit->set_shows_host_only(m_arrangement == ToolbarArrangement::Sidebar);
        location_edit->set_suggestions_host(m_suggestions_area);
        QObject::connect(location_edit, &LocationEdit::editing_location_changed, this, &WindowToolbar::update_field_panel, Qt::UniqueConnection);
        location_edit->installEventFilter(this);
        location_edit->show();
    }
    update_field_panel();
}

// A tab that leaves the window takes its address field along.
void WindowToolbar::release_location_edit(Tab& tab)
{
    if (!tab.is_loaded())
        return;

    auto& location_edit = tab.location_edit();
    if (m_location_edit_layout->indexOf(&location_edit) == -1)
        return;
    if (m_location_edit == &location_edit) {
        QObject::disconnect(m_location_edit, &LocationEdit::editing_location_changed, this, nullptr);
        m_location_edit->removeEventFilter(this);
        m_location_edit = nullptr;
    }
    location_edit.set_suggestions_host(nullptr);
    m_location_edit_layout->removeWidget(&location_edit);
    location_edit.hide();
    location_edit.setParent(&tab);
}

QToolButton* WindowToolbar::create_navigation_history_toolbar_button(QWidget& parent, QAction& action, int direction)
{
    auto* button = create_toolbar_button(parent, action);
    button->on_open_in_new_tab = [this, direction](auto activate_tab) {
        if (m_tab)
            browser_window().open_history_entry_in_new_tab(*m_tab, direction, activate_tab);
    };
    auto* menu = new QMenu(button);
    auto populate_menu = [this, menu, direction] {
        m_history_menu_tab = m_tab;
        if (m_tab && m_tab->is_loaded())
            populate_session_history_traversal_menu(*menu, m_tab->view(), direction);
        else
            menu->clear();
    };
    QObject::connect(menu, &QMenu::aboutToShow, button, populate_menu);
    QObject::connect(menu, &QMenu::triggered, button, [this](QAction* action) {
        auto tab = m_history_menu_tab;
        if (!tab || !tab->is_loaded())
            return;
        auto step = action->data().toInt();
        if (auto modifiers = QApplication::keyboardModifiers(); modifiers.testFlag(Qt::ControlModifier))
            browser_window().duplicate_tab(*tab, activate_tab_for_modifiers(modifiers), step);
        else
            tab->view().traverse_the_history_to_step(step);
    });
    button->setMenu(menu);
    button->setPopupMode(QToolButton::DelayedPopup);
    QObject::connect(button, &QToolButton::clicked, button, [button, direction] {
        if (!button->click_opens_in_new_tab())
            button->play_icon_motion(direction < 0 ? ToolbarButton::IconMotion::NudgeLeft : ToolbarButton::IconMotion::NudgeRight);
    });
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(button, &QToolButton::customContextMenuRequested, button, [button, menu, populate_menu](QPoint const&) {
        populate_menu();
        if (!menu->isEmpty())
            button->showMenu();
    });
    return button;
}

void WindowToolbar::set_window_controls_visible(bool visible)
{
    m_window_controls_visible = visible;
    // NB: The sidebar has no room for the window controls, and its toolbar is as tall as its two rows.
    if (m_arrangement == ToolbarArrangement::Sidebar)
        return;

    auto const has_native_window_controls = use_native_macos_window_controls() && visible;
    auto const has_custom_window_controls = !use_native_macos_window_controls() && visible;

    m_window_controls_separator->setVisible(has_custom_window_controls);
    m_window_controls->setVisible(has_native_window_controls || has_custom_window_controls);
    m_toolbar->setFixedHeight(toolbar_height(visible));
    m_toolbar->layout()->setContentsMargins(TOOLBAR_HORIZONTAL_MARGIN, 0, toolbar_right_margin(has_custom_window_controls), 0);
    m_toolbar->layout()->invalidate();
}

// How far the navigation buttons sit past the toolbar's leading margin beside the collapsed sidebar, where the Ladybird
// button and the sidebar's toggle come before them.
int WindowToolbar::lead_beside_collapsed_sidebar(QWidget const& ladybird_buttons, QSize sidebar_toggle_size) const
{
    auto toolbar_spacing = m_toolbar->layout()->spacing();
    auto lead = 0;
    if constexpr (use_native_macos_window_controls())
        lead += m_window_controls->width() + toolbar_spacing;

    auto slot_width = m_sidebar_slot->layout()->sizeHint().width();
    auto slot_spacing = m_sidebar_slot->layout()->spacing();
    if (ladybird_buttons.parentWidget() != m_sidebar_slot)
        slot_width += ladybird_buttons.sizeHint().width() + slot_spacing;
    if (!sidebar_toggle_size.isEmpty() && ladybird_buttons.parentWidget() != m_sidebar_slot)
        slot_width += sidebar_toggle_size.width() + slot_spacing;
    return lead + slot_width + toolbar_spacing;
}

// While the sidebar opens or closes, the toolbar's contents keep clear of where the collapsed sidebar's buttons sit.
void WindowToolbar::set_leading_room(int room)
{
    if (m_arrangement == ToolbarArrangement::Sidebar)
        return;
    auto margins = m_toolbar->layout()->contentsMargins();
    if (margins.left() == TOOLBAR_HORIZONTAL_MARGIN + room)
        return;
    margins.setLeft(TOOLBAR_HORIZONTAL_MARGIN + room);
    m_toolbar->layout()->setContentsMargins(margins);
    // NB: The toolbar moves along with the sidebar's edge at once, so its contents must too, or they lag a frame behind.
    m_toolbar->layout()->activate();
}

void WindowToolbar::update_window_control_icons()
{
    if (!m_minimize_window_button || !m_maximize_window_button || !m_close_window_button)
        return;

    auto is_maximized = window()->isMaximized();
    m_minimize_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowMinimize, Theme::chrome_palette(*this)));
    m_maximize_window_button->setIcon(create_chrome_icon(is_maximized ? ChromeIcon::WindowRestore : ChromeIcon::WindowMaximize, Theme::chrome_palette(*this)));
    m_maximize_window_button->setToolTip(is_maximized ? "Restore" : "Maximize");
    m_close_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowClose, Theme::chrome_palette(*this)));
}

void WindowToolbar::set_vertical_tabs_enabled(bool enabled)
{
    // The page's separator supplies the bottom border, leaving the toolbar open above the sidebar.
    setProperty("verticalTabs", enabled);
    style()->unpolish(this);
    style()->polish(this);
    update();

    m_toolbar->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    m_toolbar->layout()->invalidate();
}

// NB: The bar below the toolbar draws the line under both.
void WindowToolbar::set_bookmarks_bar_below(bool below)
{
    if (property("bookmarksBarBelow").toBool() == below)
        return;
    setProperty("bookmarksBarBelow", below);
    style()->unpolish(this);
    style()->polish(this);
    update();
}

bool WindowToolbar::event(QEvent* event)
{
    // An inactive window's toolbar icons drop to the muted color.
    if (event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate)
        recreate_icons();

    if (event->type() == QEvent::PaletteChange) {
        update_chrome_style();
        recreate_icons();
    }

    // NB: The monitor's row lies outside the toolbar, and leaves with it, as in full screen.
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide)
        update_performance_monitor_row();

    return QWidget::event(event);
}

void WindowToolbar::tab_settings_changed()
{
    recreate_icons();
}

void WindowToolbar::config_variable_changed(WebView::ConfigVariableID variable)
{
    if (variable == WebView::ConfigVariableID::ShowFeedbackButton && m_feedback_button) {
        auto show_button = Application::settings().config_variable_as_bool(variable);
        if (!show_button && m_feedback_popover)
            m_feedback_popover->close();
        m_feedback_button->setVisible(show_button);
    }
    if (variable == WebView::ConfigVariableID::ShowTabPerformanceMonitor)
        update_performance_monitor_row();
}

void WindowToolbar::update_performance_monitor_row()
{
    auto enabled = Application::settings().config_variable_as_bool(WebView::ConfigVariableID::ShowTabPerformanceMonitor);
    m_performance_monitor_row->setVisible(enabled && isVisible());
}

void WindowToolbar::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    auto style_sheet = ChromeStyle::toolbar_container_style_sheet(Theme::chrome_palette(*this));
    setStyleSheet(style_sheet);
    // NB: The sidebar's footer and top row lie outside the toolbar while the sidebar holds it, so what it shows there
    //     takes the style sheet itself.
    m_sidebar_footer->setStyleSheet(style_sheet);
    m_navigation_buttons->setStyleSheet(style_sheet);
    auto const& colors = Theme::chrome(Theme::chrome_palette(*this));
    auto row_palette = m_performance_monitor_row->palette();
    row_palette.setColor(QPalette::Window, colors.toolbar);
    row_palette.setColor(QPalette::WindowText, colors.text);
    m_performance_monitor_row->setPalette(row_palette);
    if (m_field_panel) {
        m_field_panel->setStyleSheet(style_sheet);
        m_field_panel->round_corners();
        m_field_panel->update();
    }
    if (m_downloads_popover)
        m_downloads_popover->update_chrome_style(palette());
    if (m_feedback_popover)
        m_feedback_popover->update_chrome_style(palette());
    m_is_updating_chrome_style = false;
}

void WindowToolbar::recreate_icons()
{
    auto activity = ChromeStyle::window_activity(*this);
    m_navigate_back_action.action().setIcon(create_chrome_icon(ChromeIcon::Back, Theme::chrome_palette(*this), activity, TOOLBAR_ICON_SIZE));
    m_navigate_forward_action.action().setIcon(create_chrome_icon(ChromeIcon::Forward, Theme::chrome_palette(*this), activity, TOOLBAR_ICON_SIZE));
    m_reload_action->setIcon(create_chrome_icon(ChromeIcon::Reload, Theme::chrome_palette(*this), activity, TOOLBAR_ICON_SIZE));
    m_stop_action->setIcon(create_chrome_icon(ChromeIcon::Stop, Theme::chrome_palette(*this), activity, TOOLBAR_ICON_SIZE));
    m_downloads_button->setIcon(create_chrome_icon(ChromeIcon::Download, Theme::chrome_palette(*this), activity, TOOLBAR_ICON_SIZE));
    if (m_feedback_button)
        m_feedback_button->setIcon(create_chrome_icon(ChromeIcon::Feedback, Theme::chrome_palette(*this), activity, TOOLBAR_ICON_SIZE));
    update_window_control_icons();
}

void WindowToolbar::update_downloads_button()
{
    auto downloads = RemoteDownloads::the().downloads_of_session(m_is_private);
    auto button_state = downloads_button_state(downloads);

    if (!button_state.has_downloads && m_downloads_popover)
        m_downloads_popover->close();

    // The arrow drops into place as the button appears, and bobs once as a download finishes.
    auto was_visible = m_downloads_button->isVisible();
    m_downloads_button->setVisible(button_state.has_downloads);
    if (!was_visible && button_state.has_downloads)
        m_downloads_button->play_icon_motion(ToolbarButton::IconMotion::DropIn);
    else if (button_state.completed_download_count > m_completed_download_count)
        m_downloads_button->play_icon_motion(ToolbarButton::IconMotion::Bob);
    m_completed_download_count = button_state.completed_download_count;
    m_downloads_button->set_progress(button_state.active_download_count > 0, button_state.active_download_progress);
    browser_window().set_failed_download_count(button_state.failed_download_count);
    m_downloads_button->set_needs_attention(browser_window().has_unseen_failed_downloads());

    auto tooltip = qstring_from_ak_string(button_state.tooltip);
    if (m_downloads_button->toolTip() != tooltip) {
        m_downloads_button->setToolTip(tooltip);
        m_downloads_button->setAccessibleName(tooltip);
    }
}

void WindowToolbar::update_downloads_popover()
{
    if (!m_downloads_popover || !m_downloads_popover->isVisible())
        return;

    if (m_downloads_popover->set_downloads(RemoteDownloads::the().downloads_of_session(m_is_private)))
        position_downloads_popover();
}

void WindowToolbar::update_feedback_button()
{
    if (!m_feedback_button)
        return;

    auto crash_report_count = WebView::CrashReportOutbox::the().waiting_report_count();
    m_feedback_button->set_crash_reports_waiting(crash_report_count > 0);

    if (m_feedback_popover && m_feedback_popover->isVisible()) {
        m_feedback_popover->set_crash_report_count(crash_report_count);
        fit_feedback_popover();
    }
}

void WindowToolbar::fit_feedback_popover()
{
    m_feedback_popover->card().layout()->activate();
    m_feedback_popover->adjustSize();
    move_popover_to_button(*m_feedback_popover, *m_feedback_button);
}

void WindowToolbar::show_feedback_popover()
{
    if (!m_feedback_popover) {
        m_feedback_popover = new FeedbackPopover(this);
        m_feedback_popover->on_visibility_change = [this](bool visible) {
            m_feedback_button->setChecked(visible);
        };
        m_feedback_popover->on_report_site = [this] {
            if (m_tab)
                m_tab->view().report_website_issue();
        };
        m_feedback_popover->on_report_bug = [this] {
            browser_window().new_tab_from_url(WebView::new_bug_report_url(Application::browser_options().disable_sandbox), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::after_current_tab());
        };
        m_feedback_popover->on_review_crash_reports = [this] {
            browser_window().new_tab_from_url(URL::URL::about("crashes"sv), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::after_current_tab());
        };
    }

    QString reportable_host;
    if (m_tab && m_tab->view().can_report_website_issue()) {
        if (m_tab->url().host().has_value())
            reportable_host = qstring_from_ak_string(m_tab->url().serialized_host());
    }
    auto crash_report_count = WebView::CrashReportOutbox::the().waiting_report_count();
    m_feedback_button->set_crash_reports_waiting(crash_report_count > 0);

    m_feedback_popover->update_chrome_style(palette());
    m_feedback_popover->set_reportable_host(reportable_host);
    m_feedback_popover->set_crash_report_count(crash_report_count);
    fit_feedback_popover();
    m_feedback_popover->show();
    move_popover_to_button(*m_feedback_popover, *m_feedback_button);
    m_feedback_popover->raise();
}

// The downloads hang from the downloads button. Without any, the downloads page opens instead.
void WindowToolbar::show_downloads_popover()
{
    auto downloads = RemoteDownloads::the().downloads_of_session(m_is_private);
    if (!downloads_button_state(downloads).has_downloads) {
        m_open_downloads_page_action->trigger();
        return;
    }
    browser_window().acknowledge_failed_downloads();

    if (!m_downloads_popover) {
        m_downloads_popover = new DownloadsPopover(this);
        m_downloads_popover->on_visibility_change = [this](bool visible) {
            m_downloads_button->setChecked(visible);
        };
        m_downloads_popover->on_cancel_download = [this](u64 id) {
            auto& file_downloader = RemoteDownloads::the();
            if (auto download = file_downloader.download(id); download.has_value() && WebView::FileDownloader::status_is_active(download->status))
                file_downloader.cancel_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_pause_download = [this](u64 id) {
            RemoteDownloads::the().pause_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_resume_download = [this](u64 id) {
            RemoteDownloads::the().resume_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_show_in_folder = [](u64 id) {
            if (auto download = RemoteDownloads::the().download(id); download.has_value())
                RemoteDownloads::the().show_in_folder(id);
        };
        m_downloads_popover->on_clear_finished = [this] {
            RemoteDownloads::the().prune_inactive_downloads(m_is_private);
            update_downloads_popover();
        };
        m_downloads_popover->on_open_all_downloads = [this] {
            if (m_downloads_popover)
                m_downloads_popover->close();
            m_open_downloads_page_action->trigger();
        };
    }

    m_downloads_popover->update_chrome_style(palette());
    (void)m_downloads_popover->set_downloads(RemoteDownloads::the().downloads_of_session(m_is_private));
    position_downloads_popover();
    m_downloads_popover->show();
    position_downloads_popover();
    m_downloads_popover->raise();
}

void WindowToolbar::position_downloads_popover()
{
    if (!m_downloads_popover)
        return;

    m_downloads_popover->adjustSize();
    auto& card = m_downloads_popover->card();
    auto size = card.sizeHint();
    size.setWidth(DOWNLOADS_POPOVER_WIDTH);
    if (size.height() > DOWNLOADS_POPOVER_MAX_HEIGHT)
        size.setHeight(DOWNLOADS_POPOVER_MAX_HEIGHT);
    card.setFixedSize(size);
    m_downloads_popover->adjustSize();

    move_popover_to_button(*m_downloads_popover, *m_downloads_button);
}

// A button on the toolbar row hangs its popover below it. In the sidebar's footer, at the bottom of the window, the
// popover stands on the button instead, reaching over the page.
void WindowToolbar::move_popover_to_button(Popover& popover, QWidget& button)
{
    if (m_arrangement == ToolbarArrangement::Sidebar)
        move_popover_above(popover, button, PopoverAlignment::Leading);
    else
        move_popover_below(popover, button, PopoverAlignment::Trailing);
}

void WindowToolbar::download_added(WebView::FileDownloader::Download const& download)
{
    // NB: A window lists the downloads of its own browsing session only.
    if (download.is_private != m_is_private)
        return;
    update_downloads_button();
    if (browser_window().isActiveWindow()) {
        show_downloads_popover();
        return;
    }
    update_downloads_popover();
}

void WindowToolbar::download_updated(WebView::FileDownloader::Download const& download)
{
    if (download.is_private != m_is_private)
        return;
    update_downloads_button();
    update_downloads_popover();
}

void WindowToolbar::download_removed(u64, WebView::IsPrivate is_private)
{
    if (is_private != m_is_private)
        return;
    update_downloads_button();
    update_downloads_popover();
}

}
