/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/kmalloc.h>
#include <LibURL/InternalURLs.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/ProcessSnapshot.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/ProcessManagerWindow.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Theme.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionHeader>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWindow>

#include <errno.h>

namespace Ladybird {

QString task_manager_site_label(URL::URL const& url)
{
    if (url == URL::about_blank())
        return {};
    if (url.host().has_value()) {
        // NB: Pages and request servers are isolated by site, so a site's hosts, such as www.example.com and
        //     example.com, are listed together under its registrable domain.
        if (auto registrable_domain = url.host()->registrable_domain(); registrable_domain.has_value())
            return qstring_from_ak_string(*registrable_domain);
        auto host = qstring_from_ak_string(url.serialized_host());
        if (!host.isEmpty())
            return host;
    }
    if (url.scheme() == "file"sv && !url.basename().is_empty())
        return qstring_from_ak_string(url.basename());
    return qstring_from_ak_string(url.scheme()) + ":";
}

namespace {

enum Column {
    Name,
    CPU,
    Memory,
    PID,
};

enum class RowKind {
    Process,
    Site,
    Group,
    Tab,
};

// Process rows are identified by the process's child ID, which, unlike its PID, never comes to name another process.
// Site rows are identified by their site.
constexpr int ChildIDRole = Qt::UserRole + 1;
constexpr int KindRole = Qt::UserRole + 2;
constexpr int SiteRole = Qt::UserRole + 3;
// What a site row says after the site's name, such as how many tabs show it.
constexpr int NoteRole = Qt::UserRole + 4;

constexpr int TITLE_BAR_HEIGHT = 52;
constexpr int FOOTER_HEIGHT = 52;
constexpr int ROW_HEIGHT = 30;
constexpr qreal SELECTION_RADIUS = 6;
#if defined(AK_OS_MACOS)
// NB: The traffic lights sit in the title bar, and the title comes after them.
constexpr int TITLE_LEADING_MARGIN = 86;
#else
constexpr int TITLE_LEADING_MARGIN = 14;
#endif

RowKind row_kind(QTreeWidgetItem const* item)
{
    return static_cast<RowKind>(item->data(Name, KindRole).toInt());
}

// The column the rows are sorted by is drawn in the text color and weight, with the order beside it.
class ProcessHeader final : public QHeaderView {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ProcessHeader(QWidget* parent)
        : QHeaderView(Qt::Horizontal, parent)
    {
    }

private:
    virtual void paintSection(QPainter* painter, QRect const& rect, int section) const override
    {
        QStyleOptionHeader option;
        initStyleOption(&option);
        initStyleOptionForIndex(&option, section);
        auto text = option.text;
        auto is_sorted = isSortIndicatorShown() && sortIndicatorSection() == section;

        painter->save();
        painter->setClipRect(rect);
        painter->fillRect(rect, Theme::panel().page);
        painter->fillRect(QRect(rect.left(), rect.bottom(), rect.width(), 1), Theme::panel().separator_soft);

        auto font = this->font();
        font.setPixelSize(12);
        font.setWeight(is_sorted ? QFont::DemiBold : QFont::Normal);
        painter->setFont(font);
        painter->setPen(is_sorted ? Theme::panel().text : Theme::panel().muted);

        auto text_rect = rect.adjusted(12, 0, -12, 0);
        auto alignment = section == Name ? Qt::AlignLeft : Qt::AlignRight;
        if (is_sorted) {
            // The order is shown by a small chevron after the name, pointing down for the largest first.
            static constexpr int chevron_size = 10;
            auto text_width = QFontMetrics(font).horizontalAdvance(text);
            auto chevron_left = alignment == Qt::AlignLeft ? text_rect.left() + text_width + 4 : text_rect.right() - chevron_size;
            if (alignment == Qt::AlignRight)
                text_rect.setRight(chevron_left - 4);
            auto center_y = rect.center().y() + 0.5;
            auto direction = sortIndicatorOrder() == Qt::DescendingOrder ? 1.0 : -1.0;
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(Theme::panel().text, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPointF points[] {
                { chevron_left + 2.5, center_y - direction * 2 },
                { chevron_left + 5.0, center_y + direction * 0.5 },
                { chevron_left + 7.5, center_y - direction * 2 },
            };
            painter->drawPolyline(points, 3);
            painter->setPen(Theme::panel().text);
        }
        painter->drawText(text_rect, alignment | Qt::AlignVCenter, QFontMetrics(font).elidedText(text, Qt::ElideRight, text_rect.width()));
        painter->restore();
    }
};

class ProcessTree final : public QTreeWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ProcessTree(QWidget* parent)
        : QTreeWidget(parent)
    {
    }

