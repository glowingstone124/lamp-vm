package dev.lampvm.debugger

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

class LampVmSession private constructor(private var handle: Long) : AutoCloseable {
    private val serialInputLock = Any()
    private val deferredSerialInput = DeferredSerialInput()

    val state: VmState
        get() = VmState.fromNative(NativeBindings.state(requireHandle()))

    internal val pendingSerialInputBytes: Int
        get() = synchronized(serialInputLock) { deferredSerialInput.size }

    fun start() = synchronized(serialInputLock) {
        val current = requireHandle()
        NativeBindings.start(current)
        flushDeferredSerialInput(current)
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
            NativeBindings.step(current, coreId, timeoutMillis)
        }

    fun resume() = synchronized(serialInputLock) {
        val current = requireHandle()
        flushDeferredSerialInput(current)
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

    fun readFramebuffer(): IntArray =
        NativeBindings.readFramebuffer(requireHandle())

    fun sendKey(scanCode: Int, extended: Boolean, pressed: Boolean) =
        NativeBindings.sendKey(requireHandle(), scanCode, extended, pressed)

    fun sendMouse(deltaX: Int, deltaY: Int, buttons: Int) =
        NativeBindings.sendMouse(requireHandle(), deltaX, deltaY, buttons)

    fun readSerial(capacity: Int = 8_192): ByteArray {
        val current = requireHandle()
        synchronized(serialInputLock) {
            if (deferredSerialInput.size > 0 &&
                VmState.fromNative(NativeBindings.state(current)) != VmState.Paused
            ) {
                flushDeferredSerialInput(current)
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
                NativeBindings.destroy(current)
            }
        }
    }

    private fun flushDeferredSerialInput(current: Long): Int =
        deferredSerialInput.flush { NativeBindings.serialWrite(current, it) }

    private fun requireHandle(): Long = handle.also {
        check(it != 0L) { "Lamp VM session is closed" }
    }

    companion object {
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
