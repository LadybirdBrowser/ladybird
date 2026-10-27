/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/kmalloc.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Autocomplete.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/LocationEdit.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QEasingCurve>
#include <QGraphicsEffect>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleHints>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>

namespace Ladybird {

static constexpr qreal LOCATION_BUTTON_RADIUS = 5;
static constexpr int SITE_SETTINGS_KNOB_SLIDE_DURATION_MS = 240;

class LocationActionButton final : public QToolButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit LocationActionButton(QWidget* parent)
        : QToolButton(parent)
    {
        QObject::connect(&m_motion_animation, &QVariantAnimation::valueChanged, this, [this] { update(); });
    }

    void play_motion(LocationEdit::TrailingActionMotion motion, QIcon previous_icon)
    {
        m_motion = motion;
        m_previous_icon = AK::move(previous_icon);
        m_motion_animation.stop();
        m_motion_animation.setStartValue(0.0);
        m_motion_animation.setEndValue(1.0);
        m_motion_animation.setDuration(motion == LocationEdit::TrailingActionMotion::Pop ? POP_DURATION_MS : FADE_DURATION_MS);
        m_motion_animation.start();
    }

private:
    static constexpr int POP_DURATION_MS = 220;
    static constexpr qreal POP_SCALE = 0.12;
    static constexpr qreal POP_PEAK_AT = 0.45;
    static constexpr int FADE_DURATION_MS = 160;

    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (isDown() || underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(isDown() ? Theme::panel().pressed : Theme::chrome(*this).hover);
            painter.drawRoundedRect(QRectF(rect()), LOCATION_BUTTON_RADIUS, LOCATION_BUTTON_RADIUS);
        }

        auto icon_rect = QRect(
            (width() - iconSize().width()) / 2,
            (height() - iconSize().height()) / 2,
            iconSize().width(),
            iconSize().height());
        auto mode = isEnabled() ? QIcon::Normal : QIcon::Disabled;
        auto state = isDown() ? QIcon::On : QIcon::Off;

        if (m_motion_animation.state() == QAbstractAnimation::Running) {
            auto progress = m_motion_animation.currentValue().toReal();
            if (m_motion == LocationEdit::TrailingActionMotion::Fade) {
                painter.setOpacity(1 - progress);
                m_previous_icon.paint(&painter, icon_rect, Qt::AlignCenter, mode, state);
                painter.setOpacity(progress);
            } else {
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                auto scale = 1 + (POP_SCALE * Motion::there_and_back(progress, POP_PEAK_AT));
                auto center = QRectF(icon_rect).center();
                painter.translate(center);
                painter.scale(scale, scale);
                painter.translate(-center);
            }
        }
        icon().paint(&painter, icon_rect, Qt::AlignCenter, mode, state);
    }

    LocationEdit::TrailingActionMotion m_motion { LocationEdit::TrailingActionMotion::Pop };
    QIcon m_previous_icon;
    QVariantAnimation m_motion_animation;
};

// The focused field is ringed in the accent color: a crisp line right outside its edge, inside a wider, softer ring.
// The effect draws outside the field's bounds, where a style sheet cannot.
class FocusRingEffect final : public QGraphicsEffect {
public:
    AK_ALLOC_WITH_KMALLOC;

    using QGraphicsEffect::QGraphicsEffect;

    void set_opacity(qreal opacity)
    {
        m_opacity = opacity;
        // NB: An enabled effect renders the field offscreen on every repaint, even when the ring is invisible.
        setEnabled(opacity > 0);
        update();
    }

private:
    static constexpr qreal OUTER_RING_WIDTH = 2;

    virtual QRectF boundingRectFor(QRectF const& rect) const override
    {
        auto outset = 1 + OUTER_RING_WIDTH;
        return rect.adjusted(-outset, -outset, outset, outset);
    }

    virtual void draw(QPainter* painter) override
    {
        QPoint offset;
        auto source = sourcePixmap(Qt::LogicalCoordinates, &offset);
        painter->drawPixmap(offset, source);

        auto field_rect = sourceBoundingRect(Qt::LogicalCoordinates);
        auto radius = static_cast<qreal>(ChromeStyle::LOCATION_EDIT_CORNER_RADIUS);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setOpacity(m_opacity);
        painter->setBrush(Qt::NoBrush);

        auto outer_rect = field_rect.adjusted(-1 - (OUTER_RING_WIDTH / 2), -1 - (OUTER_RING_WIDTH / 2), 1 + (OUTER_RING_WIDTH / 2), 1 + (OUTER_RING_WIDTH / 2));
        auto outer_radius = radius + 1 + (OUTER_RING_WIDTH / 2);
        painter->setPen(QPen(Theme::focus_ring(), OUTER_RING_WIDTH));
        painter->drawRoundedRect(outer_rect, outer_radius, outer_radius);

        auto inner_rect = field_rect.adjusted(-0.5, -0.5, 0.5, 0.5);
        painter->setPen(QPen(Theme::focus_edge(), 1));
        painter->drawRoundedRect(inner_rect, radius + 0.5, radius + 0.5);
        painter->restore();
    }

    qreal m_opacity { 0 };
};

static int qstring_offset_for_byte_offset(String const& text, size_t byte_offset)
{
    auto prefix = text.bytes_as_string_view().substring_view(0, byte_offset);
    return static_cast<int>(qstring_from_ak_string(prefix).length());
}

// The field's content is inset more on its leading side than on its trailing side.
static constexpr int LOCATION_LEADING_EDGE_MARGIN = 2;
static constexpr int LOCATION_TRAILING_EDGE_MARGIN = 3;
// The text starts this far in when nothing comes before it. The style sheet pads the field by as much.
static constexpr int LOCATION_TEXT_PADDING = 8;
static constexpr int LOCATION_TRAILING_ITEM_GAP = 6;
static constexpr int LOCATION_LEADING_ITEM_GAP = 6;
static constexpr int LOCATION_TRAILING_ACTION_WIDTH = 24;
static constexpr int LOCATION_SITE_SETTINGS_BUTTON_WIDTH = 26;
static constexpr int LOCATION_TRAILING_ACTION_HEIGHT = 22;
static constexpr int LOCATION_PILL_HEIGHT = 22;
static constexpr int LOCATION_PILL_HORIZONTAL_PADDING = 16;
// The zoom chip is at least wide enough for any zoom level up to 999%, so it keeps its width as the level changes.
static constexpr int LOCATION_ZOOM_CHIP_MIN_WIDTH = 54;
static constexpr int LOCATION_ZOOM_CHIP_GAP = 3;
// The location chip says what it means at first, then shrinks to its icon.
static constexpr int LOCATION_IN_USE_CHIP_COLLAPSE_DELAY_MS = 4000;
static constexpr int LOCATION_IN_USE_CHIP_ICON_ONLY_WIDTH = 30;

