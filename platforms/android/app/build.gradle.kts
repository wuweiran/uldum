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
        // minSdk 26 (Android 8.0 Oreo) — covers >98% of active devices and
        // is the floor for guaranteed OpenGL ES 3.2 support across vendors.
        // The engine's GLES backend targets ES 3.2 core and uses extensions
        // where present (EXT_buffer_storage, KHR_debug).
        minSdk = 26
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
                    "-DANDROID_PLATFORM=android-26",
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

// Keep native symbols in the packaged .so for debug builds only; release
// strips by default (arm64 ~157 MB → ~24 MB, still -O2 — stripping drops DWARF,
// not code). Variant API because keepDebugSymbols in android {} is global with
// no per-buildType form. Covers devDebug + gameDebug.
androidComponents {
    onVariants(selector().withBuildType("debug")) { variant ->
        variant.packaging.jniLibs.keepDebugSymbols.add("**/*.so")
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

val compileEngineShaders by tasks.registering {
    description = "Compile engine GLSL shaders (.vert/.frag/.comp) to SPIR-V via glslc"
    // Shader sources live with the renderer code (src/render/shaders/), not
    // in engine/ — they're engine source code, not user-overridable assets.
    // Vulkan sources are the canonical authoring; GLES variants are hand-
    // written siblings used only on the GLES backend.
    val vulkanShaderSrcDir = projectRoot.resolve("src/render/shaders/vulkan")
    val glesShaderSrcDir   = projectRoot.resolve("src/render/shaders/gles")
    val shaderOutDir = layout.buildDirectory.dir("dev_engine_shaders").get().asFile

    doFirst {
        if (!vulkanShaderSrcDir.isDirectory) {
            throw GradleException("Vulkan shader sources missing: $vulkanShaderSrcDir")
        }
        val glslc = resolveGlslc()
        shaderOutDir.deleteRecursively()
        shaderOutDir.mkdirs()

        val shaderFiles = vulkanShaderSrcDir.listFiles { f ->
            f.isFile && (f.name.endsWith(".vert") || f.name.endsWith(".frag") || f.name.endsWith(".comp"))
        } ?: emptyArray()
        if (shaderFiles.isEmpty()) {
            throw GradleException("No shader sources (*.vert, *.frag, *.comp) in $vulkanShaderSrcDir")
        }

        // Pass 1: GLSL → SPIR-V via glslc. The .spv files are kept around
        // so Vulkan-target builds can still consume them; on Android the
        // .glsl variants produced below are what the GLES backend reads.
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

        // Pass 2: stage the GLES variants. Each Vulkan shader that needs to
        // run on GLES has a hand-written sibling at src/render/shaders/gles/
        // (authored as GLSL ES 3.10 against the engine's flat binding scheme:
        // `set * 16 + binding`, push-constant UBO at binding 30). We do a
        // dumb copy with a one-byte stage tag prepended so the GLES Rhi's
        // create_shader_module knows VS vs. FS without a separate manifest.
        //
        // SPIR-V → GLSL ES auto-translation via spirv-cross was tried and
        // rejected: glslc strips unused push-constant / UBO members per
        // stage, producing struct mismatches that GL refuses to link; bindless
        // texture arrays and Vulkan-only tokens don't round-trip cleanly; and
        // every regex band-aid grew another head. Hand-authored sources are
        // the cheaper, more legible option.
        //
        // A Vulkan shader without a GLES sibling is fine — the corresponding
        // pipeline simply stays inactive on GLES (Renderer::init logs a warn
        // and continues; world rendering paths are gated on pipeline validity).
        val skipped = mutableListOf<String>()
        shaderFiles.forEach { shader ->
            val tag = when {
                shader.name.endsWith(".vert") -> "V"
                shader.name.endsWith(".frag") -> "F"
                else -> return@forEach  // skip .comp until GLES compute is wired
            }
            val glesIn  = glesShaderSrcDir.resolve(shader.name)
            val glslOut = shaderOutDir.resolve("${shader.name}.glsl")
            if (!glesIn.isFile) {
                skipped.add(shader.name)
                return@forEach
            }
            glslOut.writeText("$tag\n${glesIn.readText()}")
        }
        if (skipped.isNotEmpty()) {
            logger.warn("No GLES sibling for ${skipped.size} shader(s) — pipelines will be inactive on Android:")
            skipped.forEach { logger.warn("  - $it") }
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

        devAssetsDir.mkdirs()
        devMapsDir.deleteRecursively()
        devMapsDir.mkdirs()

        // Re-stage engine assets:
        //   1. engine/ source verbatim (scripts, types, textures, lua, etc.).
        //   2. shaders/ overlaid with our own compiled .spv set (the .glsl
        //      sources copied in step 1 are replaced with .spv so the
        //      runtime loader finds compiled binaries).
        // Fonts are not packed — the engine discovers system fonts at
        // runtime via Font::init_from_system (Android has Roboto + Noto
        // CJK + Noto Color Emoji at /system/fonts/).
        gradleEngineStaging.deleteRecursively()
        gradleEngineStaging.mkdirs()
        engineSrcDir.copyRecursively(gradleEngineStaging, overwrite = true)
        val stagingShaders = gradleEngineStaging.resolve("shaders")
        stagingShaders.deleteRecursively()
        stagingShaders.mkdirs()
        gradleCompiledShaders.copyRecursively(stagingShaders, overwrite = true)

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
