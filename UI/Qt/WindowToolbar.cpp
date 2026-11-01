/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/kmalloc.h>
#include <LibWebView/Application.h>
#include <LibWebView/DownloadPresentation.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/LadybirdButton.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/PerformanceMonitorWidget.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WindowControlButton.h>
#include <UI/Qt/WindowToolbar.h>

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
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

static constexpr int TOOLBAR_HORIZONTAL_MARGIN = 8;
// The buttons in the toolbar sit close together, while the address field keeps a wider gap on either side.
static constexpr int TOOLBAR_SPACING = 2;
// The separator after the collapsed sidebar's buttons keeps a little more room on either side.
static constexpr int SIDEBAR_SLOT_SEPARATOR_MARGIN = 6;
// NB: With the toolbar's spacing, the address field starts 8 px after the reload button.
static constexpr int TOOLBAR_LOCATION_EDIT_MARGIN = 6;
// The address field's focus ring reaches 3 px past its edge. With the toolbar's spacing, this room also puts the
// downloads button 6 px after the field.
static constexpr int TOOLBAR_LOCATION_FOCUS_RING_ROOM = 4;
static constexpr int TOOLBAR_WINDOW_CONTROLS_RIGHT_MARGIN = 4;

// NB: Unless window controls follow it, the address field ends the toolbar, and the room for its focus ring takes the
//     place of some of the toolbar's margin.
static constexpr int toolbar_right_margin(bool has_custom_window_controls)
{
    return has_custom_window_controls ? TOOLBAR_WINDOW_CONTROLS_RIGHT_MARGIN : TOOLBAR_HORIZONTAL_MARGIN - TOOLBAR_LOCATION_FOCUS_RING_ROOM;
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

        auto status_text = qstring_from_ak_string(WebView::download_status_text(download));
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
        auto visible_downloads = WebView::recent_downloads_for_popover(downloads);

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
    , m_is_private(is_private)
{
    auto& application = WebView::Application::the();

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
    // NB: The container draws the line under the toolbar along its bottom edge, below the toolbar's full height.
    toolbar_container_layout->setContentsMargins(0, 0, 0, 1);

    auto* toolbar_layout = new QHBoxLayout(m_toolbar);
    toolbar_layout->setSpacing(TOOLBAR_SPACING);
    toolbar_layout->setContentsMargins(TOOLBAR_HORIZONTAL_MARGIN, 0, toolbar_right_margin(false), 0);

    m_downloads_button = new DownloadsButton(m_toolbar);
    QObject::connect(m_downloads_button, &QToolButton::clicked, this, [this] {
        // NB: The button only stays checked while the list is open, which showing it sees to.
        m_downloads_button->setChecked(m_downloads_popover && m_downloads_popover->isVisible());
        show_downloads_popover();
    });

    m_navigate_back_action.action().setEnabled(false);
    m_navigate_forward_action.action().setEnabled(false);
    m_reload_action = create_application_action(*this, application.reload_action());
    // NB: Reloading without the cache has no button of its own, so its shortcut belongs to what holds the toolbar,
    //     which stays shown when the toolbar does not.
    m_reload_bypassing_cache_action = create_application_action(*parent, application.reload_bypassing_cache_action());
    parent->addAction(m_reload_bypassing_cache_action);
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

    auto* location_edit_container = new QWidget(m_toolbar);
    location_edit_container->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    m_location_edit_layout = new QHBoxLayout(location_edit_container);
    m_location_edit_layout->setSpacing(0);
    // NB: The field reaches the end of the toolbar, leaving room for its focus ring.
    m_location_edit_layout->setContentsMargins(TOOLBAR_LOCATION_EDIT_MARGIN, 0, TOOLBAR_LOCATION_FOCUS_RING_ROOM, 0);

    m_performance_monitor = new PerformanceMonitorWidget(m_toolbar);
    (void)WebView::TabPerformanceMonitor::the();
    m_performance_monitor->setVisible(WebView::Application::settings().config_variable_as_bool(WebView::ConfigVariableID::ShowTabPerformanceMonitor));

    if constexpr (use_native_macos_window_controls())
        toolbar_layout->addWidget(m_window_controls, 0, Qt::AlignVCenter);

    m_sidebar_slot = new QWidget(m_toolbar);
    m_sidebar_slot->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    auto* sidebar_slot_layout = new QHBoxLayout(m_sidebar_slot);
    // The Ladybird button keeps as clear of the traffic lights as on the tab strip.
    auto sidebar_slot_leading_margin = 0;
    if constexpr (use_native_macos_window_controls())
        sidebar_slot_leading_margin = NATIVE_MACOS_WINDOW_CONTROLS_INSET + NATIVE_MACOS_WINDOW_CONTROLS_WIDTH + LADYBIRD_BUTTON_TRAFFIC_LIGHTS_GAP - (TOOLBAR_HORIZONTAL_MARGIN + NATIVE_MACOS_WINDOW_CONTROLS_WIDTH + NATIVE_MACOS_WINDOW_CONTROLS_GAP + TOOLBAR_SPACING);
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
    toolbar_layout->addWidget(m_sidebar_slot, 0, Qt::AlignVCenter);

    toolbar_layout->addWidget(navigation_button_cluster, 0, Qt::AlignVCenter);
    toolbar_layout->addWidget(location_edit_container, 1);
    toolbar_layout->addWidget(m_downloads_button, 0, Qt::AlignVCenter);
    toolbar_layout->addWidget(m_performance_monitor, 0, Qt::AlignVCenter);

    if constexpr (!use_native_macos_window_controls()) {
        toolbar_layout->addWidget(m_window_controls_separator, 0, Qt::AlignVCenter);
        toolbar_layout->addWidget(m_window_controls, 0, Qt::AlignVCenter);
    }

    toolbar_container_layout->addWidget(m_toolbar);

    recreate_icons();
    update_chrome_style();
    set_window_controls_visible(false);
}

WindowToolbar::~WindowToolbar() = default;

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
    set_bookmarks_bar_below(tab.has_bookmarks_bar_in_page());

    QObject::connect(&tab, &Tab::loading_changed, this, [this] {
        m_reload_button->set_loading(m_tab->is_loading());
    });
    QObject::connect(&tab, &Tab::performance_stats_changed, this, [this] {
        m_performance_monitor->set_stats(m_tab->performance_stats());
    });
}