LocationEdit::LocationEdit(QWidget* parent, WebView::IsPrivate is_private)
    : NativeTextEditingLineEdit(parent)
    , m_omnibox(is_private)
    , m_autocomplete(new Autocomplete(this))
{
    setObjectName("LadybirdLocationEdit");
    setMinimumHeight(28);
    update_chrome_style();

    m_focus_ring_effect = new FocusRingEffect(this);
    setGraphicsEffect(m_focus_ring_effect);
    update_focus_ring(0);

    m_focus_ring_animation = new QVariantAnimation(this);
    m_focus_ring_animation->setDuration(Motion::FAST);
    m_focus_ring_animation->setEasingCurve(Motion::enter_easing());
    connect(m_focus_ring_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        update_focus_ring(value.toReal());
    });

    m_leading_icon_button = new QToolButton(this);
    m_leading_icon_button->setObjectName("LadybirdLocationIcon");
    m_leading_icon_button->setIconSize({ 16, 16 });
    m_leading_icon_button->setFixedSize(LOCATION_SITE_SETTINGS_BUTTON_WIDTH, LOCATION_PILL_HEIGHT);
    m_leading_icon_button->setAutoRaise(true);
    m_leading_icon_button->setFocusPolicy(Qt::NoFocus);
    m_leading_icon_button->setCursor(Qt::ArrowCursor);
    m_leading_icon_button->hide();
    connect(m_leading_icon_button, &QToolButton::clicked, this, [this] {
        if (on_site_settings_requested)
            on_site_settings_requested();
    });

    m_trailing_action_button = new LocationActionButton(this);
    m_trailing_action_button->setObjectName("LadybirdLocationAction");
    m_trailing_action_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_trailing_action_button->setIconSize({ 16, 16 });
    m_trailing_action_button->setFixedSize(LOCATION_TRAILING_ACTION_WIDTH, LOCATION_TRAILING_ACTION_HEIGHT);
    m_trailing_action_button->setAutoRaise(true);
    m_trailing_action_button->setFocusPolicy(Qt::NoFocus);
    m_trailing_action_button->setCursor(Qt::ArrowCursor);
    m_trailing_action_button->hide();

    // While the text is inline completed to a page, the page's title stands where the bookmark star does.
    m_completion_title = new QLabel(this);
    m_completion_title->setObjectName("LadybirdLocationCompletionTitle");
    m_completion_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto completion_title_font = m_completion_title->font();
    completion_title_font.setPointSizeF(12);
    m_completion_title->setFont(completion_title_font);
    m_completion_title->hide();

    m_zoom_indicator_button = new QToolButton(this);
    m_zoom_indicator_button->setObjectName("LadybirdLocationZoomIndicator");
    m_zoom_indicator_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_zoom_indicator_button->setFixedHeight(LOCATION_PILL_HEIGHT);
    m_zoom_indicator_button->setAutoRaise(true);
    m_zoom_indicator_button->setFocusPolicy(Qt::NoFocus);
    m_zoom_indicator_button->setCursor(Qt::ArrowCursor);
    m_zoom_indicator_button->hide();
    m_zoom_indicator_opacity = new QGraphicsOpacityEffect(m_zoom_indicator_button);
    m_zoom_indicator_button->setGraphicsEffect(m_zoom_indicator_opacity);
    m_zoom_indicator_fade = new QVariantAnimation(this);
    m_zoom_indicator_fade->setDuration(Motion::FAST);
    m_zoom_indicator_fade->setEndValue(1.0);
    connect(m_zoom_indicator_fade, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_zoom_indicator_opacity->setOpacity(value.toReal());
    });
    connect(m_zoom_indicator_fade, &QVariantAnimation::finished, this, [this] {
        if (m_zoom_indicator_opacity->opacity() > 0)
            return;
        m_zoom_indicator_button->hide();
        update_text_margins();
        update_trailing_item_positions();
    });
    connect(m_zoom_indicator_button, &QToolButton::clicked, this, [this] {
        if (on_zoom_requested)
            on_zoom_requested();
    });

    m_location_in_use_button = new QToolButton(this);
    m_location_in_use_button->setObjectName("LadybirdLocationInUse");
    m_location_in_use_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_location_in_use_button->setIconSize({ 16, 16 });
    m_location_in_use_button->setText("Using your location");
    m_location_in_use_button->setToolTip("This page is using your location");
    m_location_in_use_button->setFixedHeight(LOCATION_PILL_HEIGHT);
    m_location_in_use_button->setAutoRaise(true);
    m_location_in_use_button->setFocusPolicy(Qt::NoFocus);
    m_location_in_use_button->setCursor(Qt::ArrowCursor);
    m_location_in_use_button->hide();
    connect(m_location_in_use_button, &QToolButton::clicked, this, [this] {
        if (on_site_settings_requested)
            on_site_settings_requested();
    });
    m_location_in_use_collapse_timer = new QTimer(this);
    m_location_in_use_collapse_timer->setSingleShot(true);
    m_location_in_use_collapse_timer->setInterval(LOCATION_IN_USE_CHIP_COLLAPSE_DELAY_MS);
    connect(m_location_in_use_collapse_timer, &QTimer::timeout, this, [this] {
        m_location_in_use_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        update_location_in_use_chip();
        update_text_margins();
        update_trailing_item_positions();
    });

    for (auto* button : { m_leading_icon_button, m_trailing_action_button, m_zoom_indicator_button, m_location_in_use_button })
        button->installEventFilter(this);

    update_text_margins();
    setPlaceholderText("Search or enter address");
    update_location_icon();

    m_omnibox.on_display_change = [this](WebView::Omnibox::Display const& display) {
        auto display_text = qstring_from_ak_string(display.text);
        if (text() != display_text)
            setText(display_text);

        if (display.selection_start.has_value()) {
            auto selection_start = qstring_offset_for_byte_offset(display.text, *display.selection_start);
            auto selection_length = static_cast<int>(display_text.length()) - selection_start;
            setCursorPosition(static_cast<int>(display_text.length()));
            cursorBackward(true, selection_length);
        } else {
            deselect();
            setCursorPosition(static_cast<int>(display_text.length()));
        }
        update_completion_title();
    };

    m_omnibox.on_suggestions_change = [this] {
        update_completion_title();
        if (!m_omnibox.is_popup_visible()) {
            m_autocomplete->close();
            return;
        }
        m_autocomplete->show_with_suggestions(m_omnibox.suggestions(), m_omnibox.selected_suggestion());
    };

    m_omnibox.on_selection_change = [this] {
        m_autocomplete->set_selected_suggestion(m_omnibox.selected_suggestion());
    };

    m_omnibox.on_commit = [this](String const& input) {
        auto destination_kind = m_omnibox.destination_kind_for_last_commit();
        auto input_text = qstring_from_ak_string(input);
        if (text() != input_text)
            setText(input_text);

        clearFocus();

        auto ctrl_held = QApplication::keyboardModifiers() & Qt::ControlModifier;
        auto append_tld = ctrl_held ? WebView::AppendTLD::Yes : WebView::AppendTLD::No;

        auto url = WebView::sanitize_url(input, WebView::Application::settings().search_engine_settings().engine, append_tld);
        auto classified_input = WebView::classify_user_input(input, append_tld);
        if (destination_kind == WebView::OmniboxDestinationKind::Search
            || classified_input.classification != WebView::UserInputClassification::ExternalURL)
            set_url(url);

        if (on_navigation)
            on_navigation(input, AK::move(url), destination_kind);
    };

    connect(m_autocomplete, &Autocomplete::suggestion_clicked, this, [this](int suggestion_index) {
        m_omnibox.suggestion_clicked(static_cast<size_t>(suggestion_index));
    });

    connect(m_autocomplete, &Autocomplete::dismissed, this, [this] {
        m_omnibox.popup_dismissed();
    });

    connect(this, &QLineEdit::textEdited, this, [this] {
        if (m_url_is_hidden)
            m_has_user_edited_hidden_url = true;

        bool cursor_at_end = !hasSelectedText() && cursorPosition() == text().length();
        m_omnibox.text_edited(ak_string_from_qstring(text()), cursor_at_end);
        update_completion_title();
    });

    connect(this, &QLineEdit::textChanged, this, [this] {
        highlight_location();
        update_location_icon();
    });

    connect(this, &QLineEdit::cursorPositionChanged, this, [this](int, int new_position) {
        // A selection reaching the end of the text (an inline completion, select-all on focus) still
        // counts as "at the end": replacing it appends, and completions may be applied over it.
        bool at_end = new_position == text().length()
            || (hasSelectedText() && selectionEnd() == text().length());
        m_omnibox.cursor_moved(at_end);
    });

    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        schedule_chrome_style_update();
    });
}

