package dev.lampvm.debugger

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import kotlinx.coroutines.delay
import java.io.File
import kotlin.math.log10
import kotlin.math.pow

private val LampColors: ColorScheme = darkColorScheme(
    primary = Color(0xFF8BE9C1),
    onPrimary = Color(0xFF06251B),
    secondary = Color(0xFF8FB7FF),
    background = Color(0xFF0B1017),
    surface = Color(0xFF121923),
    surfaceVariant = Color(0xFF192330),
    onSurface = Color(0xFFE6EDF6),
    onSurfaceVariant = Color(0xFF9EADBF),
    error = Color(0xFFFF7B86),
)

fun main() = application {
    Window(
        onCloseRequest = ::exitApplication,
        title = "LampVM",
        state = rememberWindowState(size = DpSize(1_400.dp, 900.dp)),
    ) {
        MaterialTheme(colorScheme = LampColors) {
            DebuggerScreen()
        }
    }
}

@Composable
private fun DebuggerScreen() {
    var programPath by remember { mutableStateOf("bios/boot.bin") }
    var diskPath by remember { mutableStateOf("disk.img") }
    var memoryMiB by remember { mutableStateOf("64") }
    var coreCount by remember { mutableStateOf("1") }
    var cpuMHz by remember { mutableStateOf("100") }
    var executionEngine by remember { mutableStateOf(ExecutionEngine.Classic) }
    var vgaEnabled by remember { mutableStateOf(true) }
    var session by remember { mutableStateOf<LampVmSession?>(null) }
    var stats by remember { mutableStateOf<VmStats?>(null) }
    var cpu by remember { mutableStateOf<CpuSnapshot?>(null) }
    var selectedCore by remember { mutableStateOf(0) }
    var memoryAddress by remember { mutableStateOf("0x0000201c") }
    var memoryFollowsIp by remember { mutableStateOf(true) }
    var memory by remember { mutableStateOf(ByteArray(0)) }
    var disassembly by remember { mutableStateOf(emptyList<DisassembledInstruction>()) }
    var error by remember { mutableStateOf<String?>(null) }

    fun inspect() {
        val active = session ?: return
        runCatching {
            val latest = active.stats()
            stats = latest
            if (active.state != VmState.Running) {
                val coreId = selectedCore.coerceIn(0, (latest.coreCount - 1).coerceAtLeast(0))
                if (coreId != selectedCore) selectedCore = coreId
                val snapshot = active.cpu(coreId)
                cpu = snapshot
                val address = if (memoryFollowsIp) {
                    snapshot.ip.toUInt().also { memoryAddress = it.hex32() }
                } else {
                    parseAddress(memoryAddress)
                }
                memory = active.readMemory(address, 256)
                val disassemblyStart = if (snapshot.ip >= 40uL) {
                    snapshot.ip.toUInt() - 40u
                } else {
                    0u
                }
                val remaining = latest.guestRamBytes.toLong() - disassemblyStart.toLong()
                val disassemblySize = minOf(256L, remaining).coerceAtLeast(0L).toInt()
                disassembly = if (disassemblySize >= 8) {
                    LampDisassembler.decode(
                        disassemblyStart,
                        active.readMemory(disassemblyStart, disassemblySize),
                    )
                } else {
                    emptyList()
                }
            }
        }.onFailure { error = it.message }
    }

    LaunchedEffect(session) {
        while (session != null) {
            inspect()
            delay(100)
        }
    }

    DisposableEffect(Unit) {
        onDispose { session?.close() }
    }

    Surface(Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier.fillMaxSize().padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Header(stats?.state ?: VmState.Stopped)

            ConnectionBar(
                programPath = programPath,
                onProgramPathChange = { programPath = it },
                diskPath = diskPath,
                onDiskPathChange = { diskPath = it },
                memoryMiB = memoryMiB,
                onMemoryMiBChange = { memoryMiB = it },
                coreCount = coreCount,
                onCoreCountChange = { coreCount = it },
                cpuMHz = cpuMHz,
                onCpuMHzChange = { cpuMHz = it },
                executionEngine = executionEngine,
                onCycleExecutionEngine = {
                    executionEngine = ExecutionEngine.entries[
                        (executionEngine.ordinal + 1) % ExecutionEngine.entries.size
                    ]
                },
                vgaEnabled = vgaEnabled,
                onVgaEnabledChange = { vgaEnabled = it },
                connected = session != null,
                onConnect = {
                    runCatching {
                        require(File(programPath).isFile) {
                            "Guest image not found: $programPath"
                        }
                        val parsedMemoryMiB = memoryMiB.toLongOrNull()
                            ?.takeIf { it in 8..4096 }
                            ?: error("Memory must be between 8 and 4096 MiB")
                        val parsedCores = coreCount.toIntOrNull()
                            ?.takeIf { it in 1..64 }
                            ?: error("Core count must be between 1 and 64")
                        val parsedCpuMHz = cpuMHz.toLongOrNull()
                            ?.takeIf { it in 1..10_000 }
                            ?: error("CPU MHz must be between 1 and 10000")
                        session?.close()
                        session = LampVmSession.create(
                            programPath = programPath,
                            diskPath = diskPath,
                            memoryBytes = parsedMemoryMiB * 1024L * 1024L,
                            cpuFrequencyHz = parsedCpuMHz * 1_000_000L,
                            coreCount = parsedCores,
                            executionEngine = executionEngine,
                        )
                        selectedCore = 0
                        inspect()
                    }.onFailure { error = it.message }
                },
                onDisconnect = {
                    session?.close()
                    session = null
                    stats = null
                    cpu = null
                    memory = ByteArray(0)
                    disassembly = emptyList()
                    selectedCore = 0
                },
            )

            error?.let {
                ErrorBanner(it) { error = null }
            }

            ControlBar(
                state = stats?.state ?: session?.state,
                enabled = session != null,
                coreCount = stats?.coreCount ?: 1,
                activeCoreCount = stats?.activeCoreCount ?: 1,
                selectedCore = selectedCore,
                onSelectNextCore = {
                    selectedCore = (selectedCore + 1) % (stats?.coreCount ?: 1)
                    inspect()
                },
                onStart = {
                    runCatching { session?.start() }.onFailure { error = it.message }
                    inspect()
                },
                onPause = {
                    runCatching { session?.pause() }.onFailure { error = it.message }
                    inspect()
                },
                onStep = {
                    runCatching { session?.step(selectedCore) }
                        .onFailure { error = it.message }
                    inspect()
                },
                onResume = {
                    runCatching { session?.resume() }.onFailure { error = it.message }
                    inspect()
                },
                onStop = {
                    runCatching { session?.stop() }.onFailure { error = it.message }
                    inspect()
                },
            )

            StatsRow(stats)

            Row(
                modifier = Modifier.fillMaxSize(),
                horizontalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                RegisterPanel(
                    cpu = cpu,
                    coreId = selectedCore,
                    modifier = Modifier.weight(0.75f).fillMaxHeight(),
                )
                DisassemblyPanel(
                    instructions = disassembly,
                    instructionPointer = cpu?.ip,
                    coreId = selectedCore,
                    modifier = Modifier.weight(1.15f).fillMaxHeight(),
                )
                Column(
                    modifier = Modifier.weight(1.35f).fillMaxHeight(),
                    verticalArrangement = Arrangement.spacedBy(14.dp),
                ) {
                    MemoryPanel(
                        address = memoryAddress,
                        onAddressChange = {
                            memoryAddress = it
                            memoryFollowsIp = false
                        },
                        followsIp = memoryFollowsIp,
                        onFollowsIpChange = { memoryFollowsIp = it },
                        inspectable = stats?.state != VmState.Running,
                        bytes = memory,
                        onRefresh = ::inspect,
                        modifier = Modifier.weight(0.9f),
                    )
                    TerminalPanel(
                        session = session,
                        modifier = Modifier.weight(1.1f),
                    )
                }
            }
        }
    }
    session?.takeIf { vgaEnabled }?.let { active ->
        VmVgaWindow(
            session = active,
            onCloseRequest = { vgaEnabled = false },
        )
    }
}

