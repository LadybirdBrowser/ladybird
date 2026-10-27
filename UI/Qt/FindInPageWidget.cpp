/*
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/FindInPageWidget.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/NativeTextEditingLineEdit.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Tab.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/Theme.h>
#include <UI/Qt/WebContentView.h>

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QVariantAnimation>
#include <QWindow>

namespace Ladybird {

static constexpr auto NO_MATCHES_PROPERTY = "noResults";

static void set_dynamic_property_if_needed(QWidget& widget, char const* property, bool value)
{
    if (widget.property(property).toBool() == value)
        return;

    widget.setProperty(property, value);
    widget.style()->unpolish(&widget);
    widget.style()->polish(&widget);
    widget.update();
}

// The bar floats over the top right corner of the page. It is a native child of the tab, so it stacks above the native
// view the page is presented in, while the keyboard stays within the browser window.
static constexpr int BAR_WIDTH = 430;
static constexpr int BAR_HEIGHT = 40;
static constexpr int BAR_CORNER_RADIUS = 10;
static constexpr int PAGE_EDGE_INSET = 16;
static constexpr int PAGE_TOP_INSET = 8;

FindInPageWidget::FindInPageWidget(Tab* tab, WebContentView* content_view)
    : QWidget(static_cast<QWidget*>(tab))
    , m_tab(tab)
    , m_content_view(content_view)
{
    setObjectName("LadybirdFindInPageBar");
    setAccessibleName("Find in page");
#if defined(AK_OS_MACOS)
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
#endif
    m_shadow = &cast_floating_shadow(*this, BAR_CORNER_RADIUS);
    setFocusPolicy(Qt::FocusPolicy::StrongFocus);
    setFixedSize(BAR_WIDTH, BAR_HEIGHT);
    update_chrome_style();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 5, 6, 5);
    layout->setSpacing(4);

    // The search field holds its icon and the count of matches.
    auto* field = new QWidget(this);
    m_field = field;
    field->setObjectName("LadybirdFindInPageField");
    field->setAttribute(Qt::WA_StyledBackground);
    auto* field_layout = new QHBoxLayout(field);
    field_layout->setContentsMargins(8, 0, 8, 0);
    field_layout->setSpacing(8);
    m_search_icon = new QLabel(field);
    m_search_icon->setFixedSize(14, 14);
    field_layout->addWidget(m_search_icon);

    m_find_text = new NativeTextEditingLineEdit(field);
    m_find_text->setObjectName("LadybirdFindInPageText");
    m_find_text->setFocusPolicy(Qt::FocusPolicy::StrongFocus);
    m_find_text->setPlaceholderText("Find in page");
    m_find_text->setAccessibleName("Find in page");
    connect(m_find_text, &QLineEdit::textChanged, this, &FindInPageWidget::find_text_changed);
    field_layout->addWidget(m_find_text, 1);

    m_result_label = new QLabel(field);
    m_result_label->setObjectName("LadybirdFindInPageResult");
    m_result_label->setVisible(false);
    field_layout->addWidget(m_result_label);
    layout->addWidget(field, 1);

    auto make_button = [&](QString const& tooltip) {
        auto* button = new QPushButton(this);
        button->setObjectName("LadybirdFindInPageButton");
        button->setFixedSize(28, 28);
        button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
        button->setIconSize({ 16, 16 });
        button->setToolTip(tooltip);
        button->setAccessibleName(tooltip);
        button->setFlat(true);
        button->setFocusPolicy(Qt::TabFocus);
        return button;
    };

    m_previous_button = make_button("Previous match");
    connect(m_previous_button, &QPushButton::clicked, this, [this] {
        m_content_view->find_in_page_previous_match();
    });

    m_next_button = make_button("Next match");
    connect(m_next_button, &QPushButton::clicked, this, [this] {
        m_content_view->find_in_page_next_match();
    });

    m_match_case = make_button("Match case");
    m_match_case->setText("Aa");
    m_match_case->setCheckable(true);
    connect(m_match_case, &QPushButton::toggled, this, [this] {
        find_text_changed();
    });

    auto* separator = new QFrame(this);
    separator->setObjectName("LadybirdFindInPageSeparator");
    separator->setFixedSize(1, 16);

    m_exit_button = make_button("Close find");
    m_exit_button->setIconSize({ 12, 12 });
    connect(m_exit_button, &QPushButton::clicked, this, [this] {
        close_bar();
    });

    layout->addWidget(m_previous_button);
    layout->addWidget(m_next_button);
    layout->addWidget(m_match_case);
    layout->addSpacing(2);
    layout->addWidget(separator, 0, Qt::AlignVCenter);
    layout->addSpacing(2);
    layout->addWidget(m_exit_button);

    update_icons();
}

void FindInPageWidget::update_icons()
{
    auto const& panel = Theme::panel();
    m_search_icon->setPixmap(create_chrome_icon(ChromeIcon::Search, panel.muted).pixmap(QSize(14, 14), devicePixelRatioF()));
    auto navigation_color = m_has_no_matches ? ChromeStyle::find_in_page_unavailable_control_color() : panel.body;
    m_previous_button->setIcon(create_chrome_icon(ChromeIcon::ChevronUp, navigation_color));
    m_next_button->setIcon(create_chrome_icon(ChromeIcon::ChevronDown, navigation_color));
    m_exit_button->setIcon(create_chrome_icon(ChromeIcon::Close, panel.body));
}

void FindInPageWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    auto const& panel = Theme::panel();
#if defined(AK_OS_MACOS)
    // NB: The view's own layer rounds the corners and draws the outline.
    painter.fillRect(rect(), panel.surface);
#else
    painter.setBrush(panel.surface);
    painter.setPen(QPen(panel.edge, 1));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), BAR_CORNER_RADIUS, BAR_CORNER_RADIUS);
#endif
}

void FindInPageWidget::focus_search_field()
{
    setFocus(Qt::ShortcutFocusReason);
}

void FindInPageWidget::reposition()
{
    auto view_top_right = m_content_view->mapTo(m_tab, QPoint(m_content_view->width(), 0));
    auto position = QPoint(view_top_right.x() - PAGE_EDGE_INSET - BAR_WIDTH, view_top_right.y() + PAGE_TOP_INSET + qRound(m_opening_offset));
    if (pos() != position)
        move(position);
}

FindInPageWidget::~FindInPageWidget() = default;

bool FindInPageWidget::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange) {
        update_chrome_style();
        update_icons();
    }

    return QWidget::event(event);
}

void FindInPageWidget::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::find_in_page_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

void FindInPageWidget::find_text_changed()
{
    auto query = utf16_string_from_qstring(m_find_text->text());
    if (query.is_empty())
        set_has_no_matches(false);

    auto case_sensitive = m_match_case->isChecked() ? CaseSensitivity::CaseSensitive : CaseSensitivity::CaseInsensitive;
    // Every match on the page is highlighted, with the current one standing out.
    m_content_view->find_in_page(query, case_sensitive, true);
}

void FindInPageWidget::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        close_bar();
        break;
    case Qt::Key_Return:
        if (event->modifiers().testFlag(Qt::ShiftModifier))
            m_previous_button->click();
        else
            m_next_button->click();
        break;
    default:
        event->ignore();
        break;
    }
}

void FindInPageWidget::close_bar()
{
    ++m_selected_text_request_id;
    m_content_view->find_in_page_end();
    setVisible(false);
    m_content_view->setFocus();
}

void FindInPageWidget::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    m_find_text->setFocus();
    auto find_text_before_request = m_find_text->text();
    auto request_id = ++m_selected_text_request_id;
    m_content_view->selected_text()->when_resolved([guarded_this = QPointer<FindInPageWidget> { this }, request_id, find_text_before_request = AK::move(find_text_before_request)](auto& selected_text) {
        if (!guarded_this || request_id != guarded_this->m_selected_text_request_id || guarded_this->m_find_text->text() != find_text_before_request)
            return;
        if (!selected_text.is_empty()) {
            guarded_this->m_find_text->setText(qstring_from_ak_string(selected_text));
            if (guarded_this->m_find_text->text() == find_text_before_request)
                guarded_this->find_text_changed();
        }
        guarded_this->m_find_text->selectAll();
    });
    m_find_text->selectAll();
}

void FindInPageWidget::showEvent(QShowEvent*)
{
    // The bar fades in from 4 px above its place, or fades in only if the system asks for less motion.
    auto set_opacity = [this](qreal opacity) {
        set_floating_window_opacity(*this, opacity);
        set_floating_window_opacity(*m_shadow, opacity);
    };
    if (!m_opening_animation) {
        m_opening_animation = new QVariantAnimation(this);
        m_opening_animation->setDuration(Motion::FAST);
        m_opening_animation->setEasingCurve(Motion::enter_easing());
        m_opening_animation->setStartValue(0.0);
        m_opening_animation->setEndValue(1.0);
        QObject::connect(m_opening_animation, &QVariantAnimation::valueChanged, this, [this, set_opacity](QVariant const& value) {
            auto progress = value.toReal();
            m_opening_offset = SystemAppearance::prefers_reduced_motion() ? 0 : -4 * (1 - progress);
            set_opacity(progress);
            reposition();
        });
    }
    m_opening_offset = SystemAppearance::prefers_reduced_motion() ? 0 : -4;
    set_opacity(0);
    m_opening_animation->stop();
    m_opening_animation->start();
    reposition();
    raise();
#if defined(AK_OS_MACOS)
    round_appkit_view_corners(*this, BAR_CORNER_RADIUS, Theme::panel().edge);
#endif

    if (m_tab && m_tab->isVisible())
        m_tab->update_hover_label();
}

void FindInPageWidget::hideEvent(QHideEvent*)
{
    if (m_tab && m_tab->isVisible())
        m_tab->update_hover_label();
}

void FindInPageWidget::update_result_label(size_t current_match_index, Optional<size_t> const& total_match_count)
{
    if (total_match_count.has_value()) {
        set_has_no_matches(total_match_count.value() == 0);

        auto label_text = "No matches"_string;
        if (total_match_count.value() > 0)
            label_text = MUST(String::formatted("{} of {}", current_match_index + 1, total_match_count.value()));

        m_result_label->setText(qstring_from_ak_string(label_text));
        m_result_label->setVisible(true);
    } else {
        set_has_no_matches(false);
        m_result_label->setVisible(false);
    }
}

// Without a match, the field is tinted, and there is no match to go to.
void FindInPageWidget::set_has_no_matches(bool has_no_matches)
{
    if (m_has_no_matches == has_no_matches)
        return;
    m_has_no_matches = has_no_matches;
    set_dynamic_property_if_needed(*m_field, NO_MATCHES_PROPERTY, has_no_matches);
    set_dynamic_property_if_needed(*m_result_label, NO_MATCHES_PROPERTY, has_no_matches);
    m_previous_button->setEnabled(!has_no_matches);
    m_next_button->setEnabled(!has_no_matches);
    update_icons();
}

void FindInPageWidget::find_previous()
{
    m_previous_button->click();
}

void FindInPageWidget::find_next()
{
    m_next_button->click();
}

}