QWidget& LocationEdit::trailing_action_button() const
{
    return *m_trailing_action_button;
}

QWidget& LocationEdit::site_settings_button() const
{
    return *m_leading_icon_button;
}

void LocationEdit::set_trailing_action(QAction* action)
{
    m_trailing_action_button->setDefaultAction(action);
    update_trailing_action_visibility();
}

void LocationEdit::animate_trailing_action_change(TrailingActionMotion motion, Function<void()> const& change)
{
    auto previous_icon = m_trailing_action_button->icon();
    change();
    // NB: The action's icon is replaced as the change takes effect, and the button takes it up at once.
    if (SystemAppearance::prefers_reduced_motion() || m_trailing_action_button->icon().cacheKey() == previous_icon.cacheKey())
        return;
    static_cast<LocationActionButton*>(m_trailing_action_button)->play_motion(motion, AK::move(previous_icon));
}

void LocationEdit::follow_site_settings_popover(QWidget& popover)
{
    if (m_site_settings_popover == &popover)
        return;
    m_site_settings_popover = &popover;
    popover.installEventFilter(this);
}

void LocationEdit::slide_site_settings_knobs(qreal travel)
{
    if (!m_site_settings_knob_animation) {
        m_site_settings_knob_animation = new QVariantAnimation(this);
        m_site_settings_knob_animation->setDuration(SITE_SETTINGS_KNOB_SLIDE_DURATION_MS);
        m_site_settings_knob_animation->setEasingCurve(Motion::move_easing());
        QObject::connect(m_site_settings_knob_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            m_site_settings_knob_travel = value.toReal();
            if (m_shows_site_settings_button)
                m_leading_icon_button->setIcon(create_site_settings_icon(Theme::chrome_palette(*this), m_site_settings_knob_travel));
        });
    }

    m_site_settings_knob_animation->stop();
    if (SystemAppearance::prefers_reduced_motion()) {
        m_site_settings_knob_travel = travel;
        update_location_icon();
        return;
    }
    m_site_settings_knob_animation->setStartValue(m_site_settings_knob_travel);
    m_site_settings_knob_animation->setEndValue(travel);
    m_site_settings_knob_animation->start();
}

// The bookmark star is about the page, so it stands aside while the text is being edited, and for a new tab, which has
// no page yet.
void LocationEdit::update_trailing_action_visibility()
{
    m_trailing_action_button->setVisible(m_trailing_action_button->defaultAction() && !m_is_editing_location && !m_url_is_hidden);
    update_text_margins();
    update_trailing_item_positions();
}

void LocationEdit::update_completion_title()
{
    auto title = m_is_editing_location ? m_omnibox.completion_title() : OptionalNone {};
    if (title.has_value()) {
        // NB: The field's style sheet would color the title as the text it follows.
        m_completion_title->setStyleSheet(qformatted("color: {}; background: transparent;", ChromeStyle::style_sheet_color(Theme::chrome(*this).muted)));
        auto maximum_width = width() / 3;
        m_completion_title->setText(m_completion_title->fontMetrics().elidedText(qstring_from_ak_string(*title), Qt::ElideRight, maximum_width));
        m_completion_title->adjustSize();
    }
    m_completion_title->setVisible(title.has_value());
    update_text_margins();
    update_trailing_item_positions();
}

