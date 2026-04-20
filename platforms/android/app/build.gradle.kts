plugins {
    id("com.android.application")
}

android {
    // Engine namespace. A shipped game built on this engine would override
    // this (and applicationId) with its own publisher ID — e.g., com.m1knight.<name>.
    namespace = "clan.midnight.uldum"
    compileSdk = 36
    buildToolsVersion = "36.0.0"
    ndkVersion = "29.0.14206865"

    // games-activity 4.x ships prebuilt static libs via prefabs; the native
    // build links game-activity::game-activity_static instead of including
    // GameActivity.cpp directly. Enabled here so the feature is available when
    // the real engine port wires up the GameActivity C++ lifecycle.
    buildFeatures {
        prefab = true
    }

    defaultConfig {
        applicationId = "clan.midnight.uldum"
        // minSdk 33 (Android 13) is what the NDK's libvulkan.so stub needs to
        // export the Vulkan 1.3 entry points the engine uses (dynamic
        // rendering, synchronization2, maintenance4 — all 1.3 core, not
        // available as linkable symbols in the API 29/30/31/32 stubs).
        // Supporting older devices would require either volk-style dynamic
        // loading with KHR-extension fallbacks, or a separate GLES backend.
        minSdk = 33
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        // arm64-v8a for real devices; x86_64 so the Android emulator works on a Windows/Linux host.
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // We reuse our top-level CMakeLists for Android. The android/ wrapper
                // below just adds our root project as a subdirectory.
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_PLATFORM=android-33",
                )
                cppFlags += "-std=c++23"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.28.0+"
        }
    }

    // The engine produces libuldum_game.so; everything else stays internal/static.
    packaging {
        jniLibs {
            // Keep debug symbols in debug builds; strip in release.
            keepDebugSymbols += "**/*.so"
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
            isDebuggable = true
        }
        release {
            isMinifyEnabled = false  // native-heavy app; R8 has little to strip
            isDebuggable = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    // AGP 9.x's built-in Kotlin picks up the JVM target from compileOptions —
    // no separate kotlinOptions block needed.
}

dependencies {
    // GameActivity — the modern AndroidX native activity for games.
    // See https://developer.android.com/games/agdk/game-activity
    implementation("androidx.games:games-activity:4.4.1")
    // GameActivity extends AppCompatActivity; AppCompat isn't a transitive
    // dep so we add it explicitly.
    implementation("androidx.appcompat:appcompat:1.7.1")
}

// ── Bundle desktop-built engine.uldpak + maps into the APK's assets ──────
// Shaders, engine Lua, and map packages are produced by the desktop build
// (scripts\build.bat → build\bin\engine.uldpak + build\bin\maps\*.uldmap).
// This task copies them into src/main/assets so AGP packages them into the
// APK. Runs before assets are merged; fails fast with a clear error if the
// desktop build hasn't been run.
val desktopBin = rootProject.projectDir.parentFile.parentFile.resolve("build/bin")

val copyEngineAssets by tasks.registering(Copy::class) {
    description = "Copy desktop-built engine.uldpak + .uldmap files into app assets"
    doFirst {
        val uldpak = desktopBin.resolve("engine.uldpak")
        if (!uldpak.exists()) {
            throw GradleException(
                "engine.uldpak not found at $uldpak — run scripts\\build.bat on desktop first."
            )
        }
    }
    from(desktopBin.resolve("engine.uldpak"))
    from(desktopBin.resolve("maps")) {
        into("maps")
        include("*.uldmap")
    }
    into(layout.projectDirectory.dir("src/main/assets"))
}

// AGP's asset-merge step needs the files in place before it runs. Hooking
// preBuild is sufficient for both debug and release variants.
tasks.named("preBuild") { dependsOn(copyEngineAssets) }