    Function<void(QPoint const&)> on_context_menu;

private:
    virtual void contextMenuEvent(QContextMenuEvent* event) override
    {
        if (event->reason() == QContextMenuEvent::Keyboard) {
            if (currentItem())
                on_context_menu(visualItemRect(currentItem()).center());
        } else {
            on_context_menu(event->pos());
        }
        event->accept();
    }

    // The selected row is filled across all its columns with rounded ends.
    virtual void drawRow(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        if (selectionModel()->isSelected(index)) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::primary());
            // NB: The row's rectangle leaves out the branch area before it, which the fill covers too.
            QRectF row_rect(0, option.rect.top(), viewport()->width(), option.rect.height() - 1);
            painter->drawRoundedRect(row_rect, SELECTION_RADIUS, SELECTION_RADIUS);
            painter->restore();
        }
        QTreeWidget::drawRow(painter, option, index);
    }

    // The rows draw their own selection, so only the expand indicator of a site is drawn in the branch area. A process
    // always shows its tabs.
    virtual void drawBranches(QPainter* painter, QRect const& rect, QModelIndex const& index) const override
    {
        if (!model()->hasChildren(index) || static_cast<RowKind>(index.siblingAtColumn(Name).data(KindRole).toInt()) != RowKind::Site)
            return;
        auto selected = selectionModel()->isSelected(index);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(selected ? QColor(255, 255, 255, 199) : Theme::panel().muted, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        auto center = QRectF(rect.right() - 16, rect.top(), 12, rect.height()).center();
        if (isExpanded(index)) {
            QPointF points[] { center + QPointF(-3, -1.5), center + QPointF(0, 1.5), center + QPointF(3, -1.5) };
            painter->drawPolyline(points, 3);
        } else {
            QPointF points[] { center + QPointF(-1.5, -3), center + QPointF(1.5, 0), center + QPointF(-1.5, 3) };
            painter->drawPolyline(points, 3);
        }
        painter->restore();
    }
};

class ProcessDelegate final : public QStyledItemDelegate {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ProcessDelegate(QObject* parent)
        : QStyledItemDelegate(parent)
    {
    }

    virtual QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(ROW_HEIGHT);
        return size;
    }

    virtual void paint(QPainter* painter, QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        auto adjusted_option = option;
        auto selected = option.state.testFlag(QStyle::State_Selected);
        // The row's fill is drawn by the tree, so the cell draws none of its own.
        adjusted_option.state &= ~QStyle::State_Selected;
        adjusted_option.state &= ~QStyle::State_HasFocus;
        if (selected) {
            adjusted_option.palette.setColor(QPalette::Text, Qt::white);
            adjusted_option.palette.setColor(QPalette::WindowText, Qt::white);
        }
        initStyleOption(&adjusted_option, index);
        if (selected)
            adjusted_option.palette.setColor(QPalette::Text, Qt::white);
        auto* widget = option.widget;
        auto* style = widget ? widget->style() : QApplication::style();
        if (selected && index.column() != Name)
            adjusted_option.palette.setBrush(QPalette::Text, QColor(255, 255, 255, 199));
        style->drawControl(QStyle::CE_ItemViewItem, &adjusted_option, painter, widget);

        auto note = index.data(NoteRole).toString();
        if (note.isEmpty())
            return;
        auto text_rect = style->subElementRect(QStyle::SE_ItemViewItemText, &adjusted_option, widget);
        auto name_width = QFontMetrics(adjusted_option.font).horizontalAdvance(adjusted_option.text);
        auto note_font = adjusted_option.font;
        note_font.setWeight(QFont::Normal);
        note_font.setPixelSize(12);
        auto note_rect = text_rect.adjusted(name_width + 8, 0, 0, 0);
        if (note_rect.width() <= 0)
            return;
        painter->save();
        painter->setFont(note_font);
        painter->setPen(selected ? QColor(255, 255, 255, 199) : Theme::panel().muted);
        painter->drawText(note_rect, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(note_font).elidedText(note, Qt::ElideRight, note_rect.width()));
        painter->restore();
    }
};

// What a row is sorted by in a column.
QVariant sort_key(QTreeWidgetItem const* item, int column)
{
    if (column == Name)
        return item->text(Name);
    return item->data(column, Qt::UserRole);
}

bool sorts_before(QTreeWidgetItem const* a, QTreeWidgetItem const* b, int column, Qt::SortOrder order)
{
    auto less = [&](QTreeWidgetItem const* left, QTreeWidgetItem const* right) {
        if (column == Name)
            return QString::localeAwareCompare(left->text(Name), right->text(Name)) < 0;
        if (column == CPU)
            return sort_key(left, column).toDouble() < sort_key(right, column).toDouble();
        return sort_key(left, column).toULongLong() < sort_key(right, column).toULongLong();
    };
    return order == Qt::AscendingOrder ? less(a, b) : less(b, a);
}