QAction* LocationEdit::trailing_action() const
{
    return m_trailing_action_button->defaultAction();
}

void LocationEdit::set_zoom_action(QAction* action)
{
    if (m_zoom_action == action)
        return;

    if (m_zoom_action)
        QObject::disconnect(m_zoom_action, nullptr, this, nullptr);

    m_zoom_action = action;

    if (m_zoom_action)
        connect(m_zoom_action, &QAction::changed, this, &LocationEdit::update_zoom_indicator);

    update_zoom_indicator();
}

void LocationEdit::set_url_is_hidden(bool url_is_hidden)
{
    if (m_url_is_hidden == url_is_hidden)
        return;

    m_url_is_hidden = url_is_hidden;
    m_has_user_edited_hidden_url = false;

    if (m_url_is_hidden)
        set_text_and_restart_editing({});
    update_trailing_action_visibility();
}

void LocationEdit::show_autocomplete()
{
    if (!window() || !window()->isVisible())
        return;

    m_omnibox.show_all_suggestions();
}

void LocationEdit::changeEvent(QEvent* event)
{
    QLineEdit::changeEvent(event);
    if (event->type() == QEvent::ActivationChange) {
        highlight_location();
        // The focus ring only shows in the active window, though the field keeps its focus and its editing session.
        if (m_focus_ring_animation)
            m_focus_ring_animation->stop();
        if (!isActiveWindow())
            update_focus_ring(0);
        else if (m_is_editing_location && hasFocus())
            update_focus_ring(1);
    }
    // NB: Repolishing for the hover look changes our palette to the hovered colors, which needs no new style.
    if (event->type() == QEvent::PaletteChange && m_is_updating_field_hover)
        return;
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange) {
        schedule_chrome_style_update();
    }
}

bool LocationEdit::event(QEvent* event)
{
    if (event->type() == QEvent::Enter)
        set_field_hovered(true);
    else if (event->type() == QEvent::Leave)
        set_field_hovered(false);
    return QLineEdit::event(event);
}

bool LocationEdit::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_site_settings_popover) {
        if (event->type() == QEvent::Show)
            slide_site_settings_knobs(1);
        else if (event->type() == QEvent::Hide)
            slide_site_settings_knobs(0);
        return QLineEdit::eventFilter(watched, event);
    }

    // The buttons inside the field have their own hover look, so the field itself does not look hovered under them.
    // NB: Clicking a button that takes no focus gives the focus to the nearest widget around it that does, which is the
    //     field. A click on a button opens something about the page, rather than editing its address, so the field
    //     takes no focus from clicks while the mouse is over a button.
    if (event->type() == QEvent::Enter) {
        set_field_hovered(false);
        setFocusPolicy(Qt::TabFocus);
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::Hide) {
        set_field_hovered(underMouse());
        setFocusPolicy(Qt::StrongFocus);
    }
    return QLineEdit::eventFilter(watched, event);
}

// NB: The style sheet's :hover would also match while the mouse is over one of the buttons inside the field, so the
//     field's hover look follows this property instead.
void LocationEdit::set_field_hovered(bool hovered)
{
    if (property("fieldHovered").toBool() == hovered)
        return;
    setProperty("fieldHovered", hovered);
    m_is_updating_field_hover = true;
    style()->unpolish(this);
    style()->polish(this);
    m_is_updating_field_hover = false;
    update();
}

void LocationEdit::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu* menu = createStandardContextMenu();

    auto* paste_action = menu->findChild<QAction*>(QStringLiteral("edit-paste"), Qt::FindDirectChildrenOnly);

    auto qt_clipboard_text = QGuiApplication::clipboard()->text();
    auto clipboard_text = ak_string_from_qstring(qt_clipboard_text);

    bool has_search_engine = WebView::Application::settings().search_engine_settings().engine.has_value();
    auto paste_and_go_text = WebView::Omnibox::text_for_paste_and_go_action(clipboard_text, has_search_engine);
    // Escape any &s so Qt doesn't treat them as mnemonics.
    paste_and_go_text = MUST(paste_and_go_text.replace("&"sv, "&&"sv, ReplaceMode::All));

    bool clipboard_holds_url = !has_search_engine || (!clipboard_text.is_empty() && WebView::location_looks_like_url(clipboard_text));
    auto paste_and_go_icon = clipboard_holds_url
        ? create_chrome_icon(ChromeIcon::Forward, Theme::chrome_palette(*this))
        : QIcon::fromTheme(QIcon::ThemeIcon::SystemSearch);

    auto* paste_and_go_action = new QAction(paste_and_go_icon, qstring_from_ak_string(paste_and_go_text), menu);
    paste_and_go_action->setEnabled(!isReadOnly() && !qt_clipboard_text.isEmpty());
    connect(paste_and_go_action, &QAction::triggered, this, [this, clipboard_text, qt_clipboard_text] {
        setText(qt_clipboard_text);
        m_omnibox.navigate_directly_to_query(clipboard_text);
    });

    // Try to insert "Paste and Go" after the "Paste" action if we can find it.
    bool added_paste_and_go_action = false;
    if (paste_action) {
        auto const& actions = menu->actions();
        auto paste_index = actions.indexOf(paste_action);
        if (paste_index >= 0 && paste_index + 1 < actions.size()) {
            menu->insertAction(actions.at(paste_index + 1), paste_and_go_action);
            added_paste_and_go_action = true;
        }
    }
    // ...otherwise append it to the end.
    if (!added_paste_and_go_action)
        menu->addAction(paste_and_go_action);

    execute_context_menu(*menu, event->globalPos());
    delete menu;
}

// Leaving a tab parks its omnibox's editing session: the suggestion popup closes, but everything else is kept,
// including an inline completion and the suggestions, and the session picks up where it left off when the omnibox
// gets focus again.
void LocationEdit::park_editing_session()
{
    if (!hasFocus() || m_parked_editing_session.has_value())
        return;

    // NB: Only the popup's window closes. Dismissing the popup would end the omnibox's popup session, which drops an
    //     inline completion from the text.
    m_autocomplete->close();
    m_omnibox.set_suspended(true);

    m_parked_editing_session = ParkedEditingSession {
        .cursor_position = cursorPosition(),
        .selection_start = selectionStart(),
        .selection_length = static_cast<int>(selectedText().length()),
    };
}

