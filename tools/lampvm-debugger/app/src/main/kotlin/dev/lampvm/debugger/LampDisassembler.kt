package dev.lampvm.debugger

data class DisassembledInstruction(
    val address: UInt,
    val raw: ULong,
    val mnemonic: String,
    val operands: String,
) {
    val text: String
        get() = if (operands.isEmpty()) mnemonic else "$mnemonic $operands"
}

object LampDisassembler {
    private val mnemonics = arrayOfNulls<String>(0x100).apply {
        listOf(
            "ADD", "SUB", "MUL", "DIV", "HALT", "JMP", "JZ", "PUSH",
            "POP", "CALL", "RET", "LOAD", "LOAD32", "LOADX32", "STORE",
            "STORE32", "STOREX32", "CMP", "CMPI", "MOV", "MOVI", "MEMSET",
            "MEMCPY", "IN", "OUT", "INT", "IRET", "MOD", "AND", "OR",
            "XOR", "NOT", "SHL", "SHR", "SAR", "JNZ", "JG", "JGE", "JL",
            "JLE", "JC", "JNC", "FADD", "FSUB", "FMUL", "FDIV", "FNEG",
            "FABS", "FSQRT", "FCMP", "ITOF", "FTOI", "FLOAD32", "FSTORE32",
            "INC", "ADDI", "SUBI", "ANDI", "ORI", "XORI", "SHLI", "SHRI",
            "CAS", "XADD", "XCHG", "LDAR", "STLR", "FENCE", "PAUSE",
            "STARTAP", "IPI", "CPUID", "CALLR", "RJMP", "RCALL", "RJZ",
            "RJNZ", "ROL", "ROR", "ROLI", "RORI", "LOAD16", "STORE16",
            "LOADS8", "LOADS16", "RJG", "RJGE", "RJL", "RJLE", "RJC",
            "RJNC", "INTI", "LOADX", "LOADX16", "STOREX", "STOREX16",
        ).forEachIndexed { index, mnemonic -> this[index + 1] = mnemonic }
    }

    fun decode(startAddress: UInt, bytes: ByteArray): List<DisassembledInstruction> =
        bytes.asList().chunked(8)
            .takeWhile { it.size == 8 }
            .mapIndexed { index, instructionBytes ->
                val raw = instructionBytes.foldIndexed(0uL) { byteIndex, value, byte ->
                    value or (byte.toUByte().toULong() shl (byteIndex * 8))
                }
                val address = startAddress + (index * 8).toUInt()
                decodeOne(address, raw)
            }

    private fun decodeOne(address: UInt, raw: ULong): DisassembledInstruction {
        val opcode = ((raw shr 56) and 0xffu).toInt()
        val rd = ((raw shr 48) and 0xffu).toInt()
        val rs1 = ((raw shr 40) and 0xffu).toInt()
        val rs2 = ((raw shr 32) and 0xffu).toInt()
        val imm = raw.toUInt()
        val mnemonic = mnemonics[opcode]
            ?: return DisassembledInstruction(
                address, raw, ".quad", "0x${raw.toString(16).padStart(16, '0')}",
            )
        return DisassembledInstruction(
            address = address,
            raw = raw,
            mnemonic = mnemonic.lowercase(),
            operands = formatOperands(opcode, address, rd, rs1, rs2, imm),
        )
    }

    private fun formatOperands(
        opcode: Int,
        address: UInt,
        rd: Int,
        rs1: Int,
        rs2: Int,
        imm: UInt,
    ): String = when (opcode) {
        0x05, 0x0B, 0x1B, 0x44, 0x45 -> ""
        0x06, 0x07, 0x0A, in 0x24..0x2A -> imm.hexAddress()
        in 0x4A..0x4D, in 0x56..0x5B ->
            "${relativeTarget(address, imm).hexAddress()} ; ${imm.toInt().signed()}"
        0x08, 0x09, 0x1A, 0x37, 0x48, 0x49 -> "r$rd"
        0x20, in 0x2F..0x31, in 0x33..0x34 -> "r$rd, r$rs1"
        0x12, 0x14, 0x32 -> "r$rd, r$rs1"
        0x13, 0x15 -> "r$rd, ${imm.toInt().signed()}"
        0x16, 0x17 -> "r$rd, r$rs1, ${imm.toInt().signed()}"
        in 0x38..0x3E, in 0x50..0x51 ->
            "r$rd, r$rs1, ${imm.toInt().signed()}"
        0x0C, 0x0D, 0x35, in 0x52..0x55 ->
            "r$rd, [r$rs1${imm.toInt().offset()}]"
        0x0F, 0x10, 0x36, 0x53 ->
            "[r$rs1${imm.toInt().offset()}], r$rd"
        0x0E, 0x5D, 0x5E -> "r$rd, [r$rs1 + r$rs2${imm.toInt().offset()}]"
        0x11, 0x5F, 0x60 -> "[r$rs1 + r$rs2${imm.toInt().offset()}], r$rd"
        0x18 -> "r$rd, [r$rs1]"
        0x19 -> "[r$rs1], r$rd"
        0x5C -> imm.hexAddress()
        0x46 -> "r$rd, r$rs1, ${imm.toInt().signed()}"
        0x47 -> "r$rd, r$rs1"
        in 0x3F..0x41 -> "r$rd, [r$rs1${imm.toInt().offset()}], r$rs2"
        0x42 -> "r$rd, [r$rs1${imm.toInt().offset()}]"
        0x43 -> "[r$rs1${imm.toInt().offset()}], r$rd"
        else -> "r$rd, r$rs1, r$rs2"
    }

    private fun relativeTarget(address: UInt, immediate: UInt): UInt =
        (address.toLong() + immediate.toInt().toLong()).toUInt()

    private fun UInt.hexAddress(): String = "0x${toString(16).padStart(8, '0')}"
    private fun Int.signed(): String = toString()
    private fun Int.offset(): String = when {
        this > 0 -> " + $this"
        this < 0 -> " - ${-toLong()}"
        else -> ""
    }
}