// A window whose content fills its title bar gets no window moving from the platform there, so dragging the title
// bar's empty parts moves the window, and double-clicking them zooms it, as in the browser window.
class TitleBarMouseFilter final : public QObject {
public:
    TitleBarMouseFilter(QObject* parent, Function<bool()> start_window_move)
        : QObject(parent)
        , m_start_window_move(move(start_window_move))
    {
    }

private:
    virtual bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick)
            return false;
        if (static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton)
            return false;
        auto* window = static_cast<QWidget*>(object)->window();
        if (window->isFullScreen())
            return false;
        if (event->type() == QEvent::MouseButtonDblClick) {
            if (window->isMaximized())
                window->showNormal();
            else
                window->showMaximized();
            return true;
        }
        if (m_start_window_move())
            return true;
        auto* handle = window->windowHandle();
        return handle && handle->startSystemMove();
    }

    Function<bool()> m_start_window_move;
};

}

ProcessManagerWindow::ProcessManagerWindow(WebView::ProcessManager& process_manager)
    : m_process_manager(process_manager)
{
    setWindowFlags(Qt::Window);
#if defined(AK_OS_MACOS)
    // The title bar is the window's own, with the traffic lights, the title and the filter in it.
    setWindowFlag(Qt::ExpandedClientAreaHint);
    setWindowFlag(Qt::NoTitleBarBackgroundHint);
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#endif
    setWindowTitle(tr("Task Manager"));
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(820, 620);
    setMinimumSize(560, 300);

    auto* window_layout = new QVBoxLayout(this);
    window_layout->setContentsMargins(0, 0, 0, 0);
    window_layout->setSpacing(0);
    auto* content = new QWidget(this);
    content->setObjectName("LadybirdTaskManager");
    window_layout->addWidget(content);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_title_bar = new QWidget(content);
    m_title_bar->setObjectName("LadybirdTaskManagerTitleBar");
    m_title_bar->setFixedHeight(TITLE_BAR_HEIGHT);
    auto* title_bar_layout = new QHBoxLayout(m_title_bar);
    title_bar_layout->setContentsMargins(TITLE_LEADING_MARGIN, 0, 12, 0);
    m_title = new QLabel(tr("Task Manager"), m_title_bar);
    m_title->setObjectName("LadybirdTaskManagerTitle");
    title_bar_layout->addWidget(m_title);
    title_bar_layout->addStretch();
    m_search = new QLineEdit(m_title_bar);
    m_search->setObjectName("LadybirdTaskManagerFilter");
    m_search->setPlaceholderText(tr("Search processes"));
    m_search->setAccessibleName(tr("Search processes"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedSize(220, 28);
    title_bar_layout->addWidget(m_search);
    layout->addWidget(m_title_bar);
#if defined(AK_OS_MACOS)
    m_title_bar->installEventFilter(new TitleBarMouseFilter(m_title_bar, [this] { return on_title_bar_drag && on_title_bar_drag(); }));
#endif

    auto* list = new QWidget(content);
    auto* list_layout = new QVBoxLayout(list);
    list_layout->setContentsMargins(8, 0, 8, 6);
    auto* process_tree = new ProcessTree(list);
    process_tree->on_context_menu = [this](auto const& position) { show_context_menu(position); };
    m_processes = process_tree;
    m_processes->setHeader(new ProcessHeader(m_processes));
    m_processes->setHeaderLabels({ tr("Name"), tr("CPU"), tr("Memory"), tr("PID") });
    m_processes->headerItem()->setTextAlignment(Name, Qt::AlignLeft | Qt::AlignVCenter);
    m_processes->setIconSize(QSize(16, 16));
    m_processes->setAccessibleName(tr("Processes"));
    m_processes->setFrameShape(QFrame::NoFrame);
    m_processes->setItemDelegate(new ProcessDelegate(m_processes));
    m_processes->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processes->setIndentation(24);
    m_processes->setRootIsDecorated(true);
    m_processes->setFocusPolicy(Qt::StrongFocus);
    // NB: The rows are sorted within their sections, which the tree's own sorting would mix together.
    m_processes->header()->setSortIndicatorShown(true);
    m_processes->header()->setSectionsClickable(true);
    m_processes->header()->setSortIndicator(Memory, Qt::DescendingOrder);
    m_processes->header()->setFixedHeight(30);
    m_processes->header()->setStretchLastSection(false);
    m_processes->header()->setSectionResizeMode(Name, QHeaderView::Stretch);
    for (auto column : { CPU, Memory, PID }) {
        m_processes->header()->setSectionResizeMode(column, QHeaderView::Interactive);
        m_processes->setColumnWidth(column, column == Memory ? 122 : 102);
        m_processes->headerItem()->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    }
    connect(m_processes->header(), &QHeaderView::sortIndicatorChanged, this, [this] { refresh(); });
    list_layout->addWidget(m_processes);
    layout->addWidget(list, 1);
    connect(m_search, &QLineEdit::textChanged, this, &ProcessManagerWindow::apply_filter);
    connect(m_processes, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (row_kind(item) == RowKind::Site)
            m_expanded_sites.set(ak_string_from_qstring(item->data(Name, SiteRole).toString()));
    });
    connect(m_processes, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (row_kind(item) == RowKind::Process)
            item->setExpanded(true);
        if (row_kind(item) == RowKind::Site)
            m_expanded_sites.remove(ak_string_from_qstring(item->data(Name, SiteRole).toString()));
    });

    m_footer = new QWidget(content);
    m_footer->setObjectName("LadybirdTaskManagerFooter");
    m_footer->setFixedHeight(FOOTER_HEIGHT);
    auto* footer_layout = new QHBoxLayout(m_footer);
    footer_layout->setContentsMargins(20, 0, 12, 0);
    m_summary = new QLabel(m_footer);
    m_summary->setObjectName("LadybirdTaskManagerSummary");
    m_summary->setTextFormat(Qt::RichText);
    m_summary->setToolTip(tr("Total CPU and memory for all Ladybird processes. Updates every second."));
    footer_layout->addWidget(m_summary);
    footer_layout->addStretch();
    m_end_action = new QAction(tr("End Process"), this);
    m_end_action->setEnabled(false);
    connect(m_end_action, &QAction::triggered, this, &ProcessManagerWindow::end_selection);
    m_end_button = new QPushButton(m_end_action->text(), m_footer);
    m_end_button->setObjectName("LadybirdTaskManagerEndButton");
    m_end_button->setAutoDefault(false);
    m_end_button->setEnabled(false);
    m_end_button->setFixedHeight(28);
    m_end_button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    connect(m_end_button, &QPushButton::clicked, m_end_action, &QAction::trigger);
    connect(m_end_action, &QAction::changed, this, [this] {
        m_end_button->setEnabled(m_end_action->isEnabled());
        m_end_button->setText(m_end_action->text());
    });
    footer_layout->addWidget(m_end_button);
    layout->addWidget(m_footer);

    m_copy_pid_action = new QAction(tr("Copy PID"), this);
    m_copy_pid_action->setShortcut(QKeySequence::Copy);
    m_copy_pid_action->setShortcutContext(Qt::WidgetShortcut);
    m_processes->addAction(m_copy_pid_action);
    m_copy_pid_action->setEnabled(false);
    connect(m_copy_pid_action, &QAction::triggered, this, [this] {
        update_actions();
        if (m_copy_pid_action->isEnabled())
            QApplication::clipboard()->setText(m_processes->currentItem()->text(PID));
    });
    connect(m_processes, &QTreeWidget::itemSelectionChanged, this, &ProcessManagerWindow::update_actions);

    auto* close_shortcut = new QShortcut(QKeySequence::Close, this);
    connect(close_shortcut, &QShortcut::activated, this, &QWidget::close);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &ProcessManagerWindow::refresh);
    update_style();
}

