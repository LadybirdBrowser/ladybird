/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <UI/Qt/CompositorLayersWindow.h>
#include <UI/Qt/StringUtils.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProxyStyle>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStyleOption>
#include <QTextDocument>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace Ladybird {

namespace {

enum Column {
    Layer,
    Elements,
    Holds,
    Rastered,
};

constexpr int LayerIDRole = Qt::UserRole + 1;
constexpr int StackPositionRole = Qt::UserRole + 2;
constexpr int TilesRasteredSinceResetRole = Qt::UserRole + 3;
constexpr int REFRESH_INTERVAL_IN_MILLISECONDS = 150;
constexpr int SWATCH_SIZE = 12;
constexpr int INITIAL_WIDTH_OF_LAYER_COLUMN = 280;
constexpr int MAXIMUM_VISIBLE_DETAIL_LINE_COUNT = 10;

QIcon swatch_of(u32 rgb)
{
    QPixmap pixmap(SWATCH_SIZE, SWATCH_SIZE);
    pixmap.fill(QColor::fromRgb(rgb));
    return QIcon(pixmap);
}

// Sorts the layer column by where each layer is in the stack, and the rastered column by how many tiles were rastered.
class LayerItem final : public QTreeWidgetItem {
public:
    AK_ALLOC_WITH_KMALLOC;

    using QTreeWidgetItem::QTreeWidgetItem;

    virtual bool operator<(QTreeWidgetItem const& other) const override
    {
        switch (treeWidget()->sortColumn()) {
        case Layer:
            return data(Layer, StackPositionRole).toInt() < other.data(Layer, StackPositionRole).toInt();
        case Rastered:
            return data(Rastered, TilesRasteredSinceResetRole).toULongLong() < other.data(Rastered, TilesRasteredSinceResetRole).toULongLong();
        default:
            return QTreeWidgetItem::operator<(other);
        }
    }
};

#if defined(AK_OS_MACOS)
// The macOS style draws its native check box through a painter only once: the checks of the rows after the first, which
// an item view draws through the same painter, come out blank. Each is drawn through a painter of its own instead.
class StyleDrawingEachItemCheckThroughPainterOfItsOwn final : public QProxyStyle {
public:
    virtual void drawPrimitive(PrimitiveElement element, QStyleOption const* option, QPainter* painter, QWidget const* widget) const override
    {
        auto const* item_option = qstyleoption_cast<QStyleOptionViewItem const*>(option);
        if (element != PE_IndicatorItemViewItemCheck || !item_option || option->rect.isEmpty()) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        auto device_pixel_ratio = painter->device()->devicePixelRatioF();
        QPixmap check(option->rect.size() * device_pixel_ratio);
        check.setDevicePixelRatio(device_pixel_ratio);
        check.fill(Qt::transparent);
        auto check_option = *item_option;
        check_option.rect = QRect({}, option->rect.size());
        {
            QPainter check_painter(&check);
            QProxyStyle::drawPrimitive(element, &check_option, &check_painter, widget);
        }
        painter->drawPixmap(option->rect.topLeft(), check);
    }
};
#endif

// What the dump and the name of a layer call the node.
QString name_of_node(Compositing::CompositorLayerNodeDescription const& node)
{
    return QString("%1%2").arg(node.kind == Compositing::VisualContextNodeKind::Spatial ? 's' : 'e').arg(node.index);
}

QString elements_of_layer(Compositing::CompositorLayerDescription const& layer)
{
    QStringList elements;
    for (auto const& node : layer.nodes) {
        auto element = qstring_from_ak_string(node.element);
        if (!element.isEmpty() && !elements.contains(element))
            elements.append(element);
    }
    return elements.join(", ");
}

QString nodes_of_layer_with_elements(Compositing::CompositorLayerDescription const& layer)
{
    QStringList lines;
    for (auto const& node : layer.nodes) {
        auto element = qstring_from_ak_string(node.element);
        lines.append(QString("%1: %2").arg(name_of_node(node), element.isEmpty() ? "no live element" : element));
    }
    return lines.join('\n');
}

bool note_mentions_node(QString const& note, QString const& name_of_node)
{
    return note.contains(QRegularExpression(QString("\\b%1\\b").arg(name_of_node)));
}

}

