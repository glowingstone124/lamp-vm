@file:OptIn(
    kotlinx.cinterop.ExperimentalForeignApi::class,
    kotlin.experimental.ExperimentalNativeApi::class,
)

package dev.lampvm.bridge

import cnames.structs.lamp_debug_vm
import kotlinx.cinterop.ByteVar
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.ULongVar
import kotlinx.cinterop.UIntVar
import kotlinx.cinterop.alloc
import kotlinx.cinterop.convert
import kotlinx.cinterop.get
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.sizeOf
import kotlinx.cinterop.set
import kotlinx.cinterop.toKString
import kotlinx.cinterop.value
import lampvm.capi.LAMP_DEBUG_ABI_VERSION
import lampvm.capi.LAMP_DEBUG_OK
import lampvm.capi.lamp_debug_config_v1
import lampvm.capi.lamp_debug_cpu_snapshot_v1
import lampvm.capi.lamp_debug_create_from_file
import lampvm.capi.lamp_debug_destroy
import lampvm.capi.lamp_debug_get_cpu
import lampvm.capi.lamp_debug_get_stats
import lampvm.capi.lamp_debug_last_error
import lampvm.capi.lamp_debug_pause
import lampvm.capi.lamp_debug_read_memory
import lampvm.capi.lamp_debug_read_framebuffer
import lampvm.capi.lamp_debug_request_stop
import lampvm.capi.lamp_debug_resume
import lampvm.capi.lamp_debug_serial_read
import lampvm.capi.lamp_debug_serial_write
import lampvm.capi.lamp_debug_send_key
import lampvm.capi.lamp_debug_send_mouse
import lampvm.capi.lamp_debug_start
import lampvm.capi.lamp_debug_state
import lampvm.capi.lamp_debug_step
import lampvm.capi.lamp_debug_stats_v1

/* JVM sees monotonically increasing IDs, never process pointers.  The native
 * bridge remains the owner of opaque C handles and can evolve its bookkeeping
 * without changing the JNI contract.  Compose currently serializes calls on
 * its UI dispatcher; a later event-pump milestone will add a native mutex. */
private val nativeHandles = mutableMapOf<Long, CPointer<lamp_debug_vm>>()
private var nextHandle = 1L

private fun nativeHandle(value: Long): CPointer<lamp_debug_vm>? =
    nativeHandles[value]

@CName("lamp_kn_create")
fun create(
    programPath: CPointer<ByteVar>?,
    diskPath: CPointer<ByteVar>?,
    memoryBytes: Long,
    cpuFrequencyHz: Long,
    coreCount: Int,
    executionEngine: Int,
): Long = memScoped {
    if (programPath == null) return@memScoped 0L

    val config = alloc<lamp_debug_config_v1>()
    config.struct_size = sizeOf<lamp_debug_config_v1>().convert()
    config.abi_version = LAMP_DEBUG_ABI_VERSION
    config.memory_bytes = memoryBytes.toULong()
    config.cpu_frequency_hz = cpuFrequencyHz.toULong()
    config.core_count = coreCount.toUInt()
    config.execution_engine = executionEngine.toUInt()

    val output = alloc<CPointerVar<lamp_debug_vm>>()
    val status = lamp_debug_create_from_file(
        config.ptr,
        programPath.toKString(),
        diskPath?.toKString()?.takeIf { it.isNotEmpty() },
        output.ptr,
    )
    val native = output.value
    if (status != LAMP_DEBUG_OK || native == null) {
        0L
    } else {
        nextHandle++.also { nativeHandles[it] = native }
    }
}

@CName("lamp_kn_start")
fun start(handle: Long): Int =
    lamp_debug_start(nativeHandle(handle)).toInt()

@CName("lamp_kn_pause")
fun pause(handle: Long, timeoutMillis: Long): Int =
    lamp_debug_pause(nativeHandle(handle), timeoutMillis.toULong()).toInt()

@CName("lamp_kn_step")
fun step(handle: Long, coreId: Int, timeoutMillis: Long): Int =
    lamp_debug_step(
        nativeHandle(handle),
        coreId.toUInt(),
        timeoutMillis.toULong(),
    ).toInt()

@CName("lamp_kn_resume")
fun resume(handle: Long): Int =
    lamp_debug_resume(nativeHandle(handle)).toInt()

@CName("lamp_kn_stop")
fun stop(handle: Long): Int =
    lamp_debug_request_stop(nativeHandle(handle)).toInt()

@CName("lamp_kn_destroy")
fun destroy(handle: Long) {
    lamp_debug_destroy(nativeHandles.remove(handle))
}