void LocationEdit::focusInEvent(QFocusEvent* event)
{
    if (auto parked = m_parked_editing_session; parked.has_value()) {
        m_parked_editing_session.clear();
        NativeTextEditingLineEdit::focusInEvent(event);
        m_omnibox.set_suspended(false);

        // Restore the selection with the cursor at the end it was on.
        if (parked->selection_length > 0) {
            if (parked->cursor_position == parked->selection_start)
                setSelection(parked->selection_start + parked->selection_length, -parked->selection_length);
            else
                setSelection(parked->selection_start, parked->selection_length);
        } else {
            setCursorPosition(parked->cursor_position);
        }
        if (m_omnibox.is_popup_visible())
            m_autocomplete->show_with_suggestions(m_omnibox.suggestions(), m_omnibox.selected_suggestion());
        show_focus_ring();
        return;
    }

    if (event->reason() == Qt::PopupFocusReason) {
        NativeTextEditingLineEdit::focusInEvent(event);
        m_omnibox.set_suspended(false);
        return;
    }

    auto should_defer_full_url = event->reason() == Qt::MouseFocusReason
        && m_url.has_value()
        && text() == display_url();

    NativeTextEditingLineEdit::focusInEvent(event);

    m_should_show_full_url_on_mouse_release = should_defer_full_url;

    if (!should_defer_full_url && m_url.has_value() && text() == display_url())
        setText(serialized_url());

    m_omnibox.begin_editing(ak_string_from_qstring(text()));

    highlight_location();
    show_focus_ring();

    if (!should_defer_full_url) {
        QTimer::singleShot(0, this, [this] {
            if (hasFocus())
                selectAll();
        });
    }
}

void LocationEdit::focusOutEvent(QFocusEvent* event)
{
    QLineEdit::focusOutEvent(event);

    if (m_parked_editing_session.has_value())
        return;

    // A popup (for example the context menu) borrows focus without ending the editing session; late
    // suggestion deliveries must not rewrite the text or raise the suggestion popup while it is open.
    if (event->reason() == Qt::PopupFocusReason) {
        m_omnibox.set_suspended(true);
        return;
    }

    hide_focus_ring();

    m_omnibox.end_editing();
    m_should_show_full_url_on_mouse_release = false;

    if (m_url_is_hidden) {
        set_url_is_hidden(false);
        if (text().isEmpty() && m_url.has_value())
            setText(display_url());
    } else if (m_url.has_value() && text() == serialized_url()) {
        setText(display_url());
    }

    deselect();
    setCursorPosition(0);
    highlight_location();
}

void LocationEdit::update_focus_ring(qreal opacity)
{
    m_focus_ring_opacity = opacity;
    if (m_focus_ring_effect)
        static_cast<FocusRingEffect*>(m_focus_ring_effect)->set_opacity(opacity);
}

// The ring fades in when the field gets focus, and disappears at once when it loses it.
void LocationEdit::show_focus_ring()
{
    m_is_editing_location = true;
    update_trailing_action_visibility();

    if (!m_focus_ring_animation)
        return;

    m_focus_ring_animation->stop();
    m_focus_ring_animation->setStartValue(m_focus_ring_opacity);
    m_focus_ring_animation->setEndValue(1.0);
    m_focus_ring_animation->start();
}

void LocationEdit::show_focus_ring_at_once()
{
    if (!hasFocus() || !isActiveWindow())
        return;
    if (m_focus_ring_animation)
        m_focus_ring_animation->stop();
    update_focus_ring(1);
}

void LocationEdit::hide_focus_ring()
{
    m_is_editing_location = false;
    update_trailing_action_visibility();
    update_completion_title();

    if (m_focus_ring_animation)
        m_focus_ring_animation->stop();
    update_focus_ring(0);
}

void LocationEdit::native_text_editing_command_will_execute(Web::TextEditingCommand const& command)
{
    if (command.extend_selection) {
        auto focus = cursorPosition();
        auto anchor = hasSelectedText() ? (focus == selectionStart() ? selectionEnd() : selectionStart()) : focus;
        m_omnibox.accept_completion();
        setSelection(anchor, focus - anchor);
        return;
    }
    using Type = Web::TextEditingCommand::Type;
    switch (command.type) {
    case Type::DeleteBackward:
    case Type::DeleteForward:
    case Type::DeleteWordBackward:
    case Type::DeleteWordForward:
    case Type::DeleteToEndOfParagraph:
        m_omnibox.will_delete_text();
        break;
    case Type::MoveForward:
    case Type::MoveWordForward:
    case Type::MoveDown:
    case Type::MoveToEndOfParagraph:
    case Type::MoveToEndOfLine:
    case Type::MoveToRightEndOfLine:
    case Type::PageDown:
        m_omnibox.accept_completion();
        break;
    default:
        m_omnibox.cursor_moved(false);
        break;
    }
}

void LocationEdit::keyPressEvent(QKeyEvent* event)
{
    if (handle_native_text_editing_key(*event))
        return;
    if (event->key() == Qt::Key_Escape) {
        if (m_omnibox.escape_pressed() == WebView::Omnibox::EscapeAction::ClosedPopup)
            return;
        if (m_url.has_value())
            setText(serialized_url());
        clearFocus();
        emit focus_return_requested();
        return;
    }

    if (event->key() == Qt::Key_Down) {
        if (m_omnibox.select_next_suggestion())
            return;
    }

    if (event->key() == Qt::Key_Up) {
        if (m_omnibox.select_previous_suggestion())
            return;
    }

    if (event->key() == Qt::Key_Right || event->key() == Qt::Key_End) {
        if (m_omnibox.accept_completion()) {
            event->accept();
            return;
        }
    }

    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        m_omnibox.return_pressed();
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete)
        m_omnibox.will_delete_text();

    QLineEdit::keyPressEvent(event);
}

void LocationEdit::mouseReleaseEvent(QMouseEvent* event)
{
    QLineEdit::mouseReleaseEvent(event);

    if (event->button() == Qt::LeftButton && m_should_show_full_url_on_mouse_release)
        show_full_url_preserving_display_selection();
}

void LocationEdit::resizeEvent(QResizeEvent* event)
{
    QLineEdit::resizeEvent(event);

    update_trailing_item_positions();
}