CompositorLayersWindow::CompositorLayersWindow(Source source)
    : QDialog(nullptr, Qt::Tool)
    , m_source(AK::move(source))
{
    setWindowTitle("Compositor Layers");
    resize(720, 560);

    m_pick_button = new QPushButton("Pick on Page", this);
    m_pick_button->setCheckable(true);
    m_pick_button->setToolTip("Click a layer on the page to select it: the smallest layer shown under the pointer. Escape cancels.");
    m_show_only_selected_button = new QPushButton("Show Only Selected", this);
    m_show_only_selected_button->setToolTip("Uncheck every layer but the selected ones.");
    m_show_only_selected_button->setEnabled(false);
    m_show_all_button = new QPushButton("Show All", this);
    m_show_all_button->setToolTip("Check every layer.");
    m_reset_button = new QPushButton("Reset Raster Counts", this);
    m_reset_button->setToolTip("Count the tiles rastered from now on.");
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_pick_button);
    buttons->addWidget(m_show_only_selected_button);
    buttons->addWidget(m_show_all_button);
    buttons->addStretch();
    buttons->addWidget(m_reset_button);

    m_layer_tree = new QTreeWidget(this);
#if defined(AK_OS_MACOS)
    // The tree does not own its style. As a child of the window made after the tree, the style outlives it.
    auto* style_of_layer_tree = new StyleDrawingEachItemCheckThroughPainterOfItsOwn;
    style_of_layer_tree->setParent(this);
    m_layer_tree->setStyle(style_of_layer_tree);
#endif
    m_layer_tree->setColumnCount(4);
    m_layer_tree->setHeaderLabels({ "Layer", "Elements", "Holds", "Rastered" });
    m_layer_tree->setRootIsDecorated(false);
    m_layer_tree->setUniformRowHeights(true);
    // Each selected layer shows its tiles over the page.
    m_layer_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_layer_tree->setMouseTracking(true);
    m_layer_tree->viewport()->installEventFilter(this);
    m_layer_tree->header()->setStretchLastSection(false);
    m_layer_tree->header()->setSectionResizeMode(Layer, QHeaderView::Interactive);
    m_layer_tree->header()->setSectionResizeMode(Elements, QHeaderView::Stretch);
    m_layer_tree->header()->setSectionResizeMode(Holds, QHeaderView::ResizeToContents);
    m_layer_tree->header()->setSectionResizeMode(Rastered, QHeaderView::ResizeToContents);
    m_layer_tree->setColumnWidth(Layer, INITIAL_WIDTH_OF_LAYER_COLUMN);
    m_layer_tree->setSortingEnabled(true);
    m_layer_tree->sortByColumn(Layer, Qt::DescendingOrder);

    // The details and the summaries show text from other processes. Do not read that text as markup. The details of a
    // layer can run long, so they scroll within a few lines.
    m_details = new QPlainTextEdit(this);
    m_details->setReadOnly(true);
    auto margins_of_details = 2 * (m_details->frameWidth() + static_cast<int>(m_details->document()->documentMargin()));
    m_details->setMaximumHeight(m_details->fontMetrics().lineSpacing() * MAXIMUM_VISIBLE_DETAIL_LINE_COUNT + margins_of_details);
    m_summary_of_last_frame = new QLabel(this);
    m_summary_of_last_frame->setTextFormat(Qt::PlainText);
    m_summary_of_last_frame->setWordWrap(true);
    m_summary_since_reset = new QLabel(this);
    m_summary_since_reset->setTextFormat(Qt::PlainText);
    m_summary_since_reset->setWordWrap(true);
    m_tile_highlight_legend = new QLabel(this);
    m_tile_highlight_legend->setTextFormat(Qt::RichText);
    m_tile_highlight_legend->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(buttons);
    layout->addWidget(m_layer_tree, 1);
    layout->addWidget(m_details);
    layout->addWidget(m_summary_of_last_frame);
    layout->addWidget(m_summary_since_reset);
    layout->addWidget(m_tile_highlight_legend);

    QObject::connect(m_layer_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column != Layer)
            return;
        auto layer_id = item->data(Layer, LayerIDRole).toULongLong();
        m_source.set_layer_hidden_in_active_tab(layer_id, item->checkState(Layer) == Qt::Unchecked);
    });
    QObject::connect(m_layer_tree, &QTreeWidget::itemSelectionChanged, this, [this] { tell_tab_of_selected_layers(); });
    QObject::connect(m_layer_tree, &QTreeWidget::currentItemChanged, this, [this] { show_details_of_selected_layer(); });
    QObject::connect(m_layer_tree, &QTreeWidget::itemEntered, this, [this](QTreeWidgetItem* item, int) {
        set_hovered_layer(item->data(Layer, LayerIDRole).toULongLong());
    });
    QObject::connect(m_pick_button, &QPushButton::toggled, this, [this](bool checked) {
        set_picks_layer_on_page(checked);
    });
    QObject::connect(m_show_only_selected_button, &QPushButton::clicked, this, [this] { show_only_selected_layers(); });
    QObject::connect(m_show_all_button, &QPushButton::clicked, this, [this] { show_all_layers(); });
    QObject::connect(m_reset_button, &QPushButton::clicked, this, [this] {
        m_source.reset_raster_counts_in_active_tab();
    });

    m_refresh_timer = new QTimer(this);
    m_refresh_timer->setInterval(REFRESH_INTERVAL_IN_MILLISECONDS);
    QObject::connect(m_refresh_timer, &QTimer::timeout, this, [this] { refresh(); });
}

