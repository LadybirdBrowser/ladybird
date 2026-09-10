/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibURL/InternalURLs.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/ViewImplementation.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/ProcessManagerWindow.h>
#include <UI/Qt/StringUtils.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
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

#include <errno.h>

namespace Ladybird {

QString task_manager_site_label(URL::URL const& url)
{
    if (url == URL::about_blank())
        return {};
    if (url.host().has_value()) {
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
    PID,
    CPU,
    Memory,
};

class ProcessHeader final : public QHeaderView {
public:
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
        option.rect = rect;
        auto text = option.text;
        option.text.clear();
        option.sortIndicator = QStyleOptionHeader::None;
        painter->save();
        painter->setClipRect(rect);
        style()->drawControl(QStyle::CE_Header, &option, painter, this);
        auto text_rect = rect.adjusted(8, 0, -8, 0);
        if (isSortIndicatorShown() && sortIndicatorSection() == section) {
            auto arrow_right = text_rect.right();
            auto center_y = rect.center().y();
            auto color = option.palette.color(QPalette::Text);
            color.setAlphaF(0.65);
            painter->setPen(QPen(color, 1));
            painter->setRenderHint(QPainter::Antialiasing);
            auto direction = sortIndicatorOrder() == Qt::AscendingOrder ? 1 : -1;
            QPointF points[] {
                { static_cast<qreal>(arrow_right - 6), center_y + direction * 1.5 },
                { static_cast<qreal>(arrow_right - 3), center_y - direction * 1.5 },
                { static_cast<qreal>(arrow_right), center_y + direction * 1.5 },
            };
            painter->drawPolyline(points, 3);
            text_rect.setRight(arrow_right - 10);
        }
        painter->setPen(option.palette.color(QPalette::Text));
        painter->drawText(text_rect, option.textAlignment | Qt::AlignVCenter, fontMetrics().elidedText(text, Qt::ElideRight, text_rect.width()));
        painter->restore();
    }
};

class ProcessTree final : public QTreeWidget {
public:
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
};

class ProcessDelegate final : public QStyledItemDelegate {
public:
    explicit ProcessDelegate(QObject* parent)
        : QStyledItemDelegate(parent)
    {
    }

    virtual QSize sizeHint(QStyleOptionViewItem const& option, QModelIndex const& index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), 27));
        return size;
    }
};

class ProcessItem final : public QTreeWidgetItem {
public:
    void update_page_icon(WebView::ViewImplementation const* view, QIcon const& fallback)
    {
        auto view_id = view ? Optional<u64> { view->view_id() } : Optional<u64> {};
        auto favicon_hash = view ? view->favicon_hash() : Optional<String> {};
        auto url = view ? Optional<URL::URL> { view->url() } : Optional<URL::URL> {};
        if (view_id != m_view_id || favicon_hash != m_favicon_hash || url != m_url) {
            m_view_id = view_id;
            m_favicon_hash = favicon_hash;
            m_url = url;
            m_favicon = {};
            if (view && favicon_hash.has_value()) {
                if (auto png = WebView::Application::favicon_store(view->is_private()).favicon_png(*favicon_hash); png.has_value())
                    m_favicon = icon_from_png(png->bytes(), 16);
            }
        }
        setIcon(Name, m_favicon.isNull() ? fallback : m_favicon);
    }

    virtual bool operator<(QTreeWidgetItem const& other) const override
    {
        auto column = treeWidget()->sortColumn();
        if (column == Name)
            return QString::localeAwareCompare(text(column), other.text(column)) < 0;
        if (column == CPU)
            return data(column, Qt::UserRole).toDouble() < other.data(column, Qt::UserRole).toDouble();
        return data(column, Qt::UserRole).toULongLong() < other.data(column, Qt::UserRole).toULongLong();
    }

private:
    Optional<u64> m_view_id;
    Optional<String> m_favicon_hash;
    Optional<URL::URL> m_url;
    QIcon m_favicon;
};

}

