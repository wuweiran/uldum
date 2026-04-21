import java.util.Properties

plugins {
    id("com.android.application")
}

// ── Flavor parameterization ───────────────────────────────────────────────
// Two product flavors:
//   - dev:  engine-dev APK. Bundles every engine test map from repo's maps/.
//           Fixed applicationId clan.midnight.uldum_dev. Studio's Run button
//           is the canonical workflow — no PowerShell pre-step required.
//   - game: shipping APK. Parameterized from a game project via -P flags
//           set by scripts\build_android_game.ps1. Defaults fall back to
//           sample_game so Studio can still open the project cleanly.
val projectRoot = rootProject.projectDir.parentFile.parentFile
val engineMapsDir = projectRoot.resolve("maps")
val engineBuildBin = projectRoot.resolve("build/bin")

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
    // so the `.MainActivity` in AndroidManifest resolves here regardless of
    // which flavor's applicationId is published.
    namespace = "clan.midnight.uldum"
    compileSdk = 36
    buildToolsVersion = "36.0.0"
    ndkVersion = "29.0.14206865"

    buildFeatures {
        prefab = true
    }

    defaultConfig {
        // minSdk 33 (Android 13) is what the NDK's libvulkan.so stub needs to
        // export the Vulkan 1.3 entry points the engine uses (dynamic
        // rendering, synchronization2, maintenance4 — all 1.3 core, not
        // available as linkable symbols in the API 29/30/31/32 stubs).
        minSdk = 33
        targetSdk = 36
        versionCode = 1

        // arm64-v8a for real devices; x86_64 so the Android emulator works on a Windows/Linux host.
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_PLATFORM=android-33",
                )
                cppFlags += "-std=c++23"
            }
        }
    }

    flavorDimensions += "variant"
    productFlavors {
        create("dev") {
            dimension = "variant"
            applicationId = "clan.midnight.uldum_dev"
            versionName = "0.1.0"
            manifestPlaceholders["appLabel"] = "Uldum Dev"
            externalNativeBuild {
                cmake {
                    // Flips android_main.cpp to the dev-mode path: hardcoded
                    // test_map, no game.json read.
                    arguments += "-DULDUM_ANDROID_DEV=1"
                }
            }
        }
        create("game") {
            dimension = "variant"
            applicationId = ulApplicationId
            versionName = ulVersionName
            manifestPlaceholders["appLabel"] = ulAppName
            externalNativeBuild {
                cmake {
                    arguments += "-DULDUM_GAME_PROJECT_DIR=$gameProjectDir"
                }
            }
        }
    }

    // Per-flavor res/ sources:
    //   - game: launcher icons from the game project's branding/android/
    //   - dev:  no res overlay; icon attribute is game-flavor-only (see
    //           src/game/AndroidManifest.xml), so dev needs no icons
    sourceSets["game"].res.srcDirs("$gameProjectDir/branding/android")

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.28.0+"
        }
    }

    packaging {
        jniLibs {
            keepDebugSymbols += "**/*.so"
        }
    }

    // Release signing for the `game` flavor only. Dev releases are rare; if
    // you need a signed dev APK you can point dev at a signing config too.
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
}

dependencies {
    implementation("androidx.games:games-activity:4.4.1")
    implementation("androidx.appcompat:appcompat:1.7.1")
}

// ── Asset staging ────────────────────────────────────────────────────────
// dev flavor: Gradle invokes uldum_pack.exe directly to stage every engine
// test map into src/dev/assets/maps/ plus engine.uldpak. No PowerShell
// pre-step — Studio's Run button is self-sufficient.
//
// game flavor: scripts\build_android_game.ps1 pre-stages assets into
// src/game/assets/ (parses game.json, packs listed maps). Gradle just
// validates they're present.

val uldumPackExe = engineBuildBin.resolve("uldum_pack.exe")
val engineUldpak = engineBuildBin.resolve("engine.uldpak")

val stageDevAssets by tasks.registering {
    description = "Pack every engine test map into src/dev/assets/ for the dev APK"
    val devAssetsDir = layout.projectDirectory.dir("src/dev/assets").asFile
    val devMapsDir = devAssetsDir.resolve("maps")

    doFirst {
        if (!uldumPackExe.exists()) {
            throw GradleException(
                "$uldumPackExe not found — run scripts\\build.ps1 first to produce uldum_pack.exe."
            )
        }
        if (!engineUldpak.exists()) {
            throw GradleException(
                "$engineUldpak not found — run scripts\\build.ps1 first."
            )
        }
        if (!engineMapsDir.isDirectory) {
            throw GradleException("Engine maps directory missing: $engineMapsDir")
        }

        devAssetsDir.mkdirs()
        devMapsDir.deleteRecursively()
        devMapsDir.mkdirs()

        engineUldpak.copyTo(devAssetsDir.resolve("engine.uldpak"), overwrite = true)

        val mapDirs = engineMapsDir.listFiles { f -> f.isDirectory && f.name.endsWith(".uldmap") }
        if (mapDirs.isNullOrEmpty()) {
            throw GradleException("No *.uldmap folders found in $engineMapsDir")
        }
        mapDirs.forEach { mapSrc ->
            val mapDst = devMapsDir.resolve(mapSrc.name)
            // ProcessBuilder instead of Gradle's exec{} — the script-scope
            // exec block was removed from current Gradle Kotlin DSL in favor
            // of injected ExecOperations (which doesn't work cleanly from
            // doFirst). ProcessBuilder is plain JDK and always available.
            val proc = ProcessBuilder(
                uldumPackExe.absolutePath, "pack",
                mapSrc.absolutePath, mapDst.absolutePath
            ).redirectErrorStream(true).start()
            val output = proc.inputStream.bufferedReader().readText()
            val exitCode = proc.waitFor()
            if (exitCode != 0) {
                throw GradleException("uldum_pack failed on ${mapSrc.name} (exit $exitCode)\n$output")
            }
        }
    }
}

val validateGameAssets by tasks.registering {
    description = "Check that engine.uldpak + game.json + maps/ are staged into src/game/assets/"
    doFirst {
        val assetsDir = layout.projectDirectory.dir("src/game/assets").asFile
        val missing = listOf("engine.uldpak", "game.json").filter {
            !assetsDir.resolve(it).exists()
        }
        if (missing.isNotEmpty()) {
            throw GradleException(
                "Missing ${missing.joinToString()} in $assetsDir — run scripts\\build_android_game.ps1 " +
                "(it pre-populates assets before invoking Gradle)."
            )
        }
        val mapsDir = assetsDir.resolve("maps")
        if (!mapsDir.exists() || mapsDir.listFiles().isNullOrEmpty()) {
            throw GradleException(
                "No .uldmap files in $mapsDir — run scripts\\build_android_game.ps1."
            )
        }
    }
}

// Hook each flavor's preBuild to the matching staging task. Gradle generates
// per-flavor/per-buildType task names like preDevDebugBuild, preGameDebugBuild.
afterEvaluate {
    listOf("Debug", "Release").forEach { buildType ->
        tasks.findByName("preDev${buildType}Build")?.dependsOn(stageDevAssets)
        tasks.findByName("preGame${buildType}Build")?.dependsOn(validateGameAssets)
    }
}