// The "Not secure" pill is inset as far from the field's leading edge as from its top and bottom.
static int leading_indicator_inset(QWidget const& field, QWidget const& indicator)
{
    if (indicator.property("notSecure").toBool())
        return (field.height() - indicator.height()) / 2;
    return LOCATION_LEADING_EDGE_MARGIN;
}

void LocationEdit::update_trailing_item_positions()
{
    auto button_size = m_leading_icon_button->size();
    m_leading_icon_button->move(leading_indicator_inset(*this, *m_leading_icon_button), (height() - button_size.height()) / 2);

    auto trailing_button_size = m_trailing_action_button->size();
    auto trailing_x = width() - trailing_button_size.width() - LOCATION_TRAILING_EDGE_MARGIN;
    auto trailing_y = (height() - trailing_button_size.height()) / 2;
    m_trailing_action_button->move(trailing_x, trailing_y);

    // NB: The items before the star are placed from the star's place, whether it shows or not.
    auto items_x = trailing_x;

    auto completion_title_size = m_completion_title->size();
    m_completion_title->move(width() - LOCATION_TRAILING_EDGE_MARGIN - LOCATION_TEXT_PADDING - completion_title_size.width(), (height() - completion_title_size.height()) / 2);

    auto zoom_button_size = m_zoom_indicator_button->size();
    auto zoom_y = (height() - zoom_button_size.height()) / 2;
    m_zoom_indicator_button->move(items_x - LOCATION_ZOOM_CHIP_GAP - zoom_button_size.width(), zoom_y);
    m_zoom_indicator_button->raise();

    // The location chip stands before the zoom chip, when there is one.
    auto location_chip_x = items_x;
    if (!m_zoom_indicator_button->isHidden())
        location_chip_x -= LOCATION_ZOOM_CHIP_GAP + zoom_button_size.width();
    auto location_chip_size = m_location_in_use_button->size();
    m_location_in_use_button->move(location_chip_x - LOCATION_ZOOM_CHIP_GAP - location_chip_size.width(), (height() - location_chip_size.height()) / 2);
    m_location_in_use_button->raise();
    m_trailing_action_button->raise();
}

void LocationEdit::search_engine_settings_changed()
{
    update_location_icon();
}

void LocationEdit::update_text_margins()
{
    setTextMargins(m_text_leading_margin, 0, trailing_text_margin(), 0);
}

int LocationEdit::trailing_text_margin() const
{
    auto margin = LOCATION_TRAILING_EDGE_MARGIN + LOCATION_TRAILING_ACTION_WIDTH + LOCATION_TRAILING_ITEM_GAP - LOCATION_TEXT_PADDING;
    if (m_completion_title && !m_completion_title->isHidden())
        margin = max(margin, LOCATION_TRAILING_EDGE_MARGIN + m_completion_title->width() + LOCATION_TRAILING_ITEM_GAP);

    if (m_zoom_indicator_button && !m_zoom_indicator_button->isHidden())
        margin += m_zoom_indicator_button->width() + LOCATION_ZOOM_CHIP_GAP;
    if (m_location_in_use_button && !m_location_in_use_button->isHidden())
        margin += m_location_in_use_button->width() + LOCATION_ZOOM_CHIP_GAP;

    return margin;
}

void LocationEdit::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    // Match the popup's palette without feeding our styled background into the next style update.
    setStyleSheet(ChromeStyle::location_edit_style_sheet(Theme::chrome_palette(*this)));
    m_is_updating_chrome_style = false;
    update_zoom_indicator();
    update_location_in_use_chip();
}

void LocationEdit::schedule_chrome_style_update()
{
    if (m_has_pending_chrome_style_update)
        return;

    m_has_pending_chrome_style_update = true;
    QTimer::singleShot(0, this, [this] {
        update_chrome_style();
        m_autocomplete->schedule_chrome_style_update();
        update_focus_ring(m_focus_ring_opacity);
        update_location_icon();
        update_zoom_indicator();
        highlight_location();
        update();
        m_has_pending_chrome_style_update = false;
    });
}

// Before the address, the field shows a button for the site's information, or a warning when the connection is not
// secure. While the address is being edited, nothing comes before the text.
void LocationEdit::update_location_icon()
{
    if (!m_leading_icon_button)
        return;

    auto update_indicator_style = [this](bool not_secure) {
        if (m_leading_icon_button->property("notSecure").toBool() == not_secure)
            return;

        m_leading_icon_button->setProperty("notSecure", not_secure);
        m_leading_icon_button->style()->unpolish(m_leading_icon_button);
        m_leading_icon_button->style()->polish(m_leading_icon_button);
    };

    auto show_indicator = [&](int width) {
        m_leading_icon_button->show();
        auto inset = leading_indicator_inset(*this, *m_leading_icon_button);
        m_leading_icon_button->move(inset, (height() - m_leading_icon_button->height()) / 2);
        m_text_leading_margin = inset + width + LOCATION_LEADING_ITEM_GAP - LOCATION_TEXT_PADDING;
        update_text_margins();
    };

    m_shows_site_settings_button = false;

    auto hide_indicator = [&] {
        update_indicator_style(false);
        m_leading_icon_button->hide();
        m_leading_icon_button->setText({});
        m_leading_icon_button->setIcon({});
        m_leading_icon_button->setToolTip({});
        m_text_leading_margin = 0;
        update_text_margins();
    };

    auto show_site_settings_button = [&] {
        update_indicator_style(false);
        m_leading_icon_button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_leading_icon_button->setText({});
        m_leading_icon_button->setIcon(create_site_settings_icon(Theme::chrome_palette(*this), m_site_settings_knob_travel));
        m_shows_site_settings_button = true;
        m_leading_icon_button->setIconSize({ 16, 16 });
        m_leading_icon_button->setFixedSize(LOCATION_SITE_SETTINGS_BUTTON_WIDTH, LOCATION_PILL_HEIGHT);
        m_leading_icon_button->setToolTip("Site settings");
        show_indicator(LOCATION_SITE_SETTINGS_BUTTON_WIDTH);
    };

    auto show_not_secure_indicator = [&] {
        update_indicator_style(true);
        m_leading_icon_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_leading_icon_button->setIcon(create_chrome_icon(ChromeIcon::Warning, Theme::chrome(*this).warning_ink));
        m_leading_icon_button->setIconSize({ 14, 14 });
        m_leading_icon_button->setText("Not secure");

        auto font = m_leading_icon_button->font();
        font.setPixelSize(12);
        font.setWeight(QFont::Medium);
        auto width = QFontMetrics(font).horizontalAdvance("Not secure") + 14 + 5 + LOCATION_PILL_HORIZONTAL_PADDING;
        m_leading_icon_button->setFixedSize(width, LOCATION_PILL_HEIGHT);
        m_leading_icon_button->setToolTip("The connection to this site is not secure");
        show_indicator(width);
    };

    auto is_showing_current_url_for_display = !hasFocus()
        && m_url.has_value()
        && text() == display_url();

    if (is_showing_current_url_for_display) {
        auto scheme = m_url->scheme();
        if (scheme == "http"sv)
            show_not_secure_indicator();
        else if (scheme == "https"sv)
            show_site_settings_button();
        else
            hide_indicator();
        return;
    }

    hide_indicator();
}