ProcessManagerWindow::ProcessManagerWindow(WebView::ProcessManager& process_manager)
    : m_process_manager(process_manager)
{
    setWindowFlags(Qt::Window);
    setWindowTitle(tr("Task Manager"));
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(760, 360);
    setMinimumSize(560, 260);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 4);
    layout->setSpacing(8);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(24);
    m_process_count = new QLabel(tr("Processes"), this);
    toolbar->addWidget(m_process_count);
    toolbar->addStretch();
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search processes"));
    m_search->setAccessibleName(tr("Search processes"));
    m_search->setClearButtonEnabled(true);
    m_search->setAttribute(Qt::WA_MacSmallSize);
    m_search->setMinimumWidth(180);
    m_search->setMaximumWidth(240);
    m_search->addAction(create_chrome_icon(ChromeIcon::Search, palette()), QLineEdit::LeadingPosition);
    toolbar->addWidget(m_search);
    layout->addLayout(toolbar);

    auto* process_tree = new ProcessTree(this);
    process_tree->on_context_menu = [this](auto const& position) { show_context_menu(position); };
    m_processes = process_tree;
    m_processes->setHeader(new ProcessHeader(m_processes));
    m_processes->setHeaderLabels({ tr("Process"), tr("PID"), tr("CPU"), tr("Memory") });
    m_processes->headerItem()->setTextAlignment(Name, Qt::AlignLeft | Qt::AlignVCenter);
    m_processes->setIconSize(QSize(16, 16));
    m_processes->setAccessibleName(tr("Processes"));
    m_processes->setFrameShape(QFrame::NoFrame);
    m_processes->setUniformRowHeights(true);
    m_processes->setItemDelegate(new ProcessDelegate(m_processes));
    m_processes->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processes->setSortingEnabled(true);
    m_processes->sortByColumn(PID, Qt::AscendingOrder);
    m_processes->setIndentation(14);
    m_processes->header()->setStretchLastSection(false);
    m_processes->header()->setSectionResizeMode(Name, QHeaderView::Stretch);
    for (auto column : { PID, CPU, Memory }) {
        m_processes->header()->setSectionResizeMode(column, QHeaderView::Interactive);
        m_processes->setColumnWidth(column, column == Memory ? 130 : 100);
        m_processes->headerItem()->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    }
    layout->addWidget(m_processes, 1);
    connect(m_search, &QLineEdit::textChanged, this, &ProcessManagerWindow::apply_filter);

    auto* summary_layout = new QHBoxLayout;
    summary_layout->setContentsMargins(0, 4, 0, 8);
    m_end_process_action = new QAction(tr("End Process"), this);
    m_end_process_action->setEnabled(false);
    connect(m_end_process_action, &QAction::triggered, this, &ProcessManagerWindow::end_selected_process);
    m_end_process_button = new QPushButton(m_end_process_action->text(), this);
    m_end_process_button->setAutoDefault(false);
    m_end_process_button->setEnabled(false);
    connect(m_end_process_button, &QPushButton::clicked, m_end_process_action, &QAction::trigger);
    connect(m_end_process_action, &QAction::changed, this, [this] {
        m_end_process_button->setEnabled(m_end_process_action->isEnabled());
    });
    summary_layout->addWidget(m_end_process_button);
    summary_layout->addStretch();
    m_summary = new QLabel(this);
    m_summary->setAttribute(Qt::WA_MacSmallSize);
    m_summary->setAlignment(Qt::AlignRight);
    m_summary->setToolTip(tr("Total CPU and memory for all Ladybird processes. Updates every second."));
    summary_layout->addWidget(m_summary);
    layout->addLayout(summary_layout);

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

    auto text = ChromeStyle::chrome_text(palette());
    auto background = ChromeStyle::chrome_background(palette());
    auto surface = ChromeStyle::style_sheet_color(ChromeStyle::is_dark(palette())
            ? ChromeStyle::chrome_surface_recessed(palette())
            : ChromeStyle::chrome_surface(palette()));
    auto border = ChromeStyle::style_sheet_color(ChromeStyle::mix(background, text, 0.12));
    auto foreground = ChromeStyle::style_sheet_color(text);
    auto secondary_foreground = ChromeStyle::style_sheet_color(ChromeStyle::chrome_muted_text(palette()));

    m_process_count->setStyleSheet(QStringLiteral("color: %1;").arg(secondary_foreground));
    m_summary->setStyleSheet(QStringLiteral("color: %1;").arg(secondary_foreground));
    m_processes->setStyleSheet(QStringLiteral(
        "QTreeWidget { background: %1; color: %2; border: none; }"
        "QTreeWidget::item { padding: 0px 8px; }")
            .arg(surface, foreground));
    m_processes->header()->setStyleSheet(QStringLiteral(
        "QHeaderView::section { background: %1; color: %2; border: none; border-bottom: 1px solid %3;"
        " padding: 5px 8px; }")
            .arg(surface, foreground, border));
    m_is_updating_style = false;
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

