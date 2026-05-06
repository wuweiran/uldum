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

val gameProjectDir = providers.gradleProperty("uldGameProjectDir")
    .orElse(projectRoot.resolve("sample_game").absolutePath)
    .get()
val uldApplicationId = providers.gradleProperty("uldApplicationId")
    .orElse("com.m1knight.uldum_sample")
    .get()
val uldAppName = providers.gradleProperty("uldAppName")
    .orElse("Uldum Sample")
    .get()
val uldVersionName = providers.gradleProperty("uldVersionName")
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
            applicationId = uldApplicationId
            versionName = uldVersionName
            manifestPlaceholders["appLabel"] = uldAppName
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
    // AGP deprecated srcDir/srcDirs on AndroidSourceDirectorySet in favor
    // of the mutable `directories` set. Using `.add(file(...))` keeps us
    // current with the recommended API.
    sourceSets["game"].res.directories.add("$gameProjectDir/branding/android")

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
// dev flavor: Gradle does the full pipeline — compiles shaders with glslc,
// stages engine assets, packs engine.uldpak, packs every test map. Only
// hard prereqs: uldum_pack.exe (built once by scripts\build.ps1) and
// VULKAN_SDK / glslc on PATH (for shader compilation). After that
// engine/ source edits round-trip through Studio's Run button without
// needing a desktop rebuild.
//
// game flavor: scripts\build_android_game.ps1 pre-stages assets into
// src/game/assets/ (parses game.json, packs listed maps). Gradle just
// validates they're present.

val uldumPackExe = engineBuildBin.resolve("uldum_pack.exe")
val engineSrcDir = projectRoot.resolve("engine")

// glslc.exe lookup: try VULKAN_SDK first, then assume it's on PATH. Used
// at task-execution time so a missing tool fails the build with a clear
// message rather than a silent no-op.
fun resolveGlslc(): File {
    val sdk = System.getenv("VULKAN_SDK")
    if (sdk != null) {
        val cand = File(sdk).resolve("Bin/glslc.exe")
        if (cand.exists()) return cand
    }
    return File("glslc.exe")  // delegate to PATH lookup; ProcessBuilder will surface the error
}

// Roboto regular: prefer CMake's staged copy if present (no extra path
// resolution), fall back to the FetchContent download dir. Either path
// only exists after `scripts\build.ps1` has run cmake configure at least
// once — that's a one-time prereq, not a per-build one.
fun resolveDefaultFont(): File? {
    val staged = projectRoot.resolve("build/staging/engine/fonts/Roboto-Regular.ttf")
    if (staged.exists()) return staged
    val fetched = projectRoot.resolve("build/_deps/roboto_font-src/Roboto-Regular.ttf")
    if (fetched.exists()) return fetched
    return null
}

val compileEngineShaders by tasks.registering {
    description = "Compile engine GLSL shaders (.vert/.frag/.comp) to SPIR-V via glslc"
    val shaderSrcDir = engineSrcDir.resolve("shaders")
    val shaderOutDir = layout.buildDirectory.dir("dev_engine_shaders").get().asFile

    doFirst {
        if (!shaderSrcDir.isDirectory) {
            throw GradleException("Engine shaders dir missing: $shaderSrcDir")
        }
        val glslc = resolveGlslc()
        shaderOutDir.deleteRecursively()
        shaderOutDir.mkdirs()

        val shaderFiles = shaderSrcDir.listFiles { f ->
            f.isFile && (f.name.endsWith(".vert") || f.name.endsWith(".frag") || f.name.endsWith(".comp"))
        } ?: emptyArray()
        if (shaderFiles.isEmpty()) {
            throw GradleException("No shader sources (*.vert, *.frag, *.comp) in $shaderSrcDir")
        }

        shaderFiles.forEach { shader ->
            val spvOut = shaderOutDir.resolve("${shader.name}.spv")
            val proc = ProcessBuilder(
                glslc.absolutePath, "--target-env=vulkan1.2",
                shader.absolutePath, "-o", spvOut.absolutePath
            ).redirectErrorStream(true).start()
            val output = proc.inputStream.bufferedReader().readText()
            val exitCode = proc.waitFor()
            if (exitCode != 0) {
                throw GradleException(
                    "glslc failed on ${shader.name} (exit $exitCode)\n$output\n" +
                    "Check VULKAN_SDK is set or glslc.exe is on PATH."
                )
            }
        }
    }
}