void WindowToolbar::show_location_edit(LocationEdit* location_edit)
{
    if (m_location_edit && m_location_edit != location_edit)
        m_location_edit->hide();
    m_location_edit = location_edit;
    if (location_edit)
        location_edit->show();
}

// A tab that leaves the window takes its address field along.
void WindowToolbar::release_location_edit(Tab& tab)
{
    if (!tab.is_loaded())
        return;

    auto& location_edit = tab.location_edit();
    if (m_location_edit_layout->indexOf(&location_edit) == -1)
        return;
    if (m_location_edit == &location_edit)
        m_location_edit = nullptr;
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
    if (ladybird_buttons.parentWidget() != m_sidebar_slot)
        slot_width += ladybird_buttons.sizeHint().width() + sidebar_toggle_size.width() + (2 * m_sidebar_slot->layout()->spacing());
    return lead + slot_width + toolbar_spacing;
}

// While the sidebar opens or closes, the toolbar's contents keep clear of where the collapsed sidebar's buttons sit.
void WindowToolbar::set_leading_room(int room)
{
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

    return QWidget::event(event);
}

void WindowToolbar::tab_settings_changed()
{
    recreate_icons();
}

void WindowToolbar::config_variable_changed(WebView::ConfigVariableID variable)
{
    if (variable == WebView::ConfigVariableID::ShowTabPerformanceMonitor)
        m_performance_monitor->setVisible(WebView::Application::settings().config_variable_as_bool(variable));
}

void WindowToolbar::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::toolbar_container_style_sheet(Theme::chrome_palette(*this)));
    if (m_downloads_popover)
        m_downloads_popover->update_chrome_style(palette());
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
    update_window_control_icons();
}

void WindowToolbar::update_downloads_button()
{
    auto downloads = WebView::Application::the().file_downloader().downloads_of_session(m_is_private);
    auto button_state = WebView::downloads_button_state(downloads);

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

    if (m_downloads_popover->set_downloads(WebView::Application::the().file_downloader().downloads_of_session(m_is_private)))
        position_downloads_popover();
}

// The downloads hang from the downloads button. Without any, the downloads page opens instead.
void WindowToolbar::show_downloads_popover()
{
    auto downloads = WebView::Application::the().file_downloader().downloads_of_session(m_is_private);
    if (!WebView::downloads_button_state(downloads).has_downloads) {
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
            auto& file_downloader = WebView::Application::the().file_downloader();
            if (auto download = file_downloader.download(id); download.has_value() && WebView::FileDownloader::status_is_active(download->status))
                file_downloader.cancel_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_pause_download = [this](u64 id) {
            WebView::Application::the().file_downloader().pause_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_resume_download = [this](u64 id) {
            WebView::Application::the().file_downloader().resume_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_show_in_folder = [](u64 id) {
            if (auto download = WebView::Application::the().file_downloader().download(id); download.has_value())
                WebView::Application::the().show_download_in_folder(*download);
        };
        m_downloads_popover->on_clear_finished = [this] {
            (void)WebView::Application::the().file_downloader().prune_inactive_downloads(m_is_private);
            update_downloads_popover();
        };
        m_downloads_popover->on_open_all_downloads = [this] {
            if (m_downloads_popover)
                m_downloads_popover->close();
            m_open_downloads_page_action->trigger();
        };
    }

    m_downloads_popover->update_chrome_style(palette());
    (void)m_downloads_popover->set_downloads(WebView::Application::the().file_downloader().downloads_of_session(m_is_private));
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

    move_popover_below(*m_downloads_popover, *m_downloads_button, PopoverAlignment::Trailing);
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
