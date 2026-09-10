/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibWebView/Application.h>
#include <LibWebView/SiteIsolationManager.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/ProcessManagerWindow.h>
#include <UI/Qt/StringUtils.h>

#include <QEvent>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QScrollBar>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace Ladybird {

namespace {

enum Column {
    Name,
    PID,
    CPU,
    Memory,
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
        size.setHeight(qMax(size.height(), 34));
        return size;
    }
};

class ProcessItem final : public QTreeWidgetItem {
public:
    virtual bool operator<(QTreeWidgetItem const& other) const override
    {
        auto column = treeWidget()->sortColumn();
        if (column == Name)
            return QString::localeAwareCompare(text(column), other.text(column)) < 0;
        if (column == CPU)
            return data(column, Qt::UserRole).toDouble() < other.data(column, Qt::UserRole).toDouble();
        return data(column, Qt::UserRole).toULongLong() < other.data(column, Qt::UserRole).toULongLong();
    }
};

}

ProcessManagerWindow::ProcessManagerWindow()
{
    setWindowFlags(Qt::Window);
    setWindowTitle(tr("Task Manager"));
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(860, 500);
    setMinimumSize(660, 320);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 4);
    layout->setSpacing(16);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(24);
    m_filters = new QTabBar(this);
    m_filters->addTab(tr("All processes"));
    m_filters->addTab(tr("Web content"));
    m_filters->addTab(tr("Browser"));
    m_filters->setAccessibleName(tr("Process type"));
    m_filters->setDrawBase(false);
    m_filters->setExpanding(false);
    toolbar->addWidget(m_filters);
    toolbar->addStretch();
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search processes"));
    m_search->setAccessibleName(tr("Search processes"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(180);
    m_search->setMaximumWidth(260);
    m_search->addAction(create_chrome_icon(ChromeIcon::Search, palette()), QLineEdit::LeadingPosition);
    toolbar->addWidget(m_search);
    layout->addLayout(toolbar);

    m_processes = new QTreeWidget(this);
    m_processes->setHeaderLabels({ tr("Process"), tr("PID"), tr("CPU"), tr("Memory") });
    m_processes->setIconSize(QSize(20, 20));
    m_processes->setAccessibleName(tr("Processes"));
    m_processes->setFrameShape(QFrame::NoFrame);
    m_processes->setUniformRowHeights(true);
    m_processes->setItemDelegate(new ProcessDelegate(m_processes));
    m_processes->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processes->setSortingEnabled(true);
    m_processes->sortByColumn(PID, Qt::AscendingOrder);
    m_processes->setIndentation(20);
    m_processes->header()->setStretchLastSection(false);
    m_processes->header()->setSectionResizeMode(Name, QHeaderView::Stretch);
    for (auto column : { PID, CPU, Memory }) {
        m_processes->header()->setSectionResizeMode(column, QHeaderView::Interactive);
        m_processes->setColumnWidth(column, column == Memory ? 130 : 100);
        m_processes->headerItem()->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    }
    layout->addWidget(m_processes, 1);
    connect(m_search, &QLineEdit::textChanged, this, &ProcessManagerWindow::apply_filter);
    connect(m_filters, &QTabBar::currentChanged, this, &ProcessManagerWindow::apply_filter);

    auto* summary_layout = new QHBoxLayout;
    summary_layout->setContentsMargins(8, 12, 8, 12);
    m_process_count = new QLabel(this);
    m_process_count->setAttribute(Qt::WA_MacSmallSize);
    summary_layout->addWidget(m_process_count);
    summary_layout->addStretch();
    m_summary = new QLabel(this);
    m_summary->setAttribute(Qt::WA_MacSmallSize);
    m_summary->setAlignment(Qt::AlignRight);
    m_summary->setToolTip(tr("Total CPU and memory for all Ladybird processes. Updates every second."));
    summary_layout->addWidget(m_summary);
    layout->addLayout(summary_layout);

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
    auto surface = ChromeStyle::style_sheet_color(ChromeStyle::mix(background, text, 0.08));
    auto border = ChromeStyle::style_sheet_color(ChromeStyle::mix(background, text, 0.18));
    auto foreground = ChromeStyle::style_sheet_color(text);
    auto accent = ChromeStyle::chrome_accent(palette());
    if (ChromeStyle::is_dark(palette()))
        accent = ChromeStyle::mix(accent, text, 0.45);

    m_filters->setStyleSheet(QStringLiteral(
        "QTabBar::tab { background: transparent; color: %1; padding: 10px 12px; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: %2; border-bottom-color: %2; }")
            .arg(foreground, ChromeStyle::style_sheet_color(accent)));
    m_processes->setStyleSheet(QStringLiteral(
        "QTreeWidget { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 4px; }"
        "QTreeWidget::item { padding: 0px 8px; }")
            .arg(surface, foreground, border));
    m_processes->header()->setStyleSheet(QStringLiteral(
        "QHeaderView::section { background: %1; color: %2; border: none; border-bottom: 1px solid %3;"
        " padding: 10px 24px 10px 8px; font-weight: 600; }")
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
    auto& manager = WebView::Application::process_manager();
    manager.update_all_process_statistics();
    auto embedders = WebView::SiteIsolationManager::the().remote_frame_process_embedders();

    auto scroll_position = m_processes->verticalScrollBar()->value();
    auto selected_pid = m_processes->currentItem() ? m_processes->currentItem()->data(PID, Qt::UserRole) : QVariant {};
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
    double total_cpu = 0;
    u64 total_memory = 0;
    QLocale locale;
    auto browser_icon = load_icon_from_uri("resource://icons/48x48/app-browser.png"sv);
    auto web_content_icon = create_chrome_icon(ChromeIcon::Globe, palette());
    auto service_icon = create_chrome_icon(ChromeIcon::WindowRestore, palette());
    manager.for_each_process_statistics([&](auto& process, auto const& statistics) {
        auto* item = items.take(statistics.pid).value_or_lazy_evaluated([] { return new ProcessItem; });
        auto name = qstring_from_ak_string(WebView::process_name_from_type(process.type()));
        if (process.title().has_value())
            name += QStringLiteral(" - ") + qstring_from_utf16_string(*process.title());
        item->setText(Name, name);
        item->setData(Name, Qt::UserRole, static_cast<int>(process.type()));
        if (process.type() == WebView::ProcessType::Browser)
            item->setIcon(Name, browser_icon);
        else
            item->setIcon(Name, process.type() == WebView::ProcessType::WebContent ? web_content_icon : service_icon);
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
        auto* parent = m_processes->invisibleRootItem();
        if (auto embedder = embedders.get(entry.key); embedder.has_value()) {
            if (auto parent_item = live_items.get(*embedder); parent_item.has_value())
                parent = *parent_item;
        }
        parent->addChild(entry.value);
    }
    for (auto& entry : live_items) {
        entry.value->setExpanded(expanded.get(entry.key).value_or(true));
        if (entry.value->data(PID, Qt::UserRole) == selected_pid)
            m_processes->setCurrentItem(entry.value);
    }
    m_processes->setSortingEnabled(true);
    m_processes->verticalScrollBar()->setValue(scroll_position);
    m_processes->setUpdatesEnabled(true);
    apply_filter();
    m_summary->setText(tr("CPU: %1%    Memory: %2")
            .arg(locale.toString(total_cpu, 'f', 2))
            .arg(locale.formattedDataSize(total_memory, 2)));
}

void ProcessManagerWindow::apply_filter()
{
    auto query = m_search->text().trimmed();
    auto group = m_filters->currentIndex();
    size_t visible_count = 0;
    size_t total_count = 0;
    auto filter = [&](auto& self, QTreeWidgetItem* item) -> bool {
        ++total_count;
        auto type = static_cast<WebView::ProcessType>(item->data(Name, Qt::UserRole).toInt());
        bool is_web_content = type == WebView::ProcessType::WebContent || type == WebView::ProcessType::WebWorker;
        bool matches_group = group == 0 || (group == 1 ? is_web_content : !is_web_content);
        bool visible = matches_group && (item->text(Name).contains(query, Qt::CaseInsensitive) || item->text(PID).contains(query));
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
}

}