@Composable
private fun Header(state: VmState) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column {
            Text("LAMPVM", color = MaterialTheme.colorScheme.primary,
                 fontSize = 12.sp, fontWeight = FontWeight.Bold)
            Text("Virtual Machine & Debugger", fontSize = 26.sp,
                 fontWeight = FontWeight.SemiBold)
        }
        Spacer(Modifier.weight(1f))
        StateBadge(state)
    }
}

@Composable
private fun StateBadge(state: VmState) {
    val color = when (state) {
        VmState.Running -> MaterialTheme.colorScheme.primary
        VmState.Paused -> Color(0xFFFFCC66)
        VmState.Panicked -> MaterialTheme.colorScheme.error
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }
    Box(
        Modifier.background(color.copy(alpha = 0.14f), RoundedCornerShape(20.dp))
            .padding(horizontal = 14.dp, vertical = 7.dp),
    ) {
        Text(state.name.uppercase(), color = color, fontSize = 12.sp,
             fontWeight = FontWeight.Bold)
    }
}

@Composable
private fun ConnectionBar(
    programPath: String,
    onProgramPathChange: (String) -> Unit,
    diskPath: String,
    onDiskPathChange: (String) -> Unit,
    memoryMiB: String,
    onMemoryMiBChange: (String) -> Unit,
    coreCount: String,
    onCoreCountChange: (String) -> Unit,
    cpuMHz: String,
    onCpuMHzChange: (String) -> Unit,
    executionEngine: ExecutionEngine,
    onCycleExecutionEngine: () -> Unit,
    vgaEnabled: Boolean,
    onVgaEnabledChange: (Boolean) -> Unit,
    connected: Boolean,
    onConnect: () -> Unit,
    onDisconnect: () -> Unit,
) {
    Card(colors = CardDefaults.cardColors(MaterialTheme.colorScheme.surface)) {
        Column(
            Modifier.fillMaxWidth().padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                OutlinedTextField(
                    value = programPath,
                    onValueChange = onProgramPathChange,
                    label = { Text("Guest image") },
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                    enabled = !connected,
                )
                OutlinedTextField(
                    value = diskPath,
                    onValueChange = onDiskPathChange,
                    label = { Text("Disk image (optional)") },
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                    enabled = !connected,
                )
                Button(onClick = if (connected) onDisconnect else onConnect) {
                    Text(if (connected) "Disconnect" else "Attach")
                }
            }
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                OutlinedTextField(
                    value = memoryMiB,
                    onValueChange = onMemoryMiBChange,
                    label = { Text("Memory MiB") },
                    modifier = Modifier.width(140.dp),
                    singleLine = true,
                    enabled = !connected,
                )
                OutlinedTextField(
                    value = coreCount,
                    onValueChange = onCoreCountChange,
                    label = { Text("vCPU cores") },
                    modifier = Modifier.width(120.dp),
                    singleLine = true,
                    enabled = !connected,
                )
                OutlinedTextField(
                    value = cpuMHz,
                    onValueChange = onCpuMHzChange,
                    label = { Text("CPU MHz") },
                    modifier = Modifier.width(130.dp),
                    singleLine = true,
                    enabled = !connected,
                )
                OutlinedButton(
                    onClick = onCycleExecutionEngine,
                    enabled = !connected,
                ) {
                    Text("Engine · ${executionEngine.displayName}")
                }
                OutlinedButton(
                    onClick = { onVgaEnabledChange(!vgaEnabled) },
                ) {
                    Text(if (vgaEnabled) "VGA · On" else "VGA · Off")
                }
                Text(
                    "Configuration is applied when attaching",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontSize = 11.sp,
                )
            }
        }
    }
}

