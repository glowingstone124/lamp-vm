package dev.lampvm.debugger

import java.util.ArrayDeque

enum class VmState {
    Created,
    Running,
    Paused,
    Stopped,
    Panicked;

    companion object {
        fun fromNative(value: Int): VmState = entries.getOrElse(value) { Panicked }
    }
}

enum class ExecutionEngine(val nativeValue: Int, val displayName: String) {
    Classic(0, "Classic"),
    Cached(1, "Cached"),
    Threaded(2, "Threaded"),
    Jit(3, "JIT"),
}

data class VmStats(
    val state: VmState,
    val cpuFrequencyHz: Long,
    val virtualCycles: ULong,
    val executedInstructions: ULong,
    val executionRateHz: ULong,
    val uptimeNs: ULong,
    val hostResidentBytes: ULong,
    val guestRamBytes: ULong,
    val coreCount: Int,
    val activeCoreCount: Int,
)

data class CpuSnapshot(
    val coreId: Int,
    val registers: List<UInt>,
    val ip: ULong,
    val lastIp: ULong,
    val flags: UInt,
    val callStackPointer: Int,
    val dataStackPointer: Int,
    val interruptStackPointer: Int,
    val activeInterrupt: UInt,
    val inInterrupt: Boolean,
    val irqMasked: Boolean,
    val bootstrapProcessor: Boolean,
)

private sealed interface DeferredPs2Input {
    data class Key(val scanCode: Int, val extended: Boolean, val pressed: Boolean) :
        DeferredPs2Input

    data class Mouse(val deltaX: Int, val deltaY: Int, val buttons: Int) :
        DeferredPs2Input
}

/* Must stay below LAMP_DEBUG_MOUSE_MAX_DELTA. Keeping a little headroom also
 * leaves space for the controller's response FIFO reservation. */
private const val MAX_PS2_MOUSE_DELTA_PER_REPORT = 16_384

class LampVmSession private constructor(private var handle: Long) : AutoCloseable {
    private val serialInputLock = Any()
    private val deferredSerialInput = DeferredSerialInput()
    private val deferredPs2Input = ArrayDeque<DeferredPs2Input>()

    val state: VmState
        get() = VmState.fromNative(NativeBindings.state(requireHandle()))

    internal val pendingSerialInputBytes: Int
        get() = synchronized(serialInputLock) { deferredSerialInput.size }

    fun start() = synchronized(serialInputLock) {
        val current = requireHandle()
        NativeBindings.start(current)
        flushDeferredSerialInput(current)
        flushDeferredPs2Input(current)
    }

    fun pause(timeoutMillis: Long = 2_000) = synchronized(serialInputLock) {
        NativeBindings.pause(requireHandle(), timeoutMillis)
    }

    fun step(coreId: Int = 0, timeoutMillis: Long = 2_000) =
        synchronized(serialInputLock) {
            val current = requireHandle()
            /* Queue serial input before publishing the step request so the
             * paused vCPU can observe its interrupt during this step. */
            flushDeferredSerialInput(current)
            flushDeferredPs2Input(current)
            NativeBindings.step(current, coreId, timeoutMillis)
        }

    fun resume() = synchronized(serialInputLock) {
        val current = requireHandle()
        flushDeferredSerialInput(current)
        flushDeferredPs2Input(current)
        NativeBindings.resume(current)
    }

    fun stop() = synchronized(serialInputLock) {
        NativeBindings.stop(requireHandle())
    }

    fun stats(): VmStats {
        val value = NativeBindings.stats(requireHandle())
        return VmStats(
            state = VmState.fromNative(value[0].toInt()),
            cpuFrequencyHz = value[1],
            virtualCycles = value[2].toULong(),
            executedInstructions = value[3].toULong(),
            executionRateHz = value[4].toULong(),
            uptimeNs = value[5].toULong(),
            hostResidentBytes = value[6].toULong(),
            guestRamBytes = value[7].toULong(),
            coreCount = value[8].toInt(),
            activeCoreCount = value[9].toInt(),
        )
    }

    fun cpu(coreId: Int): CpuSnapshot {
        val value = NativeBindings.cpu(requireHandle(), coreId)
        return CpuSnapshot(
            coreId = value[0].toInt(),
            registers = (1..32).map { value[it].toUInt() },
            ip = value[33].toULong(),
            lastIp = value[34].toULong(),
            flags = value[35].toUInt(),
            callStackPointer = value[36].toInt(),
            dataStackPointer = value[37].toInt(),
            interruptStackPointer = value[38].toInt(),
            activeInterrupt = value[39].toUInt(),
            inInterrupt = value[40] != 0L,
            irqMasked = value[41] != 0L,
            bootstrapProcessor = value[42] != 0L,
        )
    }

    fun readMemory(address: UInt, size: Int): ByteArray =
        NativeBindings.readMemory(requireHandle(), address.toInt(), size)

    fun readMmio(address: UInt, size: Int): ByteArray =
        NativeBindings.readMmio(requireHandle(), address.toInt(), size)

