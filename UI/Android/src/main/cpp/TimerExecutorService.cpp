/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ALooperEventLoopImplementation.h"
#include <jni.h>

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_TimerExecutorService_00024Timer_nativeRun(JNIEnv*, jobject /* thiz */, jlong, jlong);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_TimerExecutorService_00024Timer_nativeRun(JNIEnv*, jobject /* thiz */, jlong native_data, jlong id)
{
    auto& event_loop_manager = *reinterpret_cast<Ladybird::ALooperEventLoopManager*>(native_data);
    event_loop_manager.timer_fired(id);
}
