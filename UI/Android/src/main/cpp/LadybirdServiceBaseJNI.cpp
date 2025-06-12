/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdServiceBase.h"
#include <AK/Atomic.h>
#include <AK/Format.h>
#include <LibCore/ResourceImplementationFile.h>
#include <UI/Utilities.h>
#include <jni.h>

JavaVM* global_vm;

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdServiceBase_nativeThreadLoop(JNIEnv*, jobject /* thiz */, jint);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdServiceBase_nativeThreadLoop(JNIEnv*, jobject /* thiz */, jint ipc_socket)
{
    auto ret = service_main(ipc_socket);
    if (ret.is_error()) {
        warnln("Runtime Error: {}", ret.release_error());
    } else {
        outln("Thread exited with code {}", ret.release_value());
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdServiceBase_initNativeCode(JNIEnv*, jobject /* thiz */, jstring, jstring);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdServiceBase_initNativeCode(JNIEnv* env, jobject /* thiz */, jstring resource_dir, jstring tag_name)
{
    static Atomic<bool> s_initialized_flag { false };
    if (s_initialized_flag.exchange(true) == true) {
        // Skip initializing if someone else already started the process at some point in the past
        return;
    }

    env->GetJavaVM(&global_vm);

    char const* raw_resource_dir = env->GetStringUTFChars(resource_dir, nullptr);
    s_ladybird_resource_root = raw_resource_dir;
    env->ReleaseStringUTFChars(resource_dir, raw_resource_dir);
    // FIXME: Use a custom Android version that uses AssetManager to load files.
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::formatted("{}/res", s_ladybird_resource_root))));

    char const* raw_tag_name = env->GetStringUTFChars(tag_name, nullptr);
    AK::set_log_tag_name(raw_tag_name);
    env->ReleaseStringUTFChars(tag_name, raw_tag_name);
}