    fun readFramebuffer(): IntArray = synchronized(serialInputLock) {
        val current = requireHandle()
        /* VGA polling is also the retry clock for PS/2 events queued while
         * the guest is still completing controller initialization. */
        flushDeferredPs2Input(current)
        NativeBindings.readFramebuffer(current)
    }

    fun sendKey(scanCode: Int, extended: Boolean, pressed: Boolean) =
        synchronized(serialInputLock) {
            enqueuePs2Input(
                DeferredPs2Input.Key(scanCode, extended, pressed),
            )
            flushDeferredPs2Input(requireHandle())
        }

    fun sendMouse(deltaX: Int, deltaY: Int, buttons: Int) =
        synchronized(serialInputLock) {
            enqueuePs2Input(DeferredPs2Input.Mouse(deltaX, deltaY, buttons))
            flushDeferredPs2Input(requireHandle())
        }

    internal fun flushPendingInput() = synchronized(serialInputLock) {
        if (handle != 0L) {
            flushDeferredPs2Input(handle)
        }
    }

    fun readSerial(capacity: Int = 8_192): ByteArray {
        val current = requireHandle()
        synchronized(serialInputLock) {
            if (VmState.fromNative(NativeBindings.state(current)) != VmState.Paused) {
                if (deferredSerialInput.size > 0) {
                    flushDeferredSerialInput(current)
                }
                flushDeferredPs2Input(current)
            }
        }
        return NativeBindings.serialRead(current, capacity)
    }

    fun writeSerial(text: String): Int =
        writeSerial(text.encodeToByteArray())

    fun writeSerial(data: ByteArray): Int = synchronized(serialInputLock) {
        if (data.isEmpty()) return@synchronized 0
        val current = requireHandle()
        deferredSerialInput.enqueue(data)
        if (VmState.fromNative(NativeBindings.state(current)) != VmState.Paused) {
            flushDeferredSerialInput(current)
        }
        /* JediTerm cares whether the debugger accepted the input. Bytes that
         * do not fit the guest RX FIFO remain queued for a later drain. */
        data.size
    }

    override fun close() {
        synchronized(serialInputLock) {
            val current = handle
            if (current != 0L) {
                handle = 0L
                deferredSerialInput.clear()
                deferredPs2Input.clear()
                NativeBindings.destroy(current)
            }
        }
    }

    private fun flushDeferredSerialInput(current: Long): Int =
        deferredSerialInput.flush { NativeBindings.serialWrite(current, it) }

    private fun enqueuePs2Input(input: DeferredPs2Input) {
        check(deferredPs2Input.size < PS2_INPUT_QUEUE_CAPACITY) {
            "guest PS/2 input queue is full"
        }
        deferredPs2Input.addLast(input)
    }

    private fun flushDeferredPs2Input(current: Long) {
        while (deferredPs2Input.isNotEmpty()) {
            val input = deferredPs2Input.peekFirst()
            try {
                when (input) {
                    is DeferredPs2Input.Key -> NativeBindings.sendKey(
                        current,
                        input.scanCode,
                        input.extended,
                        input.pressed,
                    )
                    is DeferredPs2Input.Mouse -> {
                        /* A single host event can exceed the 8-bit PS/2
                         * packet range. Send one bounded atomic report at a
                         * time and retain the remainder at the queue front
                         * if the event spans multiple reports. */
                        val chunkX = input.deltaX.coerceIn(
                            -MAX_PS2_MOUSE_DELTA_PER_REPORT,
                            MAX_PS2_MOUSE_DELTA_PER_REPORT,
                        )
                        val chunkY = input.deltaY.coerceIn(
                            -MAX_PS2_MOUSE_DELTA_PER_REPORT,
                            MAX_PS2_MOUSE_DELTA_PER_REPORT,
                        )
                        NativeBindings.sendMouse(
                            current,
                            chunkX,
                            chunkY,
                            input.buttons,
                        )
                        deferredPs2Input.removeFirst()
                        val remainingX = input.deltaX - chunkX
                        val remainingY = input.deltaY - chunkY
                        if (remainingX != 0 || remainingY != 0) {
                            deferredPs2Input.addFirst(
                                DeferredPs2Input.Mouse(
                                    remainingX,
                                    remainingY,
                                    input.buttons,
                                ),
                            )
                        }
                        continue
                    }
                }
            } catch (_: Exception) {
                /* The controller may still be booting or temporarily full.
                 * Keep ordering intact and retry on the next input/tick. */
                return
            }
            deferredPs2Input.removeFirst()
        }
    }

    private fun requireHandle(): Long = handle.also {
        check(it != 0L) { "Lamp VM session is closed" }
    }

    companion object {
        private const val PS2_INPUT_QUEUE_CAPACITY = 4_096

        fun create(
            programPath: String,
            diskPath: String = "",
            memoryBytes: Long = 64L * 1024L * 1024L,
            cpuFrequencyHz: Long = 100_000_000L,
            coreCount: Int = 1,
            executionEngine: ExecutionEngine = ExecutionEngine.Classic,
        ): LampVmSession {
            return LampVmSession(
                NativeBindings.create(
                    programPath,
                    diskPath,
                    memoryBytes,
                    cpuFrequencyHz,
                    coreCount,
                    executionEngine.nativeValue,
                ),
            )
        }
    }
}
