/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibWebCommon/Page/TextEditingCommand.h>
#include <QLineEdit>

#ifdef AK_OS_MACOS
#    include <UI/Qt/NativeTextEditingBindings.h>
#endif

namespace Ladybird {

class NativeTextEditingLineEdit : public QLineEdit {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit NativeTextEditingLineEdit(QWidget* parent = nullptr);

protected:
    virtual void focusInEvent(QFocusEvent*) override;
    virtual void keyPressEvent(QKeyEvent*) override;
    bool handle_native_text_editing_key(QKeyEvent&);
    void apply_text_editing_command(Web::TextEditingCommand const&);
    virtual void native_text_editing_command_will_execute(Web::TextEditingCommand const&) { }

private:
#ifdef AK_OS_MACOS
    NativeTextEditingBindings m_native_text_editing_bindings;
#endif
};

}