void LocationEdit::update_zoom_indicator()
{
    if (!m_zoom_indicator_button)
        return;

    // The chip fades in when the page is zoomed and out when the zoom is reset. Its number changes in place.
    auto fade_to = [this](qreal opacity) {
        if (SystemAppearance::prefers_reduced_motion()) {
            m_zoom_indicator_fade->stop();
            m_zoom_indicator_opacity->setOpacity(opacity);
            if (opacity == 0) {
                m_zoom_indicator_button->hide();
                update_text_margins();
                update_trailing_item_positions();
            }
            return;
        }
        m_zoom_indicator_fade->stop();
        m_zoom_indicator_fade->setEasingCurve(opacity > 0 ? Motion::enter_easing() : Motion::exit_easing());
        m_zoom_indicator_fade->setStartValue(m_zoom_indicator_opacity->opacity());
        m_zoom_indicator_fade->setEndValue(opacity);
        m_zoom_indicator_fade->start();
    };

    auto visible = m_zoom_action && m_zoom_action->isVisible() && !m_zoom_action->text().isEmpty();
    if (!visible) {
        if (!m_zoom_indicator_button->isHidden())
            fade_to(0);
        return;
    }

    m_zoom_indicator_button->setText(m_zoom_action->text());
    m_zoom_indicator_button->setToolTip(m_zoom_action->toolTip());

    auto width = max(LOCATION_ZOOM_CHIP_MIN_WIDTH, m_zoom_indicator_button->fontMetrics().horizontalAdvance(m_zoom_indicator_button->text()) + LOCATION_PILL_HORIZONTAL_PADDING);
    m_zoom_indicator_button->setFixedSize(width, LOCATION_PILL_HEIGHT);
    if (m_zoom_indicator_button->isHidden() || m_zoom_indicator_fade->endValue().toReal() == 0) {
        if (m_zoom_indicator_button->isHidden())
            m_zoom_indicator_opacity->setOpacity(0);
        m_zoom_indicator_button->show();
        fade_to(1);
    }

    update_text_margins();
    update_trailing_item_positions();
}

void LocationEdit::set_location_in_use(bool in_use)
{
    if (in_use == !m_location_in_use_button->isHidden())
        return;

    if (in_use) {
        m_location_in_use_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        update_location_in_use_chip();
        m_location_in_use_button->show();
        m_location_in_use_collapse_timer->start();
    } else {
        m_location_in_use_collapse_timer->stop();
        m_location_in_use_button->hide();
    }

    update_text_margins();
    update_trailing_item_positions();
}

void LocationEdit::update_location_in_use_chip()
{
    if (!m_location_in_use_button)
        return;

    m_location_in_use_button->setIcon(create_chrome_icon(ChromeIcon::Location, Theme::chrome_palette(*this)));
    if (m_location_in_use_button->toolButtonStyle() == Qt::ToolButtonIconOnly) {
        m_location_in_use_button->setFixedWidth(LOCATION_IN_USE_CHIP_ICON_ONLY_WIDTH);
        return;
    }
    auto width = m_location_in_use_button->fontMetrics().horizontalAdvance(m_location_in_use_button->text()) + 16 + 4 + LOCATION_PILL_HORIZONTAL_PADDING;
    m_location_in_use_button->setFixedWidth(width);
}

void LocationEdit::highlight_location()
{
    auto url = ak_string_from_qstring(text());
    QList<QInputMethodEvent::Attribute> attributes;

    // The parts of the URL around the domain are secondary, but should not look disabled. In an inactive window, the
    // whole address drops to the muted color.
    auto is_inactive = ChromeStyle::window_activity(*this) == ChromeStyle::WindowActivity::Inactive;
    auto text_color = is_inactive ? Theme::chrome(*this).muted : ChromeStyle::chrome_text(Theme::chrome_palette(*this));
    auto darkened_text_color = text_color;
    if (!is_inactive)
        darkened_text_color.setAlpha(ChromeStyle::is_dark(Theme::chrome_palette(*this)) ? 178 : 148);

    QTextCharFormat dark_attributes;
    dark_attributes.setForeground(darkened_text_color);

    QTextCharFormat highlight_attributes;
    highlight_attributes.setForeground(text_color);

    auto append_attributes = [&](StringView scheme_and_subdomain, StringView effective_tld_plus_one, StringView remainder) {
        attributes.append({
            QInputMethodEvent::TextFormat,
            -cursorPosition(),
            static_cast<int>(scheme_and_subdomain.length()),
            dark_attributes,
        });

        attributes.append({
            QInputMethodEvent::TextFormat,
            static_cast<int>(scheme_and_subdomain.length() - cursorPosition()),
            static_cast<int>(effective_tld_plus_one.length()),
            highlight_attributes,
        });

        attributes.append({
            QInputMethodEvent::TextFormat,
            static_cast<int>(scheme_and_subdomain.length() + effective_tld_plus_one.length() - cursorPosition()),
            static_cast<int>(remainder.length()),
            dark_attributes,
        });
    };

    if (m_url.has_value() && text() == display_url() && m_url->scheme().is_one_of("http"sv, "https"sv)) {
        auto serialized_url = m_url->serialize();
        if (auto url_parts = WebView::break_url_into_parts(serialized_url); url_parts.has_value()) {
            auto scheme_and_subdomain = url_parts->scheme_and_subdomain;
            auto remainder = url_parts->remainder;

            auto scheme_prefix_length = m_url->scheme().length() + "://"sv.length();
            if (scheme_and_subdomain.length() >= scheme_prefix_length)
                scheme_and_subdomain = scheme_and_subdomain.substring_view(scheme_prefix_length);
            if (scheme_and_subdomain.starts_with("www."sv, CaseSensitivity::CaseInsensitive))
                scheme_and_subdomain = scheme_and_subdomain.substring_view(4);
            if (remainder == "/"sv)
                remainder = {};

            append_attributes(scheme_and_subdomain, url_parts->effective_tld_plus_one, remainder);
        }
    } else if (auto url_parts = WebView::break_url_into_parts(url); url_parts.has_value()) {
        append_attributes(url_parts->scheme_and_subdomain, url_parts->effective_tld_plus_one, url_parts->remainder);
    }

    QInputMethodEvent event(QString(), attributes);
    QCoreApplication::sendEvent(this, &event);
}

