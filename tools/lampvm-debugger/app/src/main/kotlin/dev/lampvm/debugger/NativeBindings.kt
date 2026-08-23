package dev.lampvm.debugger

import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption

internal object NativeBindings {
    private val extractedNativeDirectory by lazy {
        Files.createTempDirectory("lampvm-native-").toFile().apply { deleteOnExit() }
    }

    init {
        val core = requiredLibrary("lampvm.core.library", "liblampvm_core.dylib")
        val bridge = requiredLibrary(
            "lampvm.bridge.library",
            "liblampvm_debug_bridge.dylib",
        )
        System.load(core.absolutePath)
        System.load(bridge.absolutePath)
    }

    private fun requiredLibrary(property: String, fileName: String): File {
        System.getProperty(property)?.let { path ->
            return File(path).also {
                check(it.isFile) { "Native library does not exist: ${it.absolutePath}" }
            }
        }

        val resource = "/native/macos-arm64/$fileName"
        val destination = File(extractedNativeDirectory, fileName)
        NativeBindings::class.java.getResourceAsStream(resource).use { input ->
            checkNotNull(input) {
                "Missing bundled native library $resource; use the macOS arm64 distribution"
            }
            Files.copy(input, destination.toPath(), StandardCopyOption.REPLACE_EXISTING)
        }
        destination.deleteOnExit()
        return destination
    }

    external fun create(
        programPath: String,
        diskPath: String,
        memoryBytes: Long,
        cpuFrequencyHz: Long,
        coreCount: Int,
        executionEngine: Int,
    ): Long

    external fun start(handle: Long)
    external fun pause(handle: Long, timeoutMillis: Long)
    external fun step(handle: Long, coreId: Int, timeoutMillis: Long)
    external fun resume(handle: Long)
    external fun stop(handle: Long)
    external fun destroy(handle: Long)
    external fun state(handle: Long): Int
    external fun stats(handle: Long): LongArray
    external fun cpu(handle: Long, coreId: Int): LongArray
    external fun readMemory(handle: Long, address: Int, size: Int): ByteArray
    external fun readMmio(handle: Long, address: Int, size: Int): ByteArray
    external fun readFramebuffer(handle: Long): IntArray
    external fun sendKey(handle: Long, scanCode: Int, extended: Boolean, pressed: Boolean)
    external fun sendMouse(handle: Long, deltaX: Int, deltaY: Int, buttons: Int)
    external fun serialRead(handle: Long, capacity: Int): ByteArray
    external fun serialWrite(handle: Long, data: ByteArray): Int
}