@CName("lamp_kn_state")
fun state(handle: Long): Int =
    lamp_debug_state(nativeHandle(handle)).toInt()

@CName("lamp_kn_stats")
fun stats(handle: Long, output: CPointer<ULongVar>?, capacity: Int): Int =
    memScoped {
        if (output == null || capacity < 10) return@memScoped 1
        val value = alloc<lamp_debug_stats_v1>()
        value.struct_size = sizeOf<lamp_debug_stats_v1>().convert()
        val status = lamp_debug_get_stats(nativeHandle(handle), value.ptr)
        if (status != LAMP_DEBUG_OK) return@memScoped status.toInt()

        output[0] = value.state.toULong()
        output[1] = value.cpu_frequency_hz
        output[2] = value.virtual_cycles
        output[3] = value.executed_instructions
        output[4] = value.execution_rate_hz
        output[5] = value.uptime_ns
        output[6] = value.host_resident_bytes
        output[7] = value.guest_ram_bytes
        output[8] = value.core_count.toULong()
        output[9] = value.active_core_count.toULong()
        0
    }

@CName("lamp_kn_cpu")
fun cpu(
    handle: Long,
    coreId: Int,
    output: CPointer<ULongVar>?,
    capacity: Int,
): Int = memScoped {
    if (output == null || capacity < 43) return@memScoped 1
    val value = alloc<lamp_debug_cpu_snapshot_v1>()
    value.struct_size = sizeOf<lamp_debug_cpu_snapshot_v1>().convert()
    val status = lamp_debug_get_cpu(nativeHandle(handle), coreId.toUInt(), value.ptr)
    if (status != LAMP_DEBUG_OK) return@memScoped status.toInt()

    output[0] = value.core_id.toULong()
    repeat(32) { output[it + 1] = value.registers[it].toULong() }
    output[33] = value.ip
    output[34] = value.last_ip
    output[35] = value.flags.toULong()
    output[36] = value.call_stack_pointer.toLong().toULong()
    output[37] = value.data_stack_pointer.toLong().toULong()
    output[38] = value.interrupt_stack_pointer.toLong().toULong()
    output[39] = value.active_interrupt.toULong()
    output[40] = value.in_interrupt.toULong()
    output[41] = value.irq_masked.toULong()
    output[42] = value.is_bootstrap_processor.toULong()
    0
}

@CName("lamp_kn_read_memory")
fun readMemory(
    handle: Long,
    address: Int,
    destination: CPointer<UByteVar>?,
    size: Int,
): Int = lamp_debug_read_memory(
    nativeHandle(handle),
    address.toUInt(),
    destination,
    size.convert(),
).toInt()

@CName("lamp_kn_read_framebuffer")
fun readFramebuffer(
    handle: Long,
    destination: CPointer<UIntVar>?,
    pixelCapacity: Int,
): Int = lamp_debug_read_framebuffer(
    nativeHandle(handle),
    destination,
    pixelCapacity.convert(),
).toInt()

@CName("lamp_kn_send_key")
fun sendKey(
    handle: Long,
    scanCode: Int,
    extended: Int,
    pressed: Int,
): Int = lamp_debug_send_key(
    nativeHandle(handle),
    scanCode.toUByte(),
    extended.toUByte(),
    pressed.toUByte(),
).toInt()

@CName("lamp_kn_send_mouse")
fun sendMouse(
    handle: Long,
    deltaX: Int,
    deltaY: Int,
    buttons: Int,
): Int = lamp_debug_send_mouse(
    nativeHandle(handle),
    deltaX,
    deltaY,
    buttons.toUByte(),
).toInt()

@CName("lamp_kn_serial_read")
fun serialRead(
    handle: Long,
    destination: CPointer<UByteVar>?,
    capacity: Int,
): Int = lamp_debug_serial_read(
    nativeHandle(handle),
    destination,
    capacity.convert(),
).toInt()

@CName("lamp_kn_serial_write")
fun serialWrite(
    handle: Long,
    source: CPointer<UByteVar>?,
    size: Int,
): Int = lamp_debug_serial_write(
    nativeHandle(handle),
    source,
    size.convert(),
).toInt()

@CName("lamp_kn_last_error")
fun lastError(
    handle: Long,
    destination: CPointer<UByteVar>?,
    capacity: Int,
): Int {
    if (destination == null || capacity <= 0) return 0
    val message = lamp_debug_last_error(nativeHandle(handle))
        ?.toKString()
        .orEmpty()
    val encoded = message.encodeToByteArray()
    val count = minOf(encoded.size, capacity - 1)
    repeat(count) { destination[it] = encoded[it].toUByte() }
    destination[count] = 0u
    return count
}
