/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <adwaita.h>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/window.h>
#include <stdio.h>

class HelloWorld : public Gtk::Window {
public:
    HelloWorld();
    ~HelloWorld() override;

protected:
    // Signal handlers:
    void on_button_clicked();

    // Member widgets:
    Gtk::Button m_button;
};

HelloWorld::HelloWorld()
    : m_button("Hello World") // creates a new button with label "Hello World".
{
    // Sets the margin around the button.
    m_button.set_margin(10);

    // When the button receives the "clicked" signal, it will call the
    // on_button_clicked() method defined below.
    m_button.signal_clicked().connect([this] {
        on_button_clicked();
    });

    // This packs the button into the Window (a container).
    set_child(m_button);
}

HelloWorld::~HelloWorld()
{
}

void HelloWorld::on_button_clicked()
{
    puts("Button clicked!");
}

int main(int argc, char* argv[])
{
    adw_init();
    auto app = Gtk::Application::create("org.example.myapp");
    return app->make_window_and_run<HelloWorld>(argc, argv);
}