bool ProcessManagerWindow::event(QEvent* event)
{
    // NB: Qt sends the show event before the native window appears, so it never shows with its controls unmoved.
    if ((event->type() == QEvent::Show || event->type() == QEvent::WindowTitleChange) && on_title_bar_change)
        on_title_bar_change();
    return QDialog::event(event);
}

void ProcessManagerWindow::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange)
        update_style();
}

void ProcessManagerWindow::update_style()
{
    if (!m_processes || m_is_updating_style)
        return;
    m_is_updating_style = true;

    auto const& panel = Theme::panel();
    auto color = [](QColor const& color) { return ChromeStyle::style_sheet_color(color); };
    auto* content = m_title_bar->parentWidget();
    content->setStyleSheet(qformatted(R"(
QWidget#LadybirdTaskManager {{
    background: {0};
}}
QWidget#LadybirdTaskManagerTitleBar {{
    background: {1};
    border-bottom: 1px solid {2};
}}
QLabel#LadybirdTaskManagerTitle {{
    color: {3};
    font-weight: 600;
}}
QLineEdit#LadybirdTaskManagerFilter {{
    color: {3};
    background: {4};
    border: none;
    border-radius: 7px;
    padding: 0 8px;
    selection-background-color: {5};
}}
QWidget#LadybirdTaskManagerFooter {{
    background: {0};
    border-top: 1px solid {2};
}}
QLabel#LadybirdTaskManagerSummary {{
    color: {6};
}}
QPushButton#LadybirdTaskManagerEndButton {{
    color: {3};
    background: {4};
    border: none;
    border-radius: 7px;
    padding: 0 14px;
}}
QPushButton#LadybirdTaskManagerEndButton:pressed {{
    background: {7};
}}
QPushButton#LadybirdTaskManagerEndButton:disabled {{
    color: {6};
}}
QTreeWidget {{
    background: {0};
    color: {3};
    border: none;
    outline: none;
}}
QTreeWidget::item {{
    padding: 0px 4px;
}}
)",
        color(panel.page), color(Theme::chrome().toolbar), color(panel.separator), color(panel.text), color(panel.control),
        color(Theme::selection()), color(panel.muted), color(panel.pressed)));

    // NB: The tree draws the selected row's fill itself, so the style's highlight under the branch area is left clear.
    auto tree_palette = m_processes->palette();
    tree_palette.setColor(QPalette::Highlight, Qt::transparent);
    m_processes->setPalette(tree_palette);

    auto summary_font = m_summary->font();
    summary_font.setPixelSize(12);
    m_summary->setFont(summary_font);

    m_browser_icon = load_icon_from_uri("resource://icons/48x48/app-browser.png"sv);
    m_document_icon = create_chrome_icon(ChromeIcon::Document, panel.body);
    m_network_icon = create_chrome_icon(ChromeIcon::Network, panel.body);
    m_compositor_icon = create_chrome_icon(ChromeIcon::Compositor, panel.body);
    m_image_icon = create_chrome_icon(ChromeIcon::Image, panel.body);
    m_service_icon = create_chrome_icon(ChromeIcon::WindowRestore, panel.body);
    auto search_icon = create_chrome_icon(ChromeIcon::Search, panel.muted);
    if (m_search_icon)
        m_search_icon->setIcon(search_icon);
    else
        m_search_icon = m_search->addAction(search_icon, QLineEdit::LeadingPosition);
    m_favicons.clear();

    m_is_updating_style = false;
    refresh();
}

void ProcessManagerWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
    m_timer->start();
}

void ProcessManagerWindow::hideEvent(QHideEvent* event)
{
    m_timer->stop();
    QDialog::hideEvent(event);
}

QIcon ProcessManagerWindow::favicon_for(WebView::ProcessPageSnapshot const& page)
{
    if (!page.favicon_hash.has_value())
        return create_chrome_icon(ChromeIcon::Globe, Theme::panel().body);
    if (auto icon = m_favicons.get(*page.favicon_hash); icon.has_value())
        return *icon;
    QIcon icon;
    if (auto png = WebView::favicon_png_for_page(page); png.has_value())
        icon = favicon_with_rounded_corners(icon_from_png(png->bytes(), 16), 16);
    if (icon.isNull())
        icon = create_chrome_icon(ChromeIcon::Globe, Theme::panel().body);
    m_favicons.set(*page.favicon_hash, icon);
    return icon;
}

void ProcessManagerWindow::refresh()
{
    if (m_is_updating_style)
        return;

    auto processes = WebView::process_snapshots(m_process_manager);
    QLocale locale;

    HashMap<u64, WebView::ProcessSnapshot const*> processes_by_child_id;
    for (auto const& process : processes)
        processes_by_child_id.set(process.child_id, &process);

    // Each process is listed with the browser's own processes, with a site, or with the spare processes. A process
    // that works for another, such as a page's image decoder or a worker's media server, is listed with it.
    auto is_spare = [](WebView::ProcessSnapshot const& process) {
        return process.type == WebView::ProcessType::WebContent && process.title.has_value() && *process.title == "(spare)"_utf16;
    };
    // NB: A page's process, a worker's process, and a request server bound to a site, belong to that site.
    auto site_of = [](WebView::ProcessSnapshot const& process) -> QString {
        if (!first_is_one_of(process.type, WebView::ProcessType::WebContent, WebView::ProcessType::WebWorker, WebView::ProcessType::RequestServer) || !process.site_url.has_value())
            return {};
        return task_manager_site_label(*process.site_url);
    };
    // NB: A process that works for another may itself work for a third, as a worker's image decoder works for the
    //     worker. A worker with its own site stays with that site; an opaque-origin worker follows its owner.
    auto section_process = [&](WebView::ProcessSnapshot const& process) -> WebView::ProcessSnapshot const& {
        auto const* section = &process;
        for (size_t depth = 0; depth < processes.size() && section->owner_child_id.has_value() && site_of(*section).isEmpty(); ++depth) {
            auto owner = processes_by_child_id.get(*section->owner_child_id);
            if (!owner.has_value())
                break;
            section = *owner;
        }
        return *section;
    };

    struct Site {
        QString name;
        Vector<WebView::ProcessSnapshot const*> processes;
        size_t tab_count { 0 };
        size_t frame_count { 0 };
        WebView::ProcessPageSnapshot const* page { nullptr };
        double cpu { 0 };
        u64 memory { 0 };
    };
    Vector<WebView::ProcessSnapshot const*> own_processes;
    Vector<WebView::ProcessSnapshot const*> spare_processes;
    Vector<Site> sites;
    double total_cpu = 0;
    u64 total_memory = 0;

    for (auto const& process : processes) {
        total_cpu += process.cpu_percent;
        total_memory += process.memory_usage_bytes;
        auto const& section = section_process(process);
        if (is_spare(section)) {
            spare_processes.append(&process);
            continue;
        }
        auto site_name = site_of(section);
        if (site_name.isEmpty()) {
            own_processes.append(&process);
            continue;
        }
        auto site = sites.find_if([&](auto const& site) { return site.name == site_name; });
        if (site == sites.end()) {
            Site new_site;
            new_site.name = site_name;
            sites.append(AK::move(new_site));
            site = sites.end() - 1;
        }
        site->processes.append(&process);
        site->tab_count += process.tabs.size();
        site->frame_count += process.frame_count;
        if (process.page.has_value() && (!site->page || (process.page->kind == WebView::ProcessPageKind::TopLevel && site->page->kind != WebView::ProcessPageKind::TopLevel)))
            site->page = &*process.page;
        site->cpu += process.cpu_percent;
        site->memory += process.memory_usage_bytes;
    }

    auto selected = m_processes->currentItem() && m_processes->currentItem()->isSelected() ? m_processes->currentItem() : nullptr;
    auto selected_child_id = selected && row_kind(selected) == RowKind::Process ? selected->data(PID, ChildIDRole) : QVariant {};
    auto selected_site = selected && row_kind(selected) == RowKind::Site ? selected->data(Name, SiteRole) : QVariant {};
    auto scroll_position = m_processes->verticalScrollBar()->value();

    QSignalBlocker selection_blocker(m_processes);
    m_processes->setUpdatesEnabled(false);
    m_processes->clear();

    auto sort_column = m_processes->header()->sortIndicatorSection();
    auto sort_order = m_processes->header()->sortIndicatorOrder();
    auto sort_items = [&](Vector<QTreeWidgetItem*>& items) {
        quick_sort(items, [&](auto* a, auto* b) { return sorts_before(a, b, sort_column, sort_order); });
    };

    auto set_numbers = [&](QTreeWidgetItem& item, double cpu, u64 memory) {
        item.setText(CPU, locale.toString(cpu, 'f', 1) + '%');
        item.setText(Memory, locale.formattedDataSize(memory, 1, QLocale::DataSizeTraditionalFormat));
        item.setData(CPU, Qt::UserRole, cpu);
        item.setData(Memory, Qt::UserRole, static_cast<qulonglong>(memory));
        for (auto column : { CPU, Memory, PID })
            item.setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    };

    auto make_process_item = [&](WebView::ProcessSnapshot const& process, bool shows_title) {
        auto* item = new QTreeWidgetItem;
        item->setData(Name, KindRole, static_cast<int>(RowKind::Process));
        auto name = qstring_from_ak_string(WebView::process_name_from_type(process.type));
        if (shows_title && process.title.has_value() && process.type != WebView::ProcessType::WebContent)
            name += QStringLiteral(" — ") + qstring_from_utf16_string(*process.title);
        item->setText(Name, name);
        item->setToolTip(Name, name);
        switch (process.type) {
        case WebView::ProcessType::Browser: {
            item->setIcon(Name, m_browser_icon);
            auto font = item->font(Name);
            font.setWeight(QFont::DemiBold);
            item->setFont(Name, font);
            break;
        }
        case WebView::ProcessType::WebContent:
            item->setIcon(Name, m_document_icon);
            break;
        case WebView::ProcessType::RequestServer:
            item->setIcon(Name, m_network_icon);
            break;
        case WebView::ProcessType::Compositor:
            item->setIcon(Name, m_compositor_icon);
            break;
        case WebView::ProcessType::ImageDecoder:
            item->setIcon(Name, m_image_icon);
            break;
        default:
            item->setIcon(Name, m_service_icon);
            break;
        }
        item->setText(PID, QString::number(process.pid));
        item->setData(PID, Qt::UserRole, static_cast<qlonglong>(process.pid));
        item->setData(PID, ChildIDRole, static_cast<qulonglong>(process.child_id));
        set_numbers(*item, process.cpu_percent, process.memory_usage_bytes);
        for (auto const& tab : process.tabs) {
            auto title = tab.title.is_empty() ? tr("Untitled") : qstring_from_utf16_string(tab.title);
            auto* tab_item = new QTreeWidgetItem(item, { title });
            tab_item->setData(Name, KindRole, static_cast<int>(RowKind::Tab));
            tab_item->setFlags(Qt::ItemIsEnabled);
            tab_item->setToolTip(Name, title);
            tab_item->setIcon(Name, favicon_for(tab.page));
        }
        return item;
    };

    auto add_group = [&](QString const& name) {
        auto* item = new QTreeWidgetItem(m_processes, { name });
        item->setData(Name, KindRole, static_cast<int>(RowKind::Group));
        item->setFlags(Qt::ItemIsEnabled);
        auto font = item->font(Name);
        font.setPixelSize(12);
        font.setWeight(QFont::DemiBold);
        item->setFont(Name, font);
        item->setForeground(Name, Theme::panel().muted);
        item->setFirstColumnSpanned(true);
    };

    QTreeWidgetItem* item_to_select = nullptr;
    auto add_items = [&](QTreeWidgetItem* parent, Vector<QTreeWidgetItem*>& items) {
        sort_items(items);
        for (auto* item : items) {
            if (parent)
                parent->addChild(item);
            else
                m_processes->addTopLevelItem(item);
            if (selected_child_id.isValid() && item->data(PID, ChildIDRole) == selected_child_id)
                item_to_select = item;
        }
    };

    // The browser comes first among its own processes, whatever the order.
    Vector<QTreeWidgetItem*> own_items;
    QTreeWidgetItem* browser_item = nullptr;
    for (auto const* process : own_processes) {
        auto* item = make_process_item(*process, true);
        if (process->type == WebView::ProcessType::Browser)
            browser_item = item;
        else
            own_items.append(item);
    }
    if (browser_item) {
        m_processes->addTopLevelItem(browser_item);
        if (selected_child_id.isValid() && browser_item->data(PID, ChildIDRole) == selected_child_id)
            item_to_select = browser_item;
    }
    add_items(nullptr, own_items);

    if (!sites.is_empty()) {
        add_group(tr("Sites"));
        Vector<QTreeWidgetItem*> site_items;
        for (auto& site : sites) {
            auto* item = new QTreeWidgetItem;
            item->setData(Name, KindRole, static_cast<int>(RowKind::Site));
            item->setData(Name, SiteRole, site.name);
            item->setText(Name, site.name);
            item->setToolTip(Name, site.name);
            auto font = item->font(Name);
            font.setWeight(QFont::DemiBold);
            item->setFont(Name, font);
            if (site.page)
                item->setIcon(Name, favicon_for(*site.page));
            QStringList note;
            if (site.tab_count > 0)
                note.append(site.tab_count == 1 ? tr("1 tab") : tr("%1 tabs").arg(site.tab_count));
            if (site.frame_count > 0)
                note.append(site.frame_count == 1 ? tr("1 embedded frame") : tr("%1 embedded frames").arg(site.frame_count));
            item->setData(Name, NoteRole, note.join(QStringLiteral(" · ")));
            set_numbers(*item, site.cpu, site.memory);
            site_items.append(item);

            Vector<QTreeWidgetItem*> process_items;
            for (auto const* process : site.processes)
                process_items.append(make_process_item(*process, false));
            add_items(item, process_items);
            if (selected_site.isValid() && selected_site.toString() == site.name)
                item_to_select = item;
        }
        add_items(nullptr, site_items);
        for (auto* item : site_items) {
            item->setExpanded(m_expanded_sites.contains(ak_string_from_qstring(item->data(Name, SiteRole).toString())));
            for (int i = 0; i < item->childCount(); ++i)
                item->child(i)->setExpanded(true);
        }
    }

    if (!spare_processes.is_empty()) {
        add_group(tr("Spare"));
        Vector<QTreeWidgetItem*> spare_items;
        for (auto const* process : spare_processes) {
            auto* item = make_process_item(*process, false);
            for (int column = 0; column < m_processes->columnCount(); ++column)
                item->setForeground(column, Theme::panel().muted);
            spare_items.append(item);
        }
        add_items(nullptr, spare_items);
    }

    if (item_to_select)
        m_processes->setCurrentItem(item_to_select);
    m_processes->verticalScrollBar()->setValue(scroll_position);
    m_processes->setUpdatesEnabled(true);
    apply_filter();
    update_actions();

    auto value = [&](QString const& text) {
        return QStringLiteral("<span style=\"color: %1\">%2</span>").arg(ChromeStyle::style_sheet_color(Theme::panel().text), text.toHtmlEscaped());
    };
    m_summary->setText(tr("CPU %1&nbsp;&nbsp;&nbsp;&nbsp;Memory %2&nbsp;&nbsp;&nbsp;&nbsp;%3 · %4")
            .arg(value(locale.toString(total_cpu, 'f', 1) + '%'))
            .arg(value(locale.formattedDataSize(total_memory, 1, QLocale::DataSizeTraditionalFormat)))
            .arg(sites.size() == 1 ? tr("1 site") : tr("%1 sites").arg(sites.size()))
            .arg(processes.size() == 1 ? tr("1 process") : tr("%1 processes").arg(processes.size())));
}

