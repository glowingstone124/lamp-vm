package dev.lampvm.debugger

fun main() {
    LampVmSession.create(
        programPath = "bios/boot.bin",
        diskPath = "disk.img",
        memoryBytes = 96L * 1024L * 1024L,
        cpuFrequencyHz = 125_000_000L,
        coreCount = 4,
        executionEngine = ExecutionEngine.Threaded,
    ).use { vm ->
        check(vm.state == VmState.Created)
        val initialStats = vm.stats()
        check(initialStats.guestRamBytes == 96uL * 1024uL * 1024uL)
        check(initialStats.cpuFrequencyHz == 125_000_000L)
        check(initialStats.coreCount == 4)
        check(vm.readFramebuffer().size == VGA_WIDTH * VGA_HEIGHT) {
            "framebuffer snapshot ABI returned the wrong dimensions"
        }
        val initialCpu = vm.cpu(0)
        check(initialCpu.registers.size == 32)
        val initialCode = vm.readMemory(initialCpu.ip.toUInt(), 64)
        check(initialCode.take(8).any { it != 0.toByte() }) {
            "memory at the initial instruction pointer is empty"
        }
        val instructions = LampDisassembler.decode(initialCpu.ip.toUInt(), initialCode)
        check(instructions.isNotEmpty() && instructions.first().mnemonic != ".quad") {
            "initial guest code could not be disassembled"
        }
        val reportedCpuFrequency = vm.readMmio(0x0074c05cu, 4).toLittleEndianUInt()
        check(reportedCpuFrequency == 125_000_000u) {
            "SYSINFO MMIO returned an unexpected CPU frequency: $reportedCpuFrequency"
        }

        vm.start()
        val serial = ArrayList<Byte>()
        var attempts = 0
        while ((serial.size < 64 || !serial.toByteArray().containsSequence("\r\n".encodeToByteArray())) &&
            attempts++ < 150
        ) {
            Thread.sleep(20)
            serial += vm.readSerial().toList()
        }
        check(serial.isNotEmpty()) { "guest serial TX did not reach the debugger FIFO" }
        check(serial.toByteArray().decodeToString().contains("LAMP")) {
            "debugger FIFO did not contain the expected guest boot log"
        }
        check(serial.toByteArray().containsSequence("\r\n".encodeToByteArray())) {
            "kernel serial output did not apply ONLCR"
        }
        check(vm.writeSerial("\n") == 1) {
            "guest serial RX did not accept direct terminal input"
        }
        vm.sendKey(scanCode = 0x1e, extended = false, pressed = true)
        vm.sendKey(scanCode = 0x1e, extended = false, pressed = false)
        vm.sendMouse(deltaX = 3, deltaY = -2, buttons = 1)
        vm.sendMouse(deltaX = 0, deltaY = 0, buttons = 0)

        // Keep the default smoke alive long enough for every AP to enable MMU,
        // enter the scheduler, and restore its scheduler context at least once.
        val soakMillis = System.getenv("LAMPVM_SMOKE_SOAK_MS")?.toLongOrNull() ?: 500L
        if (soakMillis > 0L) {
            val deadlineNanos = System.nanoTime() + soakMillis * 1_000_000L
            while (System.nanoTime() < deadlineNanos && vm.state == VmState.Running) {
                vm.readSerial()
                Thread.sleep(10)
            }
            check(vm.state == VmState.Running) {
                "VM stopped during ${soakMillis}ms threaded/SMP soak: state=${vm.state}"
            }
        }

        if (vm.state == VmState.Running) {
            vm.pause()
            check(vm.state == VmState.Paused)
            val cpu = vm.cpu(0)
            check(cpu.coreId == 0)
            check(vm.cpu(1).coreId == 1)
            check(vm.readMemory(cpu.ip.toUInt(), 8).size == 8)
            val pausedInput = "queued while paused\n".encodeToByteArray()
            check(vm.writeSerial(pausedInput) == pausedInput.size)
            check(vm.pendingSerialInputBytes == pausedInput.size) {
                "terminal input was sent before the paused VM stepped"
            }
            val instructionsBeforeStep = vm.stats().executedInstructions
            vm.step(0)
            check(vm.state == VmState.Paused)
            check(vm.pendingSerialInputBytes == 0) {
                "single-step did not schedule queued terminal input"
            }
            check(vm.stats().executedInstructions == instructionsBeforeStep + 1uL) {
                "single-step did not execute exactly one guest instruction"
            }
            vm.resume()
        }

        val stats = vm.stats()
        check(stats.coreCount == 4)
        vm.stop()
        println(
            "LampVM debugger smoke test passed: " +
                "state=${vm.state}, instructions=${stats.executedInstructions}",
        )
    }
}

private fun ByteArray.containsSequence(sequence: ByteArray): Boolean {
    if (sequence.isEmpty()) return true
    if (sequence.size > size) return false
    for (start in 0..size - sequence.size) {
        var matches = true
        for (offset in sequence.indices) {
            if (this[start + offset] != sequence[offset]) {
                matches = false
                break
            }
        }
        if (matches) return true
    }
    return false
}

private fun ByteArray.toLittleEndianUInt(): UInt {
    check(size == 4) { "expected a 32-bit MMIO register" }
    return (this[0].toUInt() and 0xffu) or
        ((this[1].toUInt() and 0xffu) shl 8) or
        ((this[2].toUInt() and 0xffu) shl 16) or
        ((this[3].toUInt() and 0xffu) shl 24)
}
