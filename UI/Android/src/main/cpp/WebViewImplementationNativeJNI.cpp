/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WebViewImplementationNative.h"
#include <AK/String.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <jni.h>

using namespace Ladybird;

jclass WebViewImplementationNative::global_class_reference;
jmethodID WebViewImplementationNative::bind_webcontent_method;
jmethodID WebViewImplementationNative::invalidate_layout_method;
jmethodID WebViewImplementationNative::on_load_start_method;
jmethodID WebViewImplementationNative::on_load_finish_method;
jmethodID WebViewImplementationNative::on_title_change_method;
jmethodID WebViewImplementationNative::on_url_change_method;
jmethodID WebViewImplementationNative::on_find_in_page_method;
jmethodID WebViewImplementationNative::on_link_hover_method;

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_00024Companion_nativeClassInit(JNIEnv*, jobject /* thiz */);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_00024Companion_nativeClassInit(JNIEnv* env, jobject /* thiz */)
{
    auto local_class = env->FindClass("org/serenityos/ladybird/WebViewImplementation");
    if (!local_class)
        TODO();
    WebViewImplementationNative::global_class_reference = reinterpret_cast<jclass>(env->NewGlobalRef(local_class));
    env->DeleteLocalRef(local_class);

    auto method = env->GetMethodID(WebViewImplementationNative::global_class_reference, "bindWebContentService", "(I)V");
    if (!method)
        TODO();
    WebViewImplementationNative::bind_webcontent_method = method;

    method = env->GetMethodID(WebViewImplementationNative::global_class_reference, "invalidateLayout", "()V");
    if (!method)
        TODO();
    WebViewImplementationNative::invalidate_layout_method = method;

    method = env->GetMethodID(WebViewImplementationNative::global_class_reference, "onLoadStart", "(Ljava/lang/String;Z)V");
    if (!method)
        TODO();
    WebViewImplementationNative::on_load_start_method = method;

    method = env->GetMethodID(WebViewImplementationNative::global_class_reference, "onLoadFinish", "(Ljava/lang/String;)V");
    if (!method)
        TODO();
    WebViewImplementationNative::on_load_finish_method = method;

    method = env->GetMethodID(WebViewImplementationNative::global_class_reference, "onTitleChange", "(Ljava/lang/String;)V");
    if (!method)
        TODO();
    WebViewImplementationNative::on_title_change_method = method;

    method = env->GetMethodID(WebViewImplementationNative::global_class_reference, "onUrlChange", "(Ljava/lang/String;)V");
    if (!method)
        TODO();
    WebViewImplementationNative::on_url_change_method = method;

    method = env->GetMethodID(WebViewImplementationNative::global_class_reference, "onFindInPage", "(II)V");
    if (!method)
        TODO();
    WebViewImplementationNative::on_find_in_page_method = method;

    method = env->GetMethodID(WebViewImplementationNative::global_class_reference, "onLinkHover", "(Ljava/lang/String;)V");
    if (!method)
        TODO();
    WebViewImplementationNative::on_link_hover_method = method;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeObjectInit(JNIEnv*, jobject);

extern "C" JNIEXPORT jlong JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeObjectInit(JNIEnv* env, jobject thiz)
{
    auto ref = env->NewGlobalRef(thiz);
    auto instance = reinterpret_cast<jlong>(new WebViewImplementationNative(ref));
    return instance;
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeObjectDispose(JNIEnv*, jobject /* thiz */, jlong);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeObjectDispose(JNIEnv* env, jobject /* thiz */, jlong instance)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    env->DeleteGlobalRef(impl->java_instance());
    delete impl;
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeDrawIntoBitmap(JNIEnv*, jobject /* thiz */, jlong, jobject);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeDrawIntoBitmap(JNIEnv* env, jobject /* thiz */, jlong instance, jobject bitmap)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);

    AndroidBitmapInfo bitmap_info = {};
    void* pixels = nullptr;
    AndroidBitmap_getInfo(env, bitmap, &bitmap_info);
    AndroidBitmap_lockPixels(env, bitmap, &pixels);
    if (pixels)
        impl->paint_into_bitmap(pixels, bitmap_info);

    AndroidBitmap_unlockPixels(env, bitmap);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeSetViewportGeometry(JNIEnv*, jobject /* thiz */, jlong, jint, jint);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeSetViewportGeometry(JNIEnv*, jobject /* thiz */, jlong instance, jint w, jint h)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->set_viewport_geometry(w, h);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeLoadURL(JNIEnv*, jobject /* thiz */, jlong, jstring);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeLoadURL(JNIEnv* env, jobject /* thiz */, jlong instance, jstring url)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    char const* raw_url = env->GetStringUTFChars(url, nullptr);
    auto ak_url = URL::create_with_url_or_path(StringView { raw_url, strlen(raw_url) });
    env->ReleaseStringUTFChars(url, raw_url);
    impl->load(ak_url.release_value());
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeReload(JNIEnv*, jobject /* thiz */, jlong);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeReload(JNIEnv*, jobject /* thiz */, jlong instance)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->reload();
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeTraverseHistory(JNIEnv*, jobject /* thiz */, jlong, jint);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeTraverseHistory(JNIEnv*, jobject /* thiz */, jlong instance, jint delta)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->traverse_the_history_by_delta(delta);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeSetDevicePixelRatio(JNIEnv*, jobject /* thiz */, jlong instance, jfloat);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeSetDevicePixelRatio(JNIEnv*, jobject /* thiz */, jlong instance, jfloat ratio)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->set_device_pixel_ratio(ratio);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeMouseEvent(JNIEnv*, jobject /* thiz */, jlong, jint, jfloat, jfloat, jfloat, jfloat);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeMouseEvent(JNIEnv*, jobject /* thiz */, jlong instance, jint event_type, jfloat x, jfloat y, jfloat raw_x, jfloat raw_y)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);

    Web::MouseEvent::Type web_event_type;

    // These integers are defined in Android's MotionEvent.
    // See https://developer.android.com/reference/android/view/MotionEvent#constants_1
    if (event_type == 0) {
        // MotionEvent.ACTION_DOWN
        web_event_type = Web::MouseEvent::Type::MouseDown;
    } else if (event_type == 1) {
        // MotionEvent.ACTION_UP
        web_event_type = Web::MouseEvent::Type::MouseUp;
    } else if (event_type == 2) {
        // MotionEvent.ACTION_MOVE
        web_event_type = Web::MouseEvent::Type::MouseMove;
    } else {
        // Unknown event type, default to MouseUp
        web_event_type = Web::MouseEvent::Type::MouseUp;
    }

    impl->mouse_event(web_event_type, x, y, raw_x, raw_y);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeFindInPage(JNIEnv*, jobject, jlong, jstring, jboolean);
extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeFindNext(JNIEnv*, jobject, jlong);
extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeFindPrevious(JNIEnv*, jobject, jlong);
extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeZoomIn(JNIEnv*, jobject, jlong);
extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeZoomOut(JNIEnv*, jobject, jlong);
extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeZoomReset(JNIEnv*, jobject, jlong);
extern "C" JNIEXPORT jdouble JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeZoomLevel(JNIEnv*, jobject, jlong);
extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeSetPreferredColorScheme(JNIEnv*, jobject, jlong, jint);
extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeRunJavascript(JNIEnv*, jobject, jlong, jstring);
extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeSelectAll(JNIEnv*, jobject, jlong);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeFindInPage(JNIEnv* env, jobject /* thiz */, jlong instance, jstring query, jboolean case_sensitive)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    char const* raw = env->GetStringUTFChars(query, nullptr);
    auto ak_query = MUST(String::from_utf8(StringView { raw, strlen(raw) }));
    env->ReleaseStringUTFChars(query, raw);
    auto sensitivity = case_sensitive == JNI_TRUE ? AK::CaseSensitivity::CaseSensitive : AK::CaseSensitivity::CaseInsensitive;
    impl->find_in_page(ak_query, sensitivity);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeFindNext(JNIEnv*, jobject /* thiz */, jlong instance)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->find_in_page_next_match();
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeFindPrevious(JNIEnv*, jobject /* thiz */, jlong instance)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->find_in_page_previous_match();
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeZoomIn(JNIEnv*, jobject /* thiz */, jlong instance)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->zoom_in();
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeZoomOut(JNIEnv*, jobject /* thiz */, jlong instance)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->zoom_out();
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeZoomReset(JNIEnv*, jobject /* thiz */, jlong instance)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->reset_zoom();
}

extern "C" JNIEXPORT jdouble JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeZoomLevel(JNIEnv*, jobject /* thiz */, jlong instance)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    return impl->zoom_level();
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeSetPreferredColorScheme(JNIEnv*, jobject /* thiz */, jlong instance, jint scheme)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    Web::CSS::PreferredColorScheme css_scheme;
    switch (scheme) {
    case 1:
        css_scheme = Web::CSS::PreferredColorScheme::Light;
        break;
    case 2:
        css_scheme = Web::CSS::PreferredColorScheme::Dark;
        break;
    case 0:
    default:
        css_scheme = Web::CSS::PreferredColorScheme::Auto;
        break;
    }
    impl->set_preferred_color_scheme(css_scheme);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeRunJavascript(JNIEnv* env, jobject /* thiz */, jlong instance, jstring js)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    char const* raw = env->GetStringUTFChars(js, nullptr);
    auto ak_js = MUST(String::from_utf8(StringView { raw, strlen(raw) }));
    env->ReleaseStringUTFChars(js, raw);
    impl->run_javascript(ak_js);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_WebViewImplementation_nativeSelectAll(JNIEnv*, jobject /* thiz */, jlong instance)
{
    auto* impl = reinterpret_cast<WebViewImplementationNative*>(instance);
    impl->select_all();
}
