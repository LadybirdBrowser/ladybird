/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/SettingsDialog.h>
#include <UI/Qt/StringUtils.h>

#include <QLabel>
#include <QMenu>

namespace Ladybird {

SettingsDialog::SettingsDialog(QMainWindow* window)
    : QDialog(window)
    , m_window(window)
{
    m_layout = new QFormLayout(this);

    m_preferred_languages = new QLineEdit(this);
    m_preferred_languages->setText(Settings::the()->preferred_languages().join(","));
    QObject::connect(m_preferred_languages, &QLineEdit::editingFinished, this, [this] {
        Settings::the()->set_preferred_languages(m_preferred_languages->text().split(","));
    });
    QObject::connect(m_preferred_languages, &QLineEdit::returnPressed, this, [this] {
        close();
    });

    m_enable_autocomplete = new QCheckBox(this);
    m_enable_autocomplete->setChecked(Settings::the()->enable_autocomplete());

    m_autocomplete_engine_dropdown = new QPushButton(this);
    m_autocomplete_engine_dropdown->setText(Settings::the()->autocomplete_engine().name);
    m_autocomplete_engine_dropdown->setMaximumWidth(200);

    m_enable_do_not_track = new QCheckBox(this);
    m_enable_do_not_track->setChecked(Settings::the()->enable_do_not_track());
#if (QT_VERSION > QT_VERSION_CHECK(6, 7, 0))
    QObject::connect(m_enable_do_not_track, &QCheckBox::checkStateChanged, this, [&](int state) {
#else
    QObject::connect(m_enable_do_not_track, &QCheckBox::stateChanged, this, [&](int state) {
#endif
        Settings::the()->set_enable_do_not_track(state == Qt::Checked);
    });

    m_enable_autoplay = new QCheckBox(this);
    if (WebView::Application::web_content_options().enable_autoplay == WebView::EnableAutoplay::Yes) {
        m_enable_autoplay->setChecked(true);
    } else {
        m_enable_autoplay->setChecked(Settings::the()->enable_autoplay());
    }

#if (QT_VERSION > QT_VERSION_CHECK(6, 7, 0))
    QObject::connect(m_enable_autoplay, &QCheckBox::checkStateChanged, this, [&](int state) {
#else
    QObject::connect(m_enable_autoplay, &QCheckBox::stateChanged, this, [&](int state) {
#endif
        Settings::the()->set_enable_autoplay(state == Qt::Checked);
    });

    setup_autocomplete_engine();

    m_layout->addRow(new QLabel("Preferred Language(s)", this), m_preferred_languages);
    m_layout->addRow(new QLabel("Enable Autocomplete", this), m_enable_autocomplete);
    m_layout->addRow(new QLabel("Autocomplete Engine", this), m_autocomplete_engine_dropdown);
    m_layout->addRow(new QLabel("Send web sites a \"Do Not Track\" request", this), m_enable_do_not_track);
    m_layout->addRow(new QLabel("Enable autoplay on all websites", this), m_enable_autoplay);

    setWindowTitle("Settings");
    setLayout(m_layout);
    resize(600, 250);
}

void SettingsDialog::setup_autocomplete_engine()
{
    // FIXME: These should be centralized in LibWebView.
    Vector<Settings::EngineProvider> autocomplete_engines = {
        { "DuckDuckGo", "https://duckduckgo.com/ac/?q={}" },
        { "Google", "https://www.google.com/complete/search?client=chrome&q={}" },
        { "Yahoo", "https://search.yahoo.com/sugg/gossip/gossip-us-ura/?output=sd1&command={}" },
    };

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

#if (QT_VERSION > QT_VERSION_CHECK(6, 7, 0))
    connect(m_enable_autocomplete, &QCheckBox::checkStateChanged, this, [&](int state) {
#else
    connect(m_enable_autocomplete, &QCheckBox::stateChanged, this, [&](int state) {
#endif
        Settings::the()->set_enable_autocomplete(state == Qt::Checked);
        m_autocomplete_engine_dropdown->setEnabled(state == Qt::Checked);
    });
}

}
