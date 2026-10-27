/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/AllTabsPopover.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Theme.h>

#include <QAction>
#include <QFontMetricsF>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QtMath>

namespace Ladybird {

static constexpr int POPOVER_WIDTH = 384;
static constexpr int ROW_HEIGHT = 30;
static constexpr int ROW_RADIUS = 6;
static constexpr int ROW_HORIZONTAL_PADDING = 10;
static constexpr int ROW_ICON_SIZE = 16;
static constexpr int ROW_GAP = 10;
static constexpr int LIST_MAX_HEIGHT = 420;
// A list that scrolls fades out over the bottom of its visible part.
static constexpr int LIST_FADE_HEIGHT = 50;

// Rows of tabs, each with its favicon, title and site. The current tab's row is filled and its title bold.
class AllTabsList final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    // The open tabs keep a row highlighted for the keyboard, while the recently closed ones only follow the pointer.
    enum class Highlight : u8 {
        Keyboard,
        Pointer,
    };

    AllTabsList(Highlight highlight, QWidget* parent)
        : QWidget(parent)
        , m_highlight(highlight)
    {
        setMouseTracking(true);
    }

    Function<void(size_t)> on_activate;

    void set_entries(Vector<AllTabsPopover::Entry> entries)
    {
        m_entries = AK::move(entries);
        filter({});
    }

    void filter(QString const& query)
    {
        m_rows.clear();
        for (size_t index = 0; index < m_entries.size(); ++index) {
            auto const& entry = m_entries[index];
            if (query.isEmpty() || entry.title.contains(query, Qt::CaseInsensitive) || entry.detail.contains(query, Qt::CaseInsensitive))
                m_rows.append(index);
        }

        // The current tab starts out highlighted, and otherwise the first match.
        m_highlighted_row = m_rows.is_empty() || m_highlight == Highlight::Pointer ? -1 : 0;
        if (query.isEmpty()) {
            for (size_t row = 0; row < m_rows.size(); ++row) {
                if (m_entries[m_rows[row]].is_current)
                    m_highlighted_row = static_cast<int>(row);
            }
        }
        setFixedSize(POPOVER_WIDTH - 12, static_cast<int>(m_rows.size()) * ROW_HEIGHT);
        update();
    }

    int highlighted_row() const { return m_highlighted_row; }
    QRect row_rect(int row) const { return { 0, row * ROW_HEIGHT, width(), ROW_HEIGHT }; }
    size_t row_count() const { return m_rows.size(); }

    void move_highlight(int delta)
    {
        if (m_rows.is_empty())
            return;
        m_highlighted_row = clamp(m_highlighted_row + delta, 0, static_cast<int>(m_rows.size()) - 1);
        update();
    }

