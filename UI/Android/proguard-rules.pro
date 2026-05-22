# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
#-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile

-keep class org.libsdl.app.** {
	*;
}

-keep class org.serenityos.ladybird.LadybirdActivity {
	*;
}

-keep class org.serenityos.ladybird.SettingsActivity {
	*;
}

-keep class org.serenityos.ladybird.WebViewImplementation {
	void bindWebContentService(int);
	void invalidateLayout();
	void onLoadStart(java.lang.String, boolean);
	void onLoadFinish(java.lang.String);
	void onTitleChange(java.lang.String);
	void onUrlChange(java.lang.String);
	void onFindInPage(int, int);
	void onLinkHover(java.lang.String);
}

-keep class org.serenityos.ladybird.WebContentService {
	void bindRequestServer(int);
	void bindImageDecoder(int);
}

-keep class org.serenityos.ladybird.TimerExecutorService {
	long registerTimer(org.serenityos.ladybird.TimerExecutorService$Timer, boolean, long);
	void unregisterTimer(long);
}

-keep class org.serenityos.ladybird.TimerExecutorService$Timer {
	<init>(long);
}