void ProcessManagerWindow::apply_filter()
{
    auto query = m_search->text().trimmed();
    auto matches = [&](QTreeWidgetItem* item) {
        return item->text(Name).contains(query, Qt::CaseInsensitive) || item->text(PID).contains(query);
    };
    auto filter = [&](auto& self, QTreeWidgetItem* item) -> bool {
        auto kind = row_kind(item);
        if (kind == RowKind::Group)
            return false;
        bool visible = matches(item);
        bool has_visible_child = false;
        for (int i = 0; i < item->childCount(); ++i)
            has_visible_child |= self(self, item->child(i));
        // A site shows all it holds when its name matches.
        if (kind == RowKind::Site && visible) {
            for (int i = 0; i < item->childCount(); ++i)
                item->child(i)->setHidden(false);
        }
        visible |= has_visible_child;
        item->setHidden(!visible);
        if (has_visible_child && !query.isEmpty())
            item->setExpanded(true);
        return visible;
    };

    // A group shows while any of its rows do.
    QTreeWidgetItem* group = nullptr;
    bool group_has_visible_row = false;
    auto finish_group = [&] {
        if (group)
            group->setHidden(!group_has_visible_row);
    };
    for (int i = 0; i < m_processes->topLevelItemCount(); ++i) {
        auto* item = m_processes->topLevelItem(i);
        if (row_kind(item) == RowKind::Group) {
            finish_group();
            group = item;
            group_has_visible_row = false;
            continue;
        }
        group_has_visible_row |= filter(filter, item);
    }
    finish_group();
    update_actions();
}

