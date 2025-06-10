/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ALooperEventLoopImplementation.h"
#include "JNIHelpers.h"
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <AK/OwnPtr.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Application.h>
#include <LibWebView/Utilities.h>
#include <jni.h>

JavaVM* global_vm;
static OwnPtr<WebView::Application> s_application;
static OwnPtr<Core::EventLoop> s_main_event_loop;
static jobject s_java_instance;
static jmethodID s_schedule_event_loop_method;

class Application : public WebView::Application {
    WEB_VIEW_APPLICATION(Application);

public:
    explicit Application();
};

Application::Application() = default;

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_initNativeCode(JNIEnv*, jobject, jstring, jstring, jobject, jstring);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_initNativeCode(JNIEnv* env, jobject thiz, jstring resource_dir, jstring tag_name, jobject timer_service, jstring user_dir)
{
    char const* raw_resource_dir = env->GetStringUTFChars(resource_dir, nullptr);
    WebView::s_ladybird_resource_root = raw_resource_dir;
    env->ReleaseStringUTFChars(resource_dir, raw_resource_dir);

    // While setting XDG environment variables in order to store user data may seem silly
    // but in our case it seems to be the most rational idea.
    char const* raw_user_dir = env->GetStringUTFChars(user_dir, nullptr);
    setenv("XDG_CONFIG_HOME", ByteString::formatted("{}/config", raw_user_dir).characters(), 1);
    setenv("XDG_DATA_HOME", ByteString::formatted("{}/userdata", raw_user_dir).characters(), 1);
    env->ReleaseStringUTFChars(user_dir, raw_user_dir);

    char const* raw_tag_name = env->GetStringUTFChars(tag_name, nullptr);
    AK::set_log_tag_name(raw_tag_name);
    env->ReleaseStringUTFChars(tag_name, raw_tag_name);

    dbgln("Set resource dir to {}", WebView::s_ladybird_resource_root);

    auto file_or_error = Core::System::open(MUST(String::formatted("{}/res/icons/48x48/app-browser.png", WebView::s_ladybird_resource_root)), O_RDONLY);
    if (file_or_error.is_error()) {
        dbgln("No resource files, perhaps extracting went wrong?");
    } else {
        dbgln("Found app-browser.png");
        dbgln("Hopefully no developer changed the asset files and expected them to be re-extracted!");
    }

    env->GetJavaVM(&global_vm);
    VERIFY(global_vm);

    s_java_instance = env->NewGlobalRef(thiz);
    jclass clazz = env->GetObjectClass(s_java_instance);
    VERIFY(clazz);
    s_schedule_event_loop_method = env->GetMethodID(clazz, "scheduleEventLoop", "()V");
    VERIFY(s_schedule_event_loop_method);
    env->DeleteLocalRef(clazz);

    jobject timer_service_ref = env->NewGlobalRef(timer_service);

    auto* event_loop_manager = new Ladybird::ALooperEventLoopManager(timer_service_ref);
    event_loop_manager->on_did_post_event = [] {
        Ladybird::JavaEnvironment env(global_vm);
        env.get()->CallVoidMethod(s_java_instance, s_schedule_event_loop_method);
    };
    Core::EventLoopManager::install(*event_loop_manager);
    s_main_event_loop = make<Core::EventLoop>();

    // The strings cannot be empty
    Main::Arguments arguments = {
        .argc = 0,
        .argv = nullptr,
        .strings = Span<StringView> { new StringView("ladybird"sv), 1 }
    };

    // FIXME: We are not making use of this Application object to track our processes.
    // So, right now, the Application's ProcessManager is constantly empty.
    // (However, LibWebView depends on an Application object existing, so we do have to actually create one.)
    s_application = Application::create(arguments).release_value_but_fixme_should_propagate_errors();
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_execMainEventLoop(JNIEnv*, jobject /* thiz */);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_execMainEventLoop(JNIEnv*, jobject /* thiz */)
{
    if (s_main_event_loop) {
        s_main_event_loop->pump(Core::EventLoop::WaitMode::PollForEvents);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_disposeNativeCode(JNIEnv*, jobject /* thiz */);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_disposeNativeCode(JNIEnv* env, jobject /* thiz */)
{
    s_main_event_loop = nullptr;
    s_schedule_event_loop_method = nullptr;
    s_application = nullptr;
    env->DeleteGlobalRef(s_java_instance);

    delete &Core::EventLoopManager::the();
}
