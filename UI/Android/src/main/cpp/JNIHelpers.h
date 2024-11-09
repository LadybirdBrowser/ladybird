/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/String.h>
#include <jni.h>

namespace Ladybird {
class JavaEnvironment {
public:
    JavaEnvironment(JavaVM* vm)
        : m_vm(vm)
    {
        auto ret = m_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6);
        if (ret == JNI_EDETACHED) {
            ret = m_vm->AttachCurrentThread(&m_env, nullptr);
            VERIFY(ret == JNI_OK);
            m_did_attach_thread = true;
        } else if (ret == JNI_EVERSION) {
            VERIFY_NOT_REACHED();
        } else {
            VERIFY(ret == JNI_OK);
        }

        VERIFY(m_env != nullptr);
    }

    ~JavaEnvironment()
    {
        if (m_did_attach_thread)
            m_vm->DetachCurrentThread();
    }

    JNIEnv* get() const { return m_env; }

    jstring jstring_from_ak_string(String const& str);

private:
    JavaVM* m_vm = nullptr;
    JNIEnv* m_env = nullptr;
    bool m_did_attach_thread = false;
};
}
extern JavaVM* global_vm;
