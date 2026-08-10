import org.gradle.api.tasks.Exec

plugins {
    kotlin("multiplatform")
}

val vmRoot = rootProject.projectDir.resolve("../..").canonicalFile
val nativeCoreBuildDir = rootProject.layout.buildDirectory
    .dir("native-core").get().asFile
val jniObject = layout.buildDirectory.file("jni/lampvm_jni.o").get().asFile
val javaHome = file(System.getProperty("java.home"))

val configureNativeCore by tasks.registering(Exec::class) {
    inputs.file(rootProject.file("native-core/CMakeLists.txt"))
    inputs.file(vmRoot.resolve("include/lampvm/debug_api.h"))
    outputs.file(nativeCoreBuildDir.resolve("CMakeCache.txt"))
    commandLine(
        "cmake",
        "-S", rootProject.file("native-core").absolutePath,
        "-B", nativeCoreBuildDir.absolutePath,
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DLAMPVM_ROOT=${vmRoot.absolutePath}",
    )
}

val buildNativeCore by tasks.registering(Exec::class) {
    dependsOn(configureNativeCore)
    inputs.dir(vmRoot.resolve("src"))
    outputs.file(nativeCoreBuildDir.resolve("liblampvm_core.dylib"))
    commandLine("cmake", "--build", nativeCoreBuildDir.absolutePath, "--parallel")
}

val compileJniShim by tasks.registering(Exec::class) {
    inputs.file(project.file("src/nativeMain/c/lampvm_jni.c"))
    outputs.file(jniObject)
    doFirst { jniObject.parentFile.mkdirs() }
    commandLine(
        "cc",
        "-std=c11",
        "-fPIC",
        "-I${javaHome.resolve("include").absolutePath}",
        "-I${javaHome.resolve("include/darwin").absolutePath}",
        "-c", project.file("src/nativeMain/c/lampvm_jni.c").absolutePath,
        "-o", jniObject.absolutePath,
    )
}

kotlin {
    macosArm64("native") {
        compilations.getByName("main") {
            cinterops.create("lampvm") {
                definitionFile.set(project.file("src/nativeInterop/cinterop/lampvm.def"))
                includeDirs(vmRoot.resolve("include"))
                compilerOpts("-I${vmRoot.resolve("include").absolutePath}")
            }
        }
        binaries.sharedLib {
            baseName = "lampvm_debug_bridge"
            linkerOpts(
                jniObject.absolutePath,
                "-L${nativeCoreBuildDir.absolutePath}",
                "-llampvm_core",
                "-Wl,-rpath,@loader_path",
            )
        }
    }
}

tasks.matching {
    it.name.startsWith("link") && it.name.endsWith("SharedNative")
}.configureEach {
    dependsOn(buildNativeCore, compileJniShim)
}
