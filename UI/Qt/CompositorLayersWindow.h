/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/kmalloc.h>
#include <LibCompositing/Types.h>
#include <UI/Qt/CompositorLayerPicker.h>

#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace Ladybird {

class CompositorLayersWindow final : public QDialog {
public:
    AK_ALLOC_WITH_KMALLOC;

    struct Source {
        Function<Optional<Compositing::CompositorLayersDescription>()> layers_of_active_tab;
        Function<void(u64 layer_id, bool hidden)> set_layer_hidden_in_active_tab;
        Function<void(Vector<u64> layer_ids)> set_selected_layers_in_active_tab;
        Function<void(Optional<u64> layer_id)> set_hovered_layer_in_active_tab;
        Function<void(Vector<u64> layer_ids)> show_only_layers_in_active_tab;
        Function<void()> show_all_layers_in_active_tab;
        Function<void()> reset_raster_counts_in_active_tab;
        // Answers false where no tab can be picked from.
        Function<bool(CompositorLayerPicker)> start_picking_layer_in_active_tab;
        Function<void()> stop_picking_layer_in_active_tab;
    };

    explicit CompositorLayersWindow(Source);

    Function<void()> on_close;

private:
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual void done(int result) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

    void refresh();
    void tell_tab_of_selected_layers();
    void show_details_of_selected_layer();
    void set_hovered_layer(Optional<u64> layer_id);
    void set_picks_layer_on_page(bool);
    void show_only_selected_layers();
    void show_all_layers();
    Vector<u64> ids_of_selected_layers() const;
    Compositing::CompositorLayerDescription const* shown_layer_with_id(u64 layer_id) const;
    Optional<u64> id_of_layer_to_pick_at(Gfx::IntPoint) const;

    Source m_source;
    Optional<Compositing::CompositorLayersDescription> m_shown_description;
    HashMap<u64, QTreeWidgetItem*> m_item_by_layer_id;
    Vector<u64> m_ids_of_selected_layers_told_to_tab;
    Optional<u64> m_id_of_hovered_layer;
    QPushButton* m_pick_button { nullptr };
    QPushButton* m_show_only_selected_button { nullptr };
    QPushButton* m_show_all_button { nullptr };
    QPushButton* m_reset_button { nullptr };
    QTreeWidget* m_layer_tree { nullptr };
    QPlainTextEdit* m_details { nullptr };
    QLabel* m_summary_of_last_frame { nullptr };
    QLabel* m_summary_since_reset { nullptr };
    QLabel* m_tile_highlight_legend { nullptr };
    QTimer* m_refresh_timer { nullptr };
};

}