void CompositorLayersWindow::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    refresh();
    m_refresh_timer->start();
}

void CompositorLayersWindow::hideEvent(QHideEvent* event)
{
    m_refresh_timer->stop();
    m_pick_button->setChecked(false);
    set_hovered_layer({});
    QDialog::hideEvent(event);
}

// All ways to close the dialog call done(): the Escape key and the close button of the window call reject(), which
// calls done().
void CompositorLayersWindow::done(int result)
{
    QDialog::done(result);
    if (on_close)
        on_close();
}

bool CompositorLayersWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_layer_tree->viewport() && event->type() == QEvent::Leave)
        set_hovered_layer({});
    return QDialog::eventFilter(watched, event);
}

void CompositorLayersWindow::set_hovered_layer(Optional<u64> layer_id)
{
    if (m_id_of_hovered_layer == layer_id)
        return;
    m_id_of_hovered_layer = layer_id;
    m_source.set_hovered_layer_in_active_tab(layer_id);
}

void CompositorLayersWindow::set_picks_layer_on_page(bool picks)
{
    if (!picks) {
        m_source.stop_picking_layer_in_active_tab();
        set_hovered_layer({});
        return;
    }
    // The page stops picking by itself after a pick or a cancel.
    auto picking_ended = [this] {
        QSignalBlocker block_stop_request_for_page_that_stopped(m_pick_button);
        m_pick_button->setChecked(false);
        set_hovered_layer({});
    };
    auto started = m_source.start_picking_layer_in_active_tab(CompositorLayerPicker {
        .on_hover = [this](Gfx::IntPoint point) { set_hovered_layer(id_of_layer_to_pick_at(point)); },
        .on_leave = [this] { set_hovered_layer({}); },
        .on_pick = [this, picking_ended](Gfx::IntPoint point) {
            picking_ended();
            auto layer_id = id_of_layer_to_pick_at(point);
            if (!layer_id.has_value())
                return;
            if (auto item = m_item_by_layer_id.get(*layer_id); item.has_value()) {
                m_layer_tree->setCurrentItem(*item);
                m_layer_tree->scrollToItem(*item);
            }
            // The selection shows in the window that has the focus, which takes the arrow keys that walk the list.
            raise();
            activateWindow(); },
        .on_cancel = [picking_ended] { picking_ended(); },
    });
    if (!started) {
        QSignalBlocker block_stop_request_for_page_that_never_started(m_pick_button);
        m_pick_button->setChecked(false);
    }
}

Vector<u64> CompositorLayersWindow::ids_of_selected_layers() const
{
    Vector<u64> layer_ids;
    for (auto const* item : m_layer_tree->selectedItems())
        layer_ids.append(item->data(Layer, LayerIDRole).toULongLong());
    quick_sort(layer_ids);
    return layer_ids;
}

void CompositorLayersWindow::tell_tab_of_selected_layers()
{
    auto layer_ids = ids_of_selected_layers();
    m_show_only_selected_button->setEnabled(!layer_ids.is_empty());
    if (layer_ids != m_ids_of_selected_layers_told_to_tab) {
        m_ids_of_selected_layers_told_to_tab = layer_ids;
        m_source.set_selected_layers_in_active_tab(AK::move(layer_ids));
    }
    show_details_of_selected_layer();
}

