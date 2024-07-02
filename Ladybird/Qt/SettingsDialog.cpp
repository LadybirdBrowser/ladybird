/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SettingsDialog.h"
#include "Settings.h"
#include "StringUtils.h"
#include <LibURL/URL.h>
#include <LibWebView/SearchEngine.h>
#include <QLabel>
#include <QMenu>

namespace Ladybird {

SettingsDialog::SettingsDialog(QMainWindow* window)
    : QDialog(window)
    , m_window(window)
{
    m_layout = new QFormLayout(this);

    m_enable_search = new QCheckBox(this);
    m_enable_search->setChecked(Settings::the()->enable_search());

    m_search_engine_dropdown = new QPushButton(this);
    m_search_engine_dropdown->setText(qstring_from_ak_string(Settings::the()->search_engine().name));
    m_search_engine_dropdown->setMaximumWidth(200);

    m_enable_autocomplete = new QCheckBox(this);
    m_enable_autocomplete->setChecked(Settings::the()->enable_autocomplete());

    m_autocomplete_engine_dropdown = new QPushButton(this);
    m_autocomplete_engine_dropdown->setText(Settings::the()->autocomplete_engine().name);
    m_autocomplete_engine_dropdown->setMaximumWidth(200);

    m_new_tab_page = new QLineEdit(this);
    m_new_tab_page->setText(Settings::the()->new_tab_page());
    QObject::connect(m_new_tab_page, &QLineEdit::textChanged, this, [this] {
        auto url_string = ak_string_from_qstring(m_new_tab_page->text());
        m_new_tab_page->setStyleSheet(URL::URL(url_string).is_valid() ? "" : "border: 1px solid red;");
    });
    QObject::connect(m_new_tab_page, &QLineEdit::editingFinished, this, [this] {
        auto url_string = ak_string_from_qstring(m_new_tab_page->text());
        if (URL::URL(url_string).is_valid())
            Settings::the()->set_new_tab_page(m_new_tab_page->text());
    });
    QObject::connect(m_new_tab_page, &QLineEdit::returnPressed, this, [this] {
        close();
    });

    m_enable_do_not_track = new QCheckBox(this);
    m_enable_do_not_track->setChecked(Settings::the()->enable_do_not_track());
    QObject::connect(m_enable_do_not_track, &QCheckBox::stateChanged, this, [&](int state) {
        Settings::the()->set_enable_do_not_track(state == Qt::Checked);
    });

    setup_search_engines();

    m_layout->addRow(new QLabel("Page on New Tab", this), m_new_tab_page);
    m_layout->addRow(new QLabel("Enable Search", this), m_enable_search);
    m_layout->addRow(new QLabel("Search Engine", this), m_search_engine_dropdown);
    m_layout->addRow(new QLabel("Enable Autocomplete", this), m_enable_autocomplete);
    m_layout->addRow(new QLabel("Autocomplete Engine", this), m_autocomplete_engine_dropdown);
    m_layout->addRow(new QLabel("Send web sites a \"Do Not Track\" request", this), m_enable_do_not_track);

    setWindowTitle("Settings");
    setLayout(m_layout);
    resize(600, 250);
}

void SettingsDialog::setup_search_engines()
{
    // FIXME: These should be centralized in LibWebView.
    Vector<Settings::EngineProvider> autocomplete_engines = {
        { "DuckDuckGo", "https://duckduckgo.com/ac/?q={}" },
        { "Google", "https://www.google.com/complete/search?client=chrome&q={}" },
        { "Yahoo", "https://search.yahoo.com/sugg/gossip/gossip-us-ura/?output=sd1&command={}" },
    };

    QMenu* search_engine_menu = new QMenu(this);
    for (auto const& search_engine : WebView::search_engines()) {
        auto search_engine_name = qstring_from_ak_string(search_engine.name);
        QAction* action = new QAction(search_engine_name, this);

        connect(action, &QAction::triggered, this, [&, search_engine_name = std::move(search_engine_name)]() {
            Settings::the()->set_search_engine(search_engine);
            m_search_engine_dropdown->setText(search_engine_name);
        });

        search_engine_menu->addAction(action);
    }
    m_search_engine_dropdown->setMenu(search_engine_menu);
    m_search_engine_dropdown->setEnabled(Settings::the()->enable_search());

    QMenu* autocomplete_engine_menu = new QMenu(this);
    for (auto& autocomplete_engine : autocomplete_engines) {
        QAction* action = new QAction(autocomplete_engine.name, this);
        connect(action, &QAction::triggered, this, [&, autocomplete_engine] {
            Settings::the()->set_autocomplete_engine(autocomplete_engine);
            m_autocomplete_engine_dropdown->setText(autocomplete_engine.name);
        });
        autocomplete_engine_menu->addAction(action);
    }
    m_autocomplete_engine_dropdown->setMenu(autocomplete_engine_menu);
    m_autocomplete_engine_dropdown->setEnabled(Settings::the()->enable_autocomplete());

    connect(m_enable_search, &QCheckBox::stateChanged, this, [&](int state) {
        Settings::the()->set_enable_search(state == Qt::Checked);
        m_search_engine_dropdown->setEnabled(state == Qt::Checked);
    });

    connect(m_enable_autocomplete, &QCheckBox::stateChanged, this, [&](int state) {
        Settings::the()->set_enable_autocomplete(state == Qt::Checked);
        m_autocomplete_engine_dropdown->setEnabled(state == Qt::Checked);
    });
}

}