    void activate_highlighted_row()
    {
        if (m_highlighted_row >= 0 && on_activate)
            on_activate(m_rows[m_highlighted_row]);
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto const& panel = Theme::panel();

        auto title_font = font();
        auto detail_font = font();
        detail_font.setPixelSize(12);
        QFontMetricsF detail_metrics(detail_font);

        for (size_t row = 0; row < m_rows.size(); ++row) {
            auto const& entry = m_entries[m_rows[row]];
            auto rect = row_rect(static_cast<int>(row));

            if (entry.is_current || static_cast<int>(row) == m_highlighted_row) {
                QPainterPath path;
                path.addRoundedRect(QRectF(rect), ROW_RADIUS, ROW_RADIUS);
                painter.fillPath(path, static_cast<int>(row) == m_highlighted_row && !entry.is_current ? panel.pressed : panel.control);
            }

            auto x = rect.left() + ROW_HORIZONTAL_PADDING;
            QRect icon_rect { x, rect.center().y() - (ROW_ICON_SIZE / 2) + 1, ROW_ICON_SIZE, ROW_ICON_SIZE };
            auto icon = entry.icon.isNull() ? create_chrome_icon(ChromeIcon::Globe, panel.muted) : entry.icon;
            icon.paint(&painter, icon_rect);
            x += ROW_ICON_SIZE + ROW_GAP;

            auto right = rect.right() + 1 - ROW_HORIZONTAL_PADDING;
            auto detail_width = min(qCeil(detail_metrics.horizontalAdvance(entry.detail)), (right - x) / 2);
            painter.setFont(detail_font);
            painter.setPen(panel.muted);
            painter.drawText(QRect(right - detail_width, rect.top(), detail_width, rect.height()), Qt::AlignVCenter | Qt::AlignRight, detail_metrics.elidedText(entry.detail, Qt::ElideMiddle, detail_width));

            painter.setFont(title_font);
            painter.setPen(panel.text);
            auto title_width = right - detail_width - ROW_GAP - x;
            painter.drawText(QRect(x, rect.top(), title_width, rect.height()), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(title_font).elidedText(entry.title, Qt::ElideRight, title_width));
        }

        // More rows below the visible part of the list fade in from under it.
        auto visible = visibleRegion().boundingRect();
        if (visible.bottom() < height() - 1) {
            auto fade_top = visible.bottom() + 1 - LIST_FADE_HEIGHT;
            QLinearGradient fade(0, fade_top, 0, visible.bottom() + 1);
            auto transparent_surface = panel.surface;
            transparent_surface.setAlpha(0);
            fade.setColorAt(0.0, transparent_surface);
            fade.setColorAt(1.0, panel.surface);
            painter.fillRect(QRect(0, fade_top, width(), LIST_FADE_HEIGHT), fade);
        }
    }

    virtual void mouseMoveEvent(QMouseEvent* event) override
    {
        auto row = event->position().toPoint().y() / ROW_HEIGHT;
        if (row >= 0 && row < static_cast<int>(m_rows.size()) && row != m_highlighted_row) {
            m_highlighted_row = row;
            update();
        }
    }

    virtual void leaveEvent(QEvent*) override
    {
        if (m_highlight == Highlight::Pointer && m_highlighted_row >= 0) {
            m_highlighted_row = -1;
            update();
        }
    }

    virtual void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !rect().contains(event->position().toPoint()))
            return;
        m_highlighted_row = event->position().toPoint().y() / ROW_HEIGHT;
        activate_highlighted_row();
    }

    Highlight m_highlight { Highlight::Keyboard };
    Vector<AllTabsPopover::Entry> m_entries;
    Vector<size_t> m_rows;
    int m_highlighted_row { -1 };
};