// The checkboxes change at once, and stay as they are once the compositor tells what it hides.
void CompositorLayersWindow::show_only_selected_layers()
{
    auto layer_ids = ids_of_selected_layers();
    if (layer_ids.is_empty())
        return;
    {
        QSignalBlocker block_hide_requests_for_each_row(m_layer_tree);
        for (auto const& [layer_id, item] : m_item_by_layer_id)
            item->setCheckState(Layer, layer_ids.contains_slow(layer_id) ? Qt::Checked : Qt::Unchecked);
    }
    m_source.show_only_layers_in_active_tab(AK::move(layer_ids));
}

void CompositorLayersWindow::show_all_layers()
{
    {
        QSignalBlocker block_hide_requests_for_each_row(m_layer_tree);
        for (auto const& [layer_id, item] : m_item_by_layer_id)
            item->setCheckState(Layer, Qt::Checked);
    }
    m_source.show_all_layers_in_active_tab();
}

Compositing::CompositorLayerDescription const* CompositorLayersWindow::shown_layer_with_id(u64 layer_id) const
{
    if (!m_shown_description.has_value())
        return nullptr;
    for (auto const& layer : m_shown_description->layers) {
        if (layer.id == layer_id)
            return &layer;
    }
    return nullptr;
}

// The outline of a layer bounds the tiles it holds, and a layer that holds content all over the page bounds most of
// the viewport. The smallest shown layer under the pointer is what the pointer is over, and of layers as large, the
// topmost.
Optional<u64> CompositorLayersWindow::id_of_layer_to_pick_at(Gfx::IntPoint point) const
{
    if (!m_shown_description.has_value())
        return {};
    Optional<u64> picked_layer_id;
    i64 area_of_picked_layer = 0;
    for (auto const& layer : m_shown_description->layers) {
        auto const& rect = layer.visible_rect_in_viewport;
        if (layer.hidden || !rect.contains(point))
            continue;
        auto area = static_cast<i64>(rect.width()) * rect.height();
        if (!picked_layer_id.has_value() || area <= area_of_picked_layer) {
            picked_layer_id = layer.id;
            area_of_picked_layer = area;
        }
    }
    return picked_layer_id;
}

void CompositorLayersWindow::refresh()
{
    auto description = m_source.layers_of_active_tab();
    if (description == m_shown_description)
        return;

    {
        QSignalBlocker block_requests_while_showing_what_compositor_does(m_layer_tree);

        // Keeping the rows of the layers that stay keeps the selection and where the list was scrolled to.
        HashTable<u64> ids_of_described_layers;
        if (description.has_value()) {
            for (auto const& layer : description->layers)
                ids_of_described_layers.set(layer.id);
        }
        Vector<u64> ids_of_layers_gone;
        for (auto const& [layer_id, item] : m_item_by_layer_id) {
            if (!ids_of_described_layers.contains(layer_id))
                ids_of_layers_gone.append(layer_id);
        }
        for (auto layer_id : ids_of_layers_gone)
            delete m_item_by_layer_id.take(layer_id).value();

        auto layer_count = description.has_value() ? description->layers.size() : 0;
        for (size_t position = 0; position < layer_count; ++position) {
            auto const& layer = description->layers[position];
            auto* item = m_item_by_layer_id.get(layer.id).value_or(nullptr);
            if (!item) {
                item = new LayerItem(m_layer_tree);
                item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
                item->setData(Layer, LayerIDRole, QVariant::fromValue<qulonglong>(layer.id));
                item->setTextAlignment(Rastered, Qt::AlignRight | Qt::AlignVCenter);
                m_item_by_layer_id.set(layer.id, item);
            }
            auto name = qstring_from_ak_string(layer.name);
            item->setData(Layer, StackPositionRole, static_cast<int>(position));
            item->setCheckState(Layer, layer.hidden ? Qt::Unchecked : Qt::Checked);
            item->setIcon(Layer, swatch_of(layer.rgb_of_outline));
            item->setText(Layer, name);
            item->setToolTip(Layer, name);
            item->setText(Elements, elements_of_layer(layer));
            item->setToolTip(Elements, nodes_of_layer_with_elements(layer));
            item->setText(Holds, qstring_from_ak_string(layer.details));
            item->setData(Rastered, TilesRasteredSinceResetRole, QVariant::fromValue<qulonglong>(layer.tiles_rastered_since_reset));
            item->setText(Rastered, layer.tiles_rastered_since_reset == 0 ? QString {} : QString::number(layer.tiles_rastered_since_reset));
            item->setToolTip(Rastered, qstring_from_ak_string(layer.raster_causes_since_reset));
        }
    }

    m_shown_description = AK::move(description);
    if (!m_shown_description.has_value()) {
        m_summary_of_last_frame->setText("The active tab has presented no frame since the layers were shown.");
        m_summary_since_reset->clear();
        m_tile_highlight_legend->clear();
    } else {
        m_summary_of_last_frame->setText(qstring_from_ak_string(m_shown_description->summary_of_last_frame));
        m_summary_since_reset->setText(qstring_from_ak_string(m_shown_description->summary_since_reset));
        QStringList tile_highlight_legend_entries;
        for (auto const& highlight : m_shown_description->tile_highlights) {
            auto color = QColor::fromRgb(highlight.rgb).name();
            tile_highlight_legend_entries.append(QString("<span style=\"color: %1\">&#9632;</span> %2").arg(color, qstring_from_ak_string(highlight.raster_cause).toHtmlEscaped()));
        }
        m_tile_highlight_legend->setText("Tiles rastered are highlighted by why: " + tile_highlight_legend_entries.join("&nbsp;&nbsp; "));
    }

    // The row of a layer that left the stack went with the selection.
    tell_tab_of_selected_layers();
}

