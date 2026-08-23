package dev.lampvm.debugger

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.awt.SwingPanel
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.jediterm.terminal.TerminalColor
import com.jediterm.terminal.TextStyle
import com.jediterm.terminal.TtyConnector
import com.jediterm.terminal.ui.JediTermWidget
import com.jediterm.terminal.ui.settings.DefaultSettingsProvider
import java.awt.BorderLayout
import java.awt.Font
import java.io.IOException
import javax.swing.JPanel
import kotlinx.coroutines.delay
import kotlin.time.Duration.Companion.milliseconds

@Composable
internal fun TerminalPanel(
    session: LampVmSession?,
    state: VmState?,
    modifier: Modifier = Modifier,
) {
    Panel("SERIAL TERMINAL", modifier) {
        if (session == null) {
            Text(
                "Connect a VM to open the terminal",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            key(session) {
                var pendingInputBytes by remember { mutableIntStateOf(0) }
                LaunchedEffect(session) {
                    while (true) {
                        pendingInputBytes = session.pendingSerialInputBytes
                        delay(50.milliseconds)
                    }
                }
                Column(Modifier.fillMaxSize()) {
                    if (state == VmState.Paused) {
                        Text(
                            if (pendingInputBytes == 0) {
                                "PAUSED terminal input will be sent on STEP"
                            } else {
                                "PAUSED $pendingInputBytes bytes queued for STEP"
                            },
                            color = MaterialTheme.colorScheme.secondary,
                            style = MaterialTheme.typography.labelSmall,
                            modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                        )
                    }
                    SwingPanel(
                        factory = { JediSerialTerminal(session) },
                        background = Color(0xFF090E14),
                        modifier = Modifier.fillMaxSize().weight(1f),
                    )
                }
            }
        }
    }
}

private class JediSerialTerminal(session: LampVmSession) : JPanel(BorderLayout()) {
    private val connector = LampSerialConnector(session)
    private val terminal = JediTermWidget(LampTerminalSettings())

    init {
        background = java.awt.Color(0x09, 0x0e, 0x14)
        terminal.setTtyConnector(connector)
        add(terminal, BorderLayout.CENTER)
        terminal.start()
    }

    override fun removeNotify() {
        connector.close()
        terminal.close()
        super.removeNotify()
    }
}

@Suppress("OVERRIDE_DEPRECATION")
private class LampTerminalSettings : DefaultSettingsProvider() {
    override fun getTerminalFont(): Font = Font(Font.MONOSPACED, Font.PLAIN, 13)
    override fun getTerminalFontSize(): Float = 13f
    override fun getDefaultForeground(): TerminalColor = TerminalColor.rgb(230, 237, 246)
    override fun getDefaultBackground(): TerminalColor = TerminalColor.rgb(9, 14, 20)
    override fun getDefaultStyle(): TextStyle = TextStyle(
        getDefaultForeground(),
        getDefaultBackground(),
    )
    override fun getBufferMaxLinesCount(): Int = 20_000
    override fun audibleBell(): Boolean = false
    override fun copyOnSelect(): Boolean = true
    override fun scrollToBottomOnTyping(): Boolean = true
}

private class LampSerialConnector(
    private val session: LampVmSession,
) : TtyConnector {
    @Volatile
    private var connected = true
    private var pending = ByteArray(0)
    private var pendingOffset = 0

    override fun read(buffer: CharArray, offset: Int, length: Int): Int {
        while (connected) {
            if (pendingOffset >= pending.size) {
                pending = try {
                    session.readSerial(maxOf(length, 8_192))
                } catch (_: Throwable) {
                    connected = false
                    return -1
                }
                pendingOffset = 0
            }
            if (pending.isNotEmpty()) {
                val count = minOf(length, pending.size - pendingOffset)
                for (index in 0 until count) {
                    buffer[offset + index] = (pending[pendingOffset + index].toInt() and 0xff).toChar()
                }
                pendingOffset += count
                return count
            }
            try {
                Thread.sleep(8)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
                return -1
            }
        }
        return -1
    }

    override fun write(bytes: ByteArray) {
        ensureConnected()
        try {
            val written = session.writeSerial(bytes)
            if (written != bytes.size) {
                throw IOException("guest serial accepted $written/${bytes.size} bytes")
            }
        } catch (failure: Throwable) {
            connected = false
            throw IOException("LampVM serial write failed", failure)
        }
    }

    override fun write(value: String) = write(value.encodeToByteArray())
    override fun isConnected(): Boolean = connected
    override fun ready(): Boolean = connected && pendingOffset < pending.size
    override fun getName(): String = "LampVM serial"

    override fun waitFor(): Int {
        while (connected) Thread.sleep(20)
        return 0
    }

    override fun close() {
        connected = false
    }

    private fun ensureConnected() {
        if (!connected) throw IOException("LampVM serial is closed")
    }

}
