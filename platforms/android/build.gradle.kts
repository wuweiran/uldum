// Root build file — plugin versions only. Per-module config lives in app/build.gradle.kts.
//
// AGP 9.x has built-in Kotlin support; no separate `org.jetbrains.kotlin.android`
// plugin is needed. AGP bundles a Kotlin toolchain that matches its release.
plugins {
    id("com.android.application") version "9.1.1" apply false
}