@Composable
private fun ControlBar(
    state: VmState?,
    enabled: Boolean,
    coreCount: Int,
    activeCoreCount: Int,
    selectedCore: Int,
    onSelectNextCore: () -> Unit,
    onStart: () -> Unit,
    onPause: () -> Unit,
    onStep: () -> Unit,
    onResume: () -> Unit,
    onStop: () -> Unit,
) {
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(
            onClick = onSelectNextCore,
            enabled = enabled && coreCount > 1,
        ) {
            Text(
                "vCPU $selectedCore / ${coreCount - 1}" +
                    if (selectedCore >= activeCoreCount) " · offline" else "",
            )
        }
        Button(onClick = onStart, enabled = enabled && state == VmState.Created) {
            Text("Run")
        }
        OutlinedButton(onClick = onPause, enabled = state == VmState.Running) {
            Text("Pause")
        }
        OutlinedButton(
            onClick = onStep,
            enabled = state == VmState.Paused && selectedCore < activeCoreCount,
        ) {
            Text("Step")
        }
        OutlinedButton(onClick = onResume, enabled = state == VmState.Paused) {
            Text("Resume")
        }
        OutlinedButton(
            onClick = onStop,
            enabled = state == VmState.Running || state == VmState.Paused,
            colors = ButtonDefaults.outlinedButtonColors(
                contentColor = MaterialTheme.colorScheme.error,
            ),
        ) {
            Text("Stop")
        }
    }
}

