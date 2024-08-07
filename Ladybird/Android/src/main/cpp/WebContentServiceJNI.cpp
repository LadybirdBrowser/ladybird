/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "JNIHelpers.h"
#include "LadybirdServiceBase.h"
#include "WebContentService.h"
#include <jni.h>

jobject global_instance;
jclass global_class_reference;
jmethodID bind_request_server_method;
jmethodID bind_image_decoder_method;

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebContentService_nativeInit(JNIEnv*, jobject);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebContentService_nativeInit(JNIEnv* env, jobject thiz)
{
    global_instance = env->NewGlobalRef(thiz);

    auto local_class = env->FindClass("org/serenityos/ladybird/WebContentService");
    if (!local_class)
        TODO();
    global_class_reference = reinterpret_cast<jclass>(env->NewGlobalRef(local_class));
    env->DeleteLocalRef(local_class);

    auto method = env->GetMethodID(global_class_reference, "bindRequestServer", "(I)V");
    if (!method)
        TODO();
    bind_request_server_method = method;

    method = env->GetMethodID(global_class_reference, "bindImageDecoder", "(I)V");
    if (!method)
        TODO();
    bind_image_decoder_method = method;
}

void bind_request_server_java(int ipc_socket)
{
    Ladybird::JavaEnvironment env(global_vm);
    env.get()->CallVoidMethod(global_instance, bind_request_server_method, ipc_socket);
}

void bind_image_decoder_java(int ipc_socket)
{
    Ladybird::JavaEnvironment env(global_vm);
    env.get()->CallVoidMethod(global_instance, bind_image_decoder_method, ipc_socket);
}
