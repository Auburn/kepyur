plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// shell.nix provides the NDK and the mipsel binutils wifi-sdr needs to assemble the chip's streamer
val ndkVer: String = System.getenv("KEPYUR_NDK_VERSION") ?: "28.2.13676358"
val mipsPrefix: String = System.getenv("KEPYUR_MIPS_PREFIX") ?: "mipsel-linux-gnu-"

// the Android framework jar MenuScreenshotTest runs on (see testOptions)
val robolectricSdk: Configuration by configurations.creating

android {
    namespace = "com.kepyur.app"
    compileSdk = 35
    buildToolsVersion = "35.0.1"
    ndkVersion = ndkVer

    defaultConfig {
        applicationId = "com.kepyur.app"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"
        // phones are arm64; -Pabi=x86_64 builds for an emulator on a PC
        ndk { abiFilters += listOf((findProperty("abi") as String?) ?: "arm64-v8a") }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DMIPS_AS=${mipsPrefix}as",
                    "-DMIPS_LD=${mipsPrefix}ld",
                    "-DMIPS_OBJCOPY=${mipsPrefix}objcopy",
                    "-DDEVOURER_LOG_MAX_LEVEL=INFO",
                )
                targets += listOf("kepyur")
            }
        }
    }
    externalNativeBuild {
        cmake {
            path = file("../../native/CMakeLists.txt")
            version = "3.31.6"
        }
    }
    buildTypes {
        debug {
            // the DSP must run optimised even in debug builds
            externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=Release" } }
        }
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            all { test ->
                // MenuScreenshotTest writes its PNGs here (gradle testDebugUnitTest -Pshots=/some/dir)
                (findProperty("shots") as String?)?.let { test.systemProperty("kepyur.shots", it) }
                test.outputs.upToDateWhen { false }
                // Robolectric's own downloader gets HTTP 429 from Maven Central's mirror here: let Gradle fetch the
                // Android 15 framework jar and run Robolectric offline against it
                test.inputs.files(robolectricSdk)
                test.doFirst {
                    test.systemProperty("robolectric.offline", "true")
                    test.systemProperty("robolectric.dependency.dir", robolectricSdk.singleFile.parent)
                }
            }
        }
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.17")
    robolectricSdk("org.robolectric:android-all-instrumented:15-robolectric-13954326-i7") { isTransitive = false }
}