void CompositorLayersWindow::show_details_of_selected_layer()
{
    if (!m_shown_description.has_value()) {
        m_details->clear();
        return;
    }
    QStringList notes;
    for (auto const& note : m_shown_description->notes)
        notes.append(qstring_from_ak_string(note));

    // Of the selected layers, the details are of the one clicked last.
    QStringList lines;
    auto const* current_item = m_layer_tree->currentItem();
    auto selected_items = m_layer_tree->selectedItems();
    auto const* item = current_item && current_item->isSelected() ? current_item : (selected_items.isEmpty() ? nullptr : selected_items.first());
    auto const* layer = item ? shown_layer_with_id(item->data(Layer, LayerIDRole).toULongLong()) : nullptr;
    if (!layer) {
        lines.append("Select layers, or pick one on the page, to see their tiles and the elements that made them. Cmd-click or Shift-click selects more than one.");
        if (!notes.isEmpty()) {
            lines.append(QString {});
            lines.append(notes);
        }
    } else {
        if (selected_items.size() > 1)
            lines.append(QString("%1 layers selected; this one was clicked last:").arg(selected_items.size()));
        lines.append(qstring_from_ak_string(layer->name));
        if (!layer->nodes.is_empty()) {
            lines.append("Nodes its name mentions:");
            for (auto const& line : nodes_of_layer_with_elements(*layer).split('\n'))
                lines.append("  " + line);
        }
        lines.append("Holds: " + qstring_from_ak_string(layer->details));
        auto raster_causes = qstring_from_ak_string(layer->raster_causes_since_reset);
        lines.append(QString("Tiles rastered since reset: %1%2").arg(layer->tiles_rastered_since_reset).arg(raster_causes.isEmpty() ? QString {} : QString(" (%1)").arg(raster_causes)));
        auto rect = layer->visible_rect_in_viewport;
        if (rect.is_empty())
            lines.append("Not shown in the viewport");
        else
            lines.append(QString("Shown at %1,%2 %3x%4 in device pixels").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()));
        QStringList notes_of_layer;
        for (auto const& note : notes) {
            auto mentions_node_of_layer = any_of(layer->nodes, [&](auto const& node) { return note_mentions_node(note, name_of_node(node)); });
            if (mentions_node_of_layer)
                notes_of_layer.append("  " + note);
        }
        if (!notes_of_layer.isEmpty()) {
            lines.append("Notes of its nodes:");
            lines.append(notes_of_layer);
        }
    }
    // Setting the same text again would scroll the details back to the top.
    auto text = lines.join('\n');
    if (m_details->toPlainText() != text)
        m_details->setPlainText(text);
}

}