AllTabsPopover::AllTabsPopover(QWidget* parent)
    : Popover(parent)
{
    setAccessibleName("All tabs");
    card().setObjectName("LadybirdAllTabsPopover");
    card().setFixedWidth(POPOVER_WIDTH);

    auto* layout = new QVBoxLayout(&card());
    layout->setContentsMargins(6, 8, 6, 6);
    layout->setSpacing(0);

    m_search_field = new QLineEdit(&card());
    m_search_field->setObjectName("LadybirdAllTabsSearch");
    m_search_field->setAccessibleName("Search tabs");
    m_search_field->addAction(create_chrome_icon(ChromeIcon::Search, Theme::panel().muted), QLineEdit::LeadingPosition);
    m_search_field->installEventFilter(this);
    QObject::connect(m_search_field, &QLineEdit::textChanged, this, [this] { update_search(); });
    auto* search_row = new QHBoxLayout;
    search_row->setContentsMargins(4, 0, 4, 0);
    search_row->addWidget(m_search_field);
    layout->addLayout(search_row);
    layout->addSpacing(8);

    m_tab_list = new AllTabsList(AllTabsList::Highlight::Keyboard, &card());
    m_tab_list->on_activate = [this](size_t index) {
        close();
        if (on_tab_activated)
            on_tab_activated(index);
    };

    m_scroll_area = new QScrollArea(&card());
    m_scroll_area->setObjectName("LadybirdAllTabsList");
    m_scroll_area->setFrameShape(QFrame::NoFrame);
    m_scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll_area->setWidget(m_tab_list);
    // NB: QScrollArea fills the background of the widget it scrolls, but the list sits on the card.
    m_tab_list->setAutoFillBackground(false);
    m_scroll_area->viewport()->setAutoFillBackground(false);
    // NB: The fade at the bottom of the list moves with the visible part, so scrolling repaints the whole list.
    QObject::connect(m_scroll_area->verticalScrollBar(), &QScrollBar::valueChanged, m_tab_list, qOverload<>(&QWidget::update));
    layout->addWidget(m_scroll_area);

    // The tabs closed most recently, below the open ones.
    m_recently_closed_section = new QWidget(&card());
    auto* section_layout = new QVBoxLayout(m_recently_closed_section);
    section_layout->setContentsMargins(0, 6, 0, 0);
    section_layout->setSpacing(0);
    auto* separator = new QFrame(m_recently_closed_section);
    separator->setObjectName("LadybirdPopoverSeparator");
    separator->setFixedHeight(1);
    auto* separator_row = new QHBoxLayout;
    separator_row->setContentsMargins(10, 0, 10, 0);
    separator_row->addWidget(separator);
    section_layout->addLayout(separator_row);
    section_layout->addSpacing(6);
    auto* heading = new QLabel("Recently closed", m_recently_closed_section);
    heading->setObjectName("LadybirdAllTabsHeading");
    heading->setContentsMargins(10, 4, 10, 4);
    section_layout->addWidget(heading);
    m_recently_closed_list = new AllTabsList(AllTabsList::Highlight::Pointer, m_recently_closed_section);
    m_recently_closed_list->on_activate = [this](size_t index) {
        close();
        if (on_recently_closed_activated)
            on_recently_closed_activated(index);
    };
    section_layout->addWidget(m_recently_closed_list);
    layout->addWidget(m_recently_closed_section);
}

void AllTabsPopover::set_recently_closed(Vector<Entry> entries)
{
    m_recently_closed_list->set_entries(AK::move(entries));
    update_search();
}

void AllTabsPopover::set_tabs(Vector<Entry> entries)
{
    m_search_field->setPlaceholderText(QString("Search %1 tabs").arg(entries.size()));
    m_tab_list->set_entries(AK::move(entries));
    m_search_field->clear();
    update_search();
}

void AllTabsPopover::update_search()
{
    auto query = m_search_field->text().trimmed();
    m_tab_list->filter(query);
    m_scroll_area->setFixedHeight(min(LIST_MAX_HEIGHT, m_tab_list->height()));
    m_recently_closed_list->filter(query);
    m_recently_closed_section->setVisible(m_recently_closed_list->row_count() > 0);
    if (auto row = m_tab_list->highlighted_row(); row >= 0)
        m_scroll_area->ensureVisible(0, m_tab_list->row_rect(row).center().y(), 0, ROW_HEIGHT);
    card().adjustSize();
    adjustSize();
}

void AllTabsPopover::showEvent(QShowEvent* event)
{
    Popover::showEvent(event);
    m_search_field->setFocus();
}

bool AllTabsPopover::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_search_field && event->type() == QEvent::KeyPress) {
        auto key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            m_tab_list->move_highlight(key == Qt::Key_Up ? -1 : 1);
            m_scroll_area->ensureVisible(0, m_tab_list->row_rect(m_tab_list->highlighted_row()).center().y(), 0, ROW_HEIGHT);
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            m_tab_list->activate_highlighted_row();
            return true;
        }
    }
    return Popover::eventFilter(watched, event);
}

void AllTabsPopover::update_chrome_style(QPalette const& palette)
{
    setPalette(palette);
    setStyleSheet(ChromeStyle::all_tabs_popover_style_sheet(palette));
    m_tab_list->update();
    m_recently_closed_list->update();
}

}
