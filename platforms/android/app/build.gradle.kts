import java.util.Properties

plugins {
    id("com.android.application")
}

// ── Game project parameterization ────────────────────────────────────────
// build_android.ps1 passes these via -P flags. Defaults fall back to
// sample_game so `gradlew assembleDebug` works from Android Studio without
// the PowerShell wrapper (Studio reads the project without the per-project
// sync).
val projectRoot = rootProject.projectDir.parentFile.parentFile
val gameProjectDir = providers.gradleProperty("ulGameProjectDir")
    .orElse(projectRoot.resolve("sample_game").absolutePath)
    .get()
val ulApplicationId = providers.gradleProperty("ulApplicationId")
    .orElse("com.m1knight.uldum_sample")
    .get()
val ulAppName = providers.gradleProperty("ulAppName")
    .orElse("Uldum Sample")
    .get()
val ulVersionName = providers.gradleProperty("ulVersionName")
    .orElse("0.1.0")
    .get()

android {
    // Engine namespace — fixed. MainActivity.kt lives in clan.midnight.uldum
    // so the `.MainActivity` in AndroidManifest resolves here. applicationId
    // (the published identifier) is what changes per game — it's what the
    // Play Store sees.
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
        applicationId = ulApplicationId
        // minSdk 33 (Android 13) is what the NDK's libvulkan.so stub needs to
        // export the Vulkan 1.3 entry points the engine uses (dynamic
        // rendering, synchronization2, maintenance4 — all 1.3 core, not
        // available as linkable symbols in the API 29/30/31/32 stubs).
        // Supporting older devices would require either volk-style dynamic
        // loading with KHR-extension fallbacks, or a separate GLES backend.
        minSdk = 33
        targetSdk = 36
        versionCode = 1
        versionName = ulVersionName

        // Expand ${appLabel} inside AndroidManifest.xml. Lets each game set
        // its own launcher label without hardcoding it here.
        manifestPlaceholders["appLabel"] = ulAppName

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
                    "-DULDUM_GAME_PROJECT_DIR=$gameProjectDir",
                )
                cppFlags += "-std=c++23"
            }
        }
    }

    // Merge the game project's branding/android/ into res/ so launcher icons
    // (mipmap-mdpi/ic_launcher.png, etc.) come from the game project — not
    // committed inside the engine repo's Android scaffolding.
    sourceSets["main"].res.srcDirs("$gameProjectDir/branding/android", "src/main/res")

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

    // Release signing: read keystore config from <project>/keystore.properties
    // if present. Gitignored per-project — the example file is committed, the
    // real one isn't. If absent, only `debug` builds work; release builds fail
    // with a clear error from build_android.ps1 before Gradle ever runs.
    val keystorePropsFile = file("$gameProjectDir/keystore.properties")
    if (keystorePropsFile.exists()) {
        val keystoreProps = Properties().apply {
            keystorePropsFile.inputStream().use { load(it) }
        }
        signingConfigs {
            create("release") {
                // storeFile path is relative to the game project directory.
                storeFile = file("$gameProjectDir/${keystoreProps.getProperty("storeFile")}")
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword")
            }
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
            if (keystorePropsFile.exists()) {
                signingConfig = signingConfigs.getByName("release")
            }
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

// ── Assets validation ────────────────────────────────────────────────────
// build_android.ps1 pre-populates src/main/assets/ with engine.uldpak,
// game.json, and maps/*.uldmap using uldum_pack. This check fails the build
// early if those aren't in place (usually because the PowerShell wrapper
// wasn't used, or scripts\build.ps1 hasn't been run to produce uldum_pack).
val validateGameAssets by tasks.registering {
    description = "Check that engine.uldpak + game.json + maps/ are staged into assets/"
    doFirst {
        val assetsDir = layout.projectDirectory.dir("src/main/assets").asFile
        val missing = listOf("engine.uldpak", "game.json").filter {
            !assetsDir.resolve(it).exists()
        }
        if (missing.isNotEmpty()) {
            throw GradleException(
                "Missing ${missing.joinToString()} in $assetsDir — run scripts\\build_android.ps1 " +
                "(it pre-populates assets before invoking Gradle)."
            )
        }
        val mapsDir = assetsDir.resolve("maps")
        if (!mapsDir.exists() || mapsDir.listFiles().isNullOrEmpty()) {
            throw GradleException(
                "No .uldmap files in $mapsDir — run scripts\\build_android.ps1."
            )
        }
    }
}
tasks.named("preBuild") { dependsOn(validateGameAssets) }
