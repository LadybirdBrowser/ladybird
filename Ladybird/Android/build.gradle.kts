import com.android.build.gradle.internal.tasks.factory.dependsOn

plugins {
    id("com.android.application") version "8.4.0"
    id("org.jetbrains.kotlin.android") version "1.9.0"
}

var buildDir = layout.buildDirectory.get()
var cacheDir = System.getenv("SERENITY_CACHE_DIR") ?: "$buildDir/caches"
var sourceDir = layout.projectDirectory.dir("../../").toString()

task<Exec>("buildLagomTools") {
    commandLine = listOf("./BuildLagomTools.sh")
    environment = mapOf(
        "BUILD_DIR" to buildDir,
        "CACHE_DIR" to cacheDir,
        "PATH" to System.getenv("PATH")!!
    )
}
tasks.named("preBuild").dependsOn("buildLagomTools")
tasks.named("prepareKotlinBuildScriptModel").dependsOn("buildLagomTools")

android {
    namespace = "org.serenityos.ladybird"
    compileSdk = 34

    defaultConfig {
        applicationId = "org.serenityos.ladybird"
        minSdk = 30
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        externalNativeBuild {
            cmake {
                // FIXME: Use -std=c++23 once the Android NDK's clang supports that.
                cppFlags += "-std=c++2b"
                arguments += listOf(
                    "-DLagomTools_DIR=$buildDir/lagom-tools-install/share/LagomTools",
                    "-DSERENITY_CACHE_DIR=$cacheDir",
                    "-DVCPKG_ROOT=$sourceDir/Toolchain/Tarballs/vcpkg",
                    "-DVCPKG_TARGET_ANDROID=ON"
                )
            }
        }
        ndk {
            // Specifies the ABI configurations of your native
            // libraries Gradle should build and package with your app.
            abiFilters += listOf("x86_64", "arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.23.0+"
        }
    }

    buildFeatures {
        viewBinding = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    implementation("androidx.swiperefreshlayout:swiperefreshlayout:1.1.0")
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.1.5")
    androidTestImplementation("androidx.test.ext:junit-ktx:1.1.5")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.1")
}