@Composable
private fun StatsRow(stats: VmStats?) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        StatCard("IP/s", stats?.executionRateHz?.toLong()?.formatCompact() ?: "—", Modifier.weight(1f))
        StatCard("Instructions", stats?.executedInstructions?.toLong()?.formatCompact() ?: "—", Modifier.weight(1f))
        StatCard("vCPU", stats?.let { "${it.activeCoreCount} / ${it.coreCount}" } ?: "—", Modifier.weight(1f))
        StatCard("Guest RAM", stats?.guestRamBytes?.toLong()?.formatBytes() ?: "—", Modifier.weight(1f))
        StatCard("Host RSS", stats?.hostResidentBytes?.toLong()?.formatBytes() ?: "—", Modifier.weight(1f))
    }
}

@Composable
private fun StatCard(label: String, value: String, modifier: Modifier) {
    Card(modifier, colors = CardDefaults.cardColors(MaterialTheme.colorScheme.surfaceVariant)) {
        Column(Modifier.padding(12.dp)) {
            Text(label.uppercase(), color = MaterialTheme.colorScheme.onSurfaceVariant,
                 fontSize = 10.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(4.dp))
            Text(value, fontFamily = FontFamily.Monospace, fontSize = 17.sp)
        }
    }
}

@Composable
private fun RegisterPanel(
    cpu: CpuSnapshot?,
    coreId: Int,
    modifier: Modifier = Modifier,
) {
    Panel("CPU $coreId · REGISTERS", modifier) {
        SelectionContainer {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                Text(
                    "IP   ${cpu?.ip?.hex64() ?: "—"}\n" +
                        "LAST ${cpu?.lastIp?.hex64() ?: "—"}\n" +
                        "FLAGS ${cpu?.flags?.hex32() ?: "—"}",
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.secondary,
                )
                Spacer(Modifier.height(12.dp))
                cpu?.registers?.chunked(2)?.forEachIndexed { row, values ->
                    Row(Modifier.fillMaxWidth()) {
                        values.forEachIndexed { column, value ->
                            val index = row * 2 + column
                            Text(
                                "r${index.toString().padStart(2, '0')}  ${value.hex32()}",
                                modifier = Modifier.weight(1f).padding(vertical = 3.dp),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 13.sp,
                            )
                        }
                    }
                } ?: Text("Pause the VM to inspect CPU state",
                           color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun DisassemblyPanel(
    instructions: List<DisassembledInstruction>,
    instructionPointer: ULong?,
    coreId: Int,
    modifier: Modifier = Modifier,
) {
    Panel("CPU $coreId · DISASSEMBLY", modifier) {
        if (instructions.isEmpty()) {
            Text(
                "Pause the VM to disassemble memory around IP",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            return@Panel
        }
        SelectionContainer {
            Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
                instructions.forEach { instruction ->
                    val current = instructionPointer?.toUInt() == instruction.address
                    Row(
                        Modifier.fillMaxWidth()
                            .background(
                                if (current) MaterialTheme.colorScheme.primary.copy(alpha = 0.13f)
                                else Color.Transparent,
                                RoundedCornerShape(5.dp),
                            )
                            .padding(horizontal = 6.dp, vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            if (current) "▶" else " ",
                            modifier = Modifier.width(18.dp),
                            color = MaterialTheme.colorScheme.primary,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 12.sp,
                        )
                        Text(
                            instruction.address.hex32(),
                            modifier = Modifier.width(98.dp),
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 11.sp,
                        )
                        Text(
                            instruction.raw.toString(16).padStart(16, '0'),
                            modifier = Modifier.width(142.dp),
                            color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.72f),
                            fontFamily = FontFamily.Monospace,
                            fontSize = 10.sp,
                        )
                        Text(
                            instruction.text,
                            color = if (current) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.onSurface,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 12.sp,
                            fontWeight = if (current) FontWeight.Bold else FontWeight.Normal,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun MemoryPanel(
    address: String,
    onAddressChange: (String) -> Unit,
    followsIp: Boolean,
    onFollowsIpChange: (Boolean) -> Unit,
    inspectable: Boolean,
    bytes: ByteArray,
    onRefresh: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Panel("MEMORY", modifier) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = address,
                onValueChange = onAddressChange,
                label = { Text("Address") },
                singleLine = true,
                modifier = Modifier.widthIn(max = 220.dp),
            )
            Spacer(Modifier.width(8.dp))
            OutlinedButton(onClick = onRefresh, enabled = inspectable) {
                Text("Read")
            }
            Spacer(Modifier.width(8.dp))
            OutlinedButton(onClick = { onFollowsIpChange(!followsIp) }) {
                Text(if (followsIp) "Following IP" else "Follow IP")
            }
        }
        Spacer(Modifier.height(8.dp))
        Text(
            if (inspectable) "256 bytes from $address"
            else "Live memory is locked · pause the VM to inspect",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            fontSize = 11.sp,
        )
        Spacer(Modifier.height(6.dp))
        Box(
            Modifier.weight(1f).fillMaxWidth()
                .background(Color(0xFF090E14), RoundedCornerShape(8.dp))
                .padding(10.dp),
        ) {
            SelectionContainer {
                Text(
                    if (bytes.isEmpty()) "No memory snapshot yet"
                    else bytes.toHexDump(parseAddress(address)),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                    lineHeight = 18.sp,
                    modifier = Modifier.fillMaxSize()
                        .verticalScroll(rememberScrollState()),
                )
            }
        }
    }
}

@Composable
internal fun Panel(
    title: String,
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    Card(modifier, colors = CardDefaults.cardColors(MaterialTheme.colorScheme.surface)) {
        Column(Modifier.fillMaxSize().padding(14.dp)) {
            Text(title, color = MaterialTheme.colorScheme.primary, fontSize = 11.sp,
                 fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(10.dp))
            content()
        }
    }
}

@Composable
private fun ErrorBanner(message: String, dismiss: () -> Unit) {
    Card(colors = CardDefaults.cardColors(MaterialTheme.colorScheme.error.copy(alpha = 0.14f))) {
        Row(Modifier.fillMaxWidth().padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(message, color = MaterialTheme.colorScheme.error, modifier = Modifier.weight(1f))
            OutlinedButton(onClick = dismiss) { Text("Dismiss") }
        }
    }
}

private fun parseAddress(value: String): UInt = value.trim()
    .removePrefix("0x")
    .removePrefix("0X")
    .toUIntOrNull(16) ?: 0u

private fun UInt.hex32(): String = "0x${toString(16).padStart(8, '0')}"
private fun ULong.hex64(): String = "0x${toString(16).padStart(8, '0')}"

private fun ByteArray.toHexDump(base: UInt): String =
    asList().chunked(16).mapIndexed { row, values ->
        val address = base + (row * 16).toUInt()
        val hex = values.joinToString(" ") { it.toUByte().toString(16).padStart(2, '0') }
        val ascii = values.joinToString("") {
            val code = it.toInt() and 0xff
            if (code in 32..126) code.toChar().toString() else "."
        }
        "${address.hex32()}  ${hex.padEnd(47)}  $ascii"
    }.joinToString("\n")

private fun Long.formatBytes(): String {
    if (this <= 0) return "0 B"
    val unit = 1024.0
    val exponent = (log10(toDouble()) / log10(unit)).toInt().coerceIn(0, 4)
    val labels = arrayOf("B", "KiB", "MiB", "GiB", "TiB")
    return "%.1f %s".format(this / unit.pow(exponent), labels[exponent])
}

private fun Long.formatCompact(): String = when {
    this >= 1_000_000_000 -> "%.2fG".format(this / 1_000_000_000.0)
    this >= 1_000_000 -> "%.2fM".format(this / 1_000_000.0)
    this >= 1_000 -> "%.1fK".format(this / 1_000.0)
    else -> toString()
}
