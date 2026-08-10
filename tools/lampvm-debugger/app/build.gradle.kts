import org.gradle.api.tasks.JavaExec
import org.gradle.api.tasks.Sync
import org.jetbrains.compose.desktop.application.dsl.TargetFormat

plugins {
    kotlin("jvm")
    id("org.jetbrains.compose")
    id("org.jetbrains.kotlin.plugin.compose")
}

val vmRoot = rootProject.projectDir.resolve("../..").canonicalFile
val nativeCoreBuildDir = rootProject.layout.buildDirectory
    .dir("native-core").get().asFile
val bridgeLibrary = project(":bridge").layout.buildDirectory.file(
    "bin/native/debugShared/liblampvm_debug_bridge.dylib",
)
val bundledNativeResources = layout.buildDirectory.dir("generated/native-resources")
val bundleNativeRuntime by tasks.registering(Sync::class) {
    dependsOn(":bridge:linkDebugSharedNative")
    from(nativeCoreBuildDir.resolve("liblampvm_core.dylib"))
    from(bridgeLibrary)
    into(bundledNativeResources.map { it.dir("native/macos-arm64") })
}

dependencies {
    implementation(compose.desktop.currentOs)
    implementation(compose.material3)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-swing:1.10.2")
    implementation("org.jetbrains.jediterm:jediterm-core:3.74")
    implementation("org.jetbrains.jediterm:jediterm-ui:3.74")
}

kotlin {
    jvmToolchain(22)
}

sourceSets.main {
    resources.srcDir(bundledNativeResources)
}

tasks.named("processResources") {
    dependsOn(bundleNativeRuntime)
}

compose.desktop {
    application {
        mainClass = "dev.lampvm.debugger.MainKt"
        nativeDistributions {
            targetFormats(TargetFormat.Dmg)
            packageName = "lampvm-debugger"
            packageVersion = "1.0.0"
        }
    }
}

tasks.withType<JavaExec>().configureEach {
    dependsOn(":bridge:linkDebugSharedNative")
    systemProperty(
        "lampvm.core.library",
        nativeCoreBuildDir.resolve("liblampvm_core.dylib").absolutePath,
    )
    systemProperty("lampvm.bridge.library", bridgeLibrary.get().asFile.absolutePath)
    workingDir(vmRoot)
}

tasks.register<JavaExec>("smoke") {
    group = "verification"
    description = "Runs the JVM -> Kotlin/Native -> Lamp VM debugger smoke test"
    classpath = sourceSets.main.get().runtimeClasspath
    mainClass.set("dev.lampvm.debugger.SmokeMainKt")
}

tasks.register<JavaExec>("smokeBundledNative") {
    group = "verification"
    description = "Verifies the packaged native libraries without development paths"
    dependsOn("processResources")
    classpath = sourceSets.main.get().runtimeClasspath
    mainClass.set("dev.lampvm.debugger.SmokeMainKt")
    doFirst {
        systemProperties.remove("lampvm.core.library")
        systemProperties.remove("lampvm.bridge.library")
    }
}