void ProcessManagerWindow::refresh()
{
    auto& manager = m_process_manager;
    manager.update_all_process_statistics();

    auto scroll_position = m_processes->verticalScrollBar()->value();
    auto selected_pid = m_processes->currentItem() && m_processes->currentItem()->isSelected() ? m_processes->currentItem()->data(PID, Qt::UserRole) : QVariant {};
    QSignalBlocker selection_blocker(m_processes);
    HashMap<pid_t, QTreeWidgetItem*> items;
    HashMap<pid_t, bool> expanded;
    m_processes->setUpdatesEnabled(false);
    m_processes->setSortingEnabled(false);
    auto detach_items = [&](auto& self, QTreeWidgetItem* parent) -> void {
        while (parent->childCount()) {
            auto* item = parent->child(0);
            auto pid = static_cast<pid_t>(item->data(PID, Qt::UserRole).toLongLong());
            expanded.set(pid, item->isExpanded());
            self(self, item);
            parent->takeChild(0);
            items.set(pid, item);
        }
    };
    detach_items(detach_items, m_processes->invisibleRootItem());

    HashMap<pid_t, QTreeWidgetItem*> live_items;
    QTreeWidgetItem* browser_item = nullptr;
    double total_cpu = 0;
    u64 total_memory = 0;
    QLocale locale;
    auto browser_icon = load_icon_from_uri("resource://icons/48x48/app-browser.png"sv);
    auto web_content_icon = create_chrome_icon(ChromeIcon::Globe, palette());
    auto muted_palette = palette();
    auto muted_color = ChromeStyle::chrome_muted_text(palette());
    muted_palette.setColor(QPalette::Text, muted_color);
    muted_palette.setColor(QPalette::WindowText, muted_color);
    muted_palette.setColor(QPalette::ButtonText, muted_color);
    auto service_icon = create_chrome_icon(ChromeIcon::WindowRestore, muted_palette);
    auto spare_icon = create_chrome_icon(ChromeIcon::WindowMaximize, muted_palette);

    enum class PageKind {
        TopLevel,
        Frame,
    };
    struct ProcessPage {
        WebView::ViewImplementation* view;
        URL::URL url;
        PageKind kind;
    };
    HashMap<pid_t, ProcessPage> process_pages;
    WebView::ViewImplementation::for_each_view([&](auto& view) {
        auto active_view = WebView::Application::the().active_web_view();
        auto priority = [&](auto const& candidate) {
            if (active_view.has_value() && &*active_view == &candidate)
                return 2;
            return candidate.traversable().system_visibility_state() == Web::HTML::VisibilityState::Visible ? 1 : 0;
        };
        auto add_page = [&](pid_t pid, URL::URL const& url, PageKind kind) {
            auto current = process_pages.get(pid);
            if (!current.has_value() || priority(view) > priority(*current->view)
                || (priority(view) == priority(*current->view) && view.view_id() < current->view->view_id())
                || (current->view == &view && kind == PageKind::TopLevel && current->kind == PageKind::Frame))
                process_pages.set(pid, ProcessPage { &view, url, kind });
        };
        add_page(view.client().pid(), view.url(), PageKind::TopLevel);
        view.traversable().for_each_in_subtree([&](auto const& navigable) {
            if (navigable.has_remote_host() && navigable.replicated_state().has_value())
                add_page(navigable.remote_host_client().pid(), navigable.replicated_state()->active_document_url, PageKind::Frame);
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });
    manager.for_each_process_statistics([&](auto& process, auto const& statistics) {
        auto* item = static_cast<ProcessItem*>(items.take(statistics.pid).value_or_lazy_evaluated([] { return new ProcessItem; }));
        auto name = qstring_from_ak_string(WebView::process_name_from_type(process.type()));
        if (process.type() == WebView::ProcessType::WebContent) {
            auto page = process_pages.get(statistics.pid);
            auto* view = page.has_value() ? page->view : nullptr;
            bool is_spare = process.title().has_value() && *process.title() == "(spare)"_utf16;
            if (view) {
                auto site = task_manager_site_label(page->url);
                if (!site.isEmpty())
                    name += QStringLiteral(" — ") + site;
            } else if (process.title().has_value()) {
                name += QStringLiteral(" — ") + qstring_from_utf16_string(*process.title());
            }
            item->update_page_icon(page.has_value() && page->url != URL::about_blank() ? view : nullptr, is_spare ? spare_icon : web_content_icon);
        } else {
            if (process.title().has_value())
                name += QStringLiteral(" — ") + qstring_from_utf16_string(*process.title());
            if (process.type() == WebView::ProcessType::Browser) {
                browser_item = item;
                item->setIcon(Name, browser_icon);
            } else {
                item->setIcon(Name, service_icon);
            }
        }
        item->setText(Name, name);
        item->setToolTip(Name, name);
        item->setText(PID, QString::number(statistics.pid));
        item->setText(CPU, locale.toString(statistics.cpu_percent, 'f', 2) + '%');
        item->setText(Memory, locale.formattedDataSize(statistics.memory_usage_bytes, 2));
        item->setData(PID, Qt::UserRole, static_cast<qlonglong>(statistics.pid));
        item->setData(CPU, Qt::UserRole, statistics.cpu_percent);
        item->setData(Memory, Qt::UserRole, static_cast<qulonglong>(statistics.memory_usage_bytes));
        for (auto column : { PID, CPU, Memory })
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        live_items.set(statistics.pid, item);
        total_cpu += statistics.cpu_percent;
        total_memory += statistics.memory_usage_bytes;
    });
    for (auto& entry : items)
        delete entry.value;
    for (auto& entry : live_items) {
        auto* parent = browser_item && entry.value != browser_item ? browser_item : m_processes->invisibleRootItem();
        parent->addChild(entry.value);
    }
    m_processes->setCurrentItem(nullptr);
    for (auto& entry : live_items) {
        entry.value->setExpanded(expanded.get(entry.key).value_or(true));
        if (entry.value->data(PID, Qt::UserRole) == selected_pid)
            m_processes->setCurrentItem(entry.value);
    }
    m_processes->setSortingEnabled(true);
    m_processes->verticalScrollBar()->setValue(scroll_position);
    m_processes->setUpdatesEnabled(true);
    apply_filter();
    update_actions();
    m_summary->setText(tr("CPU: %1%    Memory: %2")
            .arg(locale.toString(total_cpu, 'f', 2))
            .arg(locale.formattedDataSize(total_memory, 2)));
}

void ProcessManagerWindow::apply_filter()
{
    auto query = m_search->text().trimmed();
    size_t visible_count = 0;
    size_t total_count = 0;
    auto filter = [&](auto& self, QTreeWidgetItem* item) -> bool {
        ++total_count;
        bool visible = item->text(Name).contains(query, Qt::CaseInsensitive) || item->text(PID).contains(query);
        bool has_visible_child = false;
        for (int i = 0; i < item->childCount(); ++i)
            has_visible_child |= self(self, item->child(i));
        visible |= has_visible_child;
        item->setHidden(!visible);
        if (has_visible_child && !query.isEmpty())
            item->setExpanded(true);
        if (visible)
            ++visible_count;
        return visible;
    };
    for (int i = 0; i < m_processes->topLevelItemCount(); ++i)
        filter(filter, m_processes->topLevelItem(i));
    m_process_count->setText(visible_count == total_count
            ? tr("%1 processes").arg(total_count)
            : tr("%1 of %2 processes").arg(visible_count).arg(total_count));
    update_actions();
}

void ProcessManagerWindow::update_actions()
{
    auto* item = m_processes->currentItem();
    bool has_selection = item && item->isSelected() && !item->isHidden();
    m_copy_pid_action->setEnabled(has_selection);
    if (!has_selection) {
        m_end_process_action->setEnabled(false);
        return;
    }
    auto pid = static_cast<pid_t>(item->data(PID, Qt::UserRole).toLongLong());
    auto process = m_process_manager.find_process(pid);
    m_copy_pid_action->setEnabled(process.has_value());
    m_end_process_action->setEnabled(process.has_value() && process->type() != WebView::ProcessType::Browser);
}

void ProcessManagerWindow::end_selected_process()
{
    update_actions();
    if (!m_end_process_action->isEnabled())
        return;
    auto pid = static_cast<pid_t>(m_processes->currentItem()->data(PID, Qt::UserRole).toLongLong());
    if (auto result = Core::Process::terminate_process(pid, Core::Process::TerminationMode::Forceful); result.is_error()) {
        if (!result.error().is_errno() || result.error().code() != ESRCH)
            QMessageBox::warning(this, tr("Unable to end process"), qformatted("Could not end process {}: {}", pid, result.error()));
    }
    m_processes->clearSelection();
    refresh();
}

void ProcessManagerWindow::show_context_menu(QPoint const& position)
{
    auto* item = m_processes->itemAt(position);
    if (!item)
        return;
    m_processes->setCurrentItem(item);
    update_actions();
    QMenu menu(this);
    if (m_end_process_action->isEnabled())
        menu.addAction(m_end_process_action);
    menu.addAction(m_copy_pid_action);
    menu.exec(m_processes->viewport()->mapToGlobal(position));
}

}