val stageDevAssets by tasks.registering {
    description = "Re-pack engine.uldpak + every test map into src/dev/assets/ for the dev APK"
    dependsOn(compileEngineShaders)

    val devAssetsDir         = layout.projectDirectory.dir("src/dev/assets").asFile
    val devMapsDir           = devAssetsDir.resolve("maps")
    val gradleEngineStaging  = layout.buildDirectory.dir("dev_engine_staging").get().asFile
    val gradleCompiledShaders = layout.buildDirectory.dir("dev_engine_shaders").get().asFile

    doFirst {
        if (!uldumPackExe.exists()) {
            throw GradleException(
                "$uldumPackExe not found — run scripts\\build.ps1 once to produce uldum_pack.exe."
            )
        }
        if (!engineSrcDir.isDirectory) {
            throw GradleException("Engine source directory missing: $engineSrcDir")
        }
        if (!engineMapsDir.isDirectory) {
            throw GradleException("Engine maps directory missing: $engineMapsDir")
        }
        val fontFile = resolveDefaultFont()
            ?: throw GradleException(
                "Roboto-Regular.ttf not found under build/_deps/ or build/staging/engine/fonts/. " +
                "Run scripts\\build.ps1 once so cmake configure resolves the FetchContent dependency."
            )

        devAssetsDir.mkdirs()
        devMapsDir.deleteRecursively()
        devMapsDir.mkdirs()

        // Re-stage engine assets:
        //   1. engine/ source verbatim (scripts, types, textures, lua, etc.).
        //   2. shaders/ overlaid with our own compiled .spv set (the .glsl
        //      sources copied in step 1 are replaced with .spv so the
        //      runtime loader finds compiled binaries).
        //   3. fonts/Roboto-Regular.ttf from FetchContent / CMake staging.
        gradleEngineStaging.deleteRecursively()
        gradleEngineStaging.mkdirs()
        engineSrcDir.copyRecursively(gradleEngineStaging, overwrite = true)
        val stagingShaders = gradleEngineStaging.resolve("shaders")
        stagingShaders.deleteRecursively()
        stagingShaders.mkdirs()
        gradleCompiledShaders.copyRecursively(stagingShaders, overwrite = true)
        gradleEngineStaging.resolve("fonts").mkdirs()
        fontFile.copyTo(
            gradleEngineStaging.resolve("fonts/Roboto-Regular.ttf"),
            overwrite = true
        )

        // Run uldum_pack. ProcessBuilder instead of Gradle's exec{} — the
        // script-scope exec block was removed from current Gradle Kotlin
        // DSL in favor of injected ExecOperations (which doesn't work
        // cleanly from doFirst). ProcessBuilder is plain JDK and always
        // available.
        fun runPack(srcDir: File, dst: File, label: String) {
            val proc = ProcessBuilder(
                uldumPackExe.absolutePath, "pack",
                srcDir.absolutePath, dst.absolutePath
            ).redirectErrorStream(true).start()
            val output = proc.inputStream.bufferedReader().readText()
            val exitCode = proc.waitFor()
            if (exitCode != 0) {
                throw GradleException("uldum_pack failed on $label (exit $exitCode)\n$output")
            }
        }

        runPack(gradleEngineStaging, devAssetsDir.resolve("engine.uldpak"), "engine")

        val mapDirs = engineMapsDir.listFiles { f -> f.isDirectory && f.name.endsWith(".uldmap") }
        if (mapDirs.isNullOrEmpty()) {
            throw GradleException("No *.uldmap folders found in $engineMapsDir")
        }
        mapDirs.forEach { mapSrc ->
            runPack(mapSrc, devMapsDir.resolve(mapSrc.name), mapSrc.name)
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