// Programmatic text changes bypass textEdited, so the editing session must restart for the model to act
// on what the bar now shows.
void LocationEdit::set_text_and_restart_editing(QString const& text)
{
    setText(text);
    // The parked cursor and selection belonged to the text that was just replaced.
    if (m_parked_editing_session.has_value())
        m_parked_editing_session = ParkedEditingSession {};
    if (!m_omnibox.is_editing())
        return;

    // Without focus, the session is alive because a popup (for example the context menu) borrowed input;
    // the restarted session must stay suspended behind it.
    bool suspended = !hasFocus();
    m_omnibox.begin_editing(ak_string_from_qstring(text));
    m_omnibox.set_suspended(suspended);
}

void LocationEdit::set_url(Optional<URL::URL> url)
{
    m_url = AK::move(url);

    if (m_url_is_hidden) {
        if (!m_has_user_edited_hidden_url)
            set_text_and_restart_editing({});
    } else if (m_url.has_value()) {
        // NB: A parked editing session still owns the omnibox, so it gets the text a focused omnibox would.
        auto is_editing = hasFocus() || m_parked_editing_session.has_value();
        set_text_and_restart_editing(is_editing ? serialized_url() : display_url());
        setCursorPosition(0);
    }

    update_location_icon();
}

void LocationEdit::show_full_url_preserving_display_selection()
{
    if (!m_should_show_full_url_on_mouse_release)
        return;

    m_should_show_full_url_on_mouse_release = false;

    if (!m_url.has_value() || text() != display_url())
        return;

    auto selection_start = selectionStart();
    auto selection_length = selectedText().length();

    set_text_and_restart_editing(serialized_url());

    if (selection_start != -1) {
        auto serialized_selection_start = serialized_url_position_for_display_position(selection_start);
        auto serialized_selection_end = serialized_url_position_for_display_position(selection_start + selection_length);
        setSelection(serialized_selection_start, serialized_selection_end - serialized_selection_start);
    } else {
        selectAll();
    }

    highlight_location();
}

int LocationEdit::serialized_url_position_for_display_position(int display_position) const
{
    VERIFY(m_url.has_value());

    auto display = display_url();
    auto serialized = serialized_url();
    display_position = qBound(0, display_position, display.length());

    if (display == serialized || !m_url->scheme().is_one_of("http"sv, "https"sv))
        return min(display_position, serialized.length());

    int display_index = 0;
    int last_serialized_position = 0;
    auto map_visible_range = [&](int serialized_start, int length) -> Optional<int> {
        if (length <= 0)
            return {};

        if (display_position < display_index + length)
            return serialized_start + display_position - display_index;

        display_index += length;
        last_serialized_position = serialized_start + length;
        return {};
    };

    auto serialized_index = qstring_from_ak_string(m_url->scheme()).length() + "://"sv.length();

    if (!m_url->username().is_empty() || !m_url->password().is_empty()) {
        auto username = qstring_from_ak_string(m_url->username());
        auto password = qstring_from_ak_string(m_url->password());
        auto userinfo_length = username.length() + 1;
        if (!password.isEmpty())
            userinfo_length += 1 + password.length();

        if (auto position = map_visible_range(serialized_index, userinfo_length); position.has_value())
            return *position;
        serialized_index += userinfo_length;
    }

    auto host = qstring_from_ak_string(m_url->serialized_host());
    auto host_offset = host.startsWith("www.", Qt::CaseInsensitive) ? 4 : 0;
    if (auto position = map_visible_range(serialized_index + host_offset, host.length() - host_offset); position.has_value())
        return *position;
    serialized_index += host.length();

    if (auto port = m_url->port(); port.has_value()) {
        auto port_text = QString::number(*port);
        auto port_length = 1 + port_text.length();
        if (auto position = map_visible_range(serialized_index, port_length); position.has_value())
            return *position;
        serialized_index += port_length;
    }

    auto path = qstring_from_ak_string(m_url->serialize_path());
    if (path != "/" || m_url->query().has_value() || m_url->fragment().has_value()) {
        if (auto position = map_visible_range(serialized_index, path.length()); position.has_value())
            return *position;
    }
    serialized_index += path.length();

    if (m_url->query().has_value()) {
        auto query_length = 1 + qstring_from_ak_string(*m_url->query()).length();
        if (auto position = map_visible_range(serialized_index, query_length); position.has_value())
            return *position;
        serialized_index += query_length;
    }

    if (m_url->fragment().has_value()) {
        auto fragment_length = 1 + qstring_from_ak_string(*m_url->fragment()).length();
        if (auto position = map_visible_range(serialized_index, fragment_length); position.has_value())
            return *position;
    }

    return last_serialized_position;
}

bool LocationEdit::text_matches_current_url() const
{
    return m_url.has_value()
        && !m_url_is_hidden
        && (text() == serialized_url() || text() == display_url());
}

QString LocationEdit::serialized_url() const
{
    VERIFY(m_url.has_value());
    return qstring_from_ak_string(m_url->serialize());
}

QString LocationEdit::display_url() const
{
    VERIFY(m_url.has_value());
    return qstring_from_ak_string(WebView::url_for_display(*m_url));
}

QString LocationEdit::current_query() const
{
    if (m_omnibox.is_editing())
        return qstring_from_ak_string(m_omnibox.query());
    return text();
}

}