void ProcessManagerWindow::update_actions()
{
    auto* item = m_processes->currentItem();
    bool has_selection = item && item->isSelected() && !item->isHidden();
    auto kind = has_selection ? row_kind(item) : RowKind::Group;
    m_end_action->setText(tr("End Process"));
    m_copy_pid_action->setEnabled(false);
    // Ending a site ends the processes that show it, so the button says how many.
    if (kind == RowKind::Site) {
        int process_count = 0;
        for (int i = 0; i < item->childCount(); ++i) {
            if (row_kind(item->child(i)) == RowKind::Process)
                ++process_count;
        }
        if (process_count > 1)
            m_end_action->setText(tr("End %1 processes").arg(process_count));
        m_end_action->setEnabled(process_count > 0);
        return;
    }
    if (kind != RowKind::Process) {
        m_end_action->setEnabled(false);
        return;
    }
    auto process = m_process_manager.find_child(item->data(PID, ChildIDRole).toULongLong());
    m_copy_pid_action->setEnabled(process.has_value());
    m_end_action->setEnabled(process.has_value() && process->type() != WebView::ProcessType::Browser);
}

void ProcessManagerWindow::end_selection()
{
    update_actions();
    if (!m_end_action->isEnabled())
        return;

    // Ending a site ends the processes that show it.
    Vector<QTreeWidgetItem*> items;
    auto* selected = m_processes->currentItem();
    if (row_kind(selected) == RowKind::Site) {
        for (int i = 0; i < selected->childCount(); ++i) {
            if (row_kind(selected->child(i)) == RowKind::Process)
                items.append(selected->child(i));
        }
    } else {
        items.append(selected);
    }

    for (auto* item : items) {
        auto pid = static_cast<pid_t>(item->data(PID, Qt::UserRole).toLongLong());
        auto child_id = item->data(PID, ChildIDRole).toULongLong();
        if (auto result = m_process_manager.terminate_child(child_id, Core::Process::TerminationMode::Forceful); result.is_error()) {
            if (!result.error().is_errno() || result.error().code() != ESRCH)
                QMessageBox::warning(this, tr("Unable to end process"), qformatted("Could not end process {}: {}", pid, result.error()));
        }
    }
    m_processes->clearSelection();
    refresh();
}

void ProcessManagerWindow::show_context_menu(QPoint const& position)
{
    auto* item = m_processes->itemAt(position);
    if (!item || (row_kind(item) != RowKind::Process && row_kind(item) != RowKind::Site))
        return;
    m_processes->setCurrentItem(item);
    update_actions();
    QMenu menu(this);
    if (m_end_action->isEnabled())
        menu.addAction(m_end_action);
    if (row_kind(item) == RowKind::Process)
        menu.addAction(m_copy_pid_action);
    if (menu.isEmpty())
        return;
    menu.exec(m_processes->viewport()->mapToGlobal(position));
}

}
