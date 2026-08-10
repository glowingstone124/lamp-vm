package dev.lampvm.debugger

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.awt.SwingPanel
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.rememberWindowState
import kotlinx.coroutines.delay
import java.awt.Dimension
import java.awt.Graphics
import java.awt.Graphics2D
import java.awt.MouseInfo
import java.awt.Point
import java.awt.RenderingHints
import java.awt.Robot
import java.awt.Toolkit
import java.awt.event.FocusAdapter
import java.awt.event.FocusEvent
import java.awt.event.KeyAdapter
import java.awt.event.KeyEvent
import java.awt.event.MouseAdapter
import java.awt.event.MouseEvent
import java.awt.event.MouseMotionAdapter
import java.awt.image.BufferedImage
import java.awt.image.DataBufferInt
import javax.swing.JPanel
import javax.swing.Timer

internal const val VGA_WIDTH = 640
internal const val VGA_HEIGHT = 480

@Composable
internal fun VmVgaWindow(
    session: LampVmSession,
    onCloseRequest: () -> Unit,
) {
    var frame by remember(session) {
        mutableStateOf(IntArray(VGA_WIDTH * VGA_HEIGHT))
    }

    LaunchedEffect(session) {
        while (true) {
            runCatching { session.readFramebuffer() }
                .onSuccess { frame = it }
            delay(50)
        }
    }

    Window(
        onCloseRequest = onCloseRequest,
        title = "LampVM Display",
        state = rememberWindowState(size = DpSize(800.dp, 640.dp)),
    ) {
        SwingPanel(
            factory = { VgaPanel(session) },
            update = { it.setFrame(frame) },
            background = Color.Black,
            modifier = Modifier.fillMaxSize().background(Color.Black),
        )
    }
}

private class VgaPanel(private val session: LampVmSession) : JPanel() {
    private val image = BufferedImage(VGA_WIDTH, VGA_HEIGHT, BufferedImage.TYPE_INT_ARGB)
    private val destination = (image.raster.dataBuffer as DataBufferInt).data
    private val pressedKeys = mutableMapOf<Int, Ps2Key>()
    private val robot = runCatching { Robot() }.getOrNull()
    private val hiddenCursor = runCatching {
        Toolkit.getDefaultToolkit().createCustomCursor(
            BufferedImage(16, 16, BufferedImage.TYPE_INT_ARGB),
            Point(0, 0),
            "lampvm-hidden-pointer",
        )
    }.getOrNull()
    private var captured = false
    private var mouseButtons = 0
    private var lastScreenPosition: Point? = null
    private var expectedWarpPosition: Point? = null
    private var pendingDeltaX = 0L
    private var pendingDeltaY = 0L
    private var inputError: String? = null
    private val mouseFlushTimer = Timer(16) { flushMouseMotion() }.apply {
        isCoalesce = true
    }

    init {
        background = java.awt.Color.BLACK
        preferredSize = Dimension(VGA_WIDTH, VGA_HEIGHT)
        isFocusable = true
        focusTraversalKeysEnabled = false

        addMouseListener(object : MouseAdapter() {
            override fun mousePressed(event: MouseEvent) {
                requestFocusInWindow()
                if (!captured) capturePointer()
                flushMouseMotion()
                mouseButtons = mouseButtons or event.button.toLampButton()
                sendMouseNow(0, 0)
            }

            override fun mouseReleased(event: MouseEvent) {
                flushMouseMotion()
                mouseButtons = mouseButtons and event.button.toLampButton().inv()
                sendMouseNow(0, 0)
            }
        })
        addMouseMotionListener(object : MouseMotionAdapter() {
            override fun mouseMoved(event: MouseEvent) = handleMotion(event)
            override fun mouseDragged(event: MouseEvent) = handleMotion(event)
        })
        addKeyListener(object : KeyAdapter() {
            override fun keyPressed(event: KeyEvent) {
                if (event.keyCode == KeyEvent.VK_G && event.isControlDown &&
                    (event.isAltDown || event.isMetaDown)
                ) {
                    releasePointer()
                    event.consume()
                    return
                }
                val key = awtToPs2(event.keyCode) ?: return
                if (pressedKeys.putIfAbsent(event.keyCode, key) == null) {
                    sendKey(key, pressed = true)
                }
                event.consume()
            }

            override fun keyReleased(event: KeyEvent) {
                pressedKeys.remove(event.keyCode)?.let { sendKey(it, pressed = false) }
                event.consume()
            }
        })
        addFocusListener(object : FocusAdapter() {
            override fun focusLost(event: FocusEvent) {
                releaseInputState()
                releasePointer()
            }
        })
    }

    fun setFrame(source: IntArray) {
        val count = minOf(source.size, destination.size)
        for (index in 0 until count) {
            destination[index] = source[index] or 0xff000000.toInt()
        }
        repaint()
    }

    override fun addNotify() {
        super.addNotify()
        mouseFlushTimer.start()
    }

    override fun removeNotify() {
        mouseFlushTimer.stop()
        releaseInputState()
        super.removeNotify()
    }

    override fun paintComponent(graphics: Graphics) {
        super.paintComponent(graphics)
        val scale = minOf(width.toDouble() / VGA_WIDTH, height.toDouble() / VGA_HEIGHT)
        val targetWidth = (VGA_WIDTH * scale).toInt().coerceAtLeast(1)
        val targetHeight = (VGA_HEIGHT * scale).toInt().coerceAtLeast(1)
        val x = (width - targetWidth) / 2
        val y = (height - targetHeight) / 2
        (graphics as Graphics2D).apply {
            setRenderingHint(
                RenderingHints.KEY_INTERPOLATION,
                RenderingHints.VALUE_INTERPOLATION_NEAREST_NEIGHBOR,
            )
            drawImage(image, x, y, targetWidth, targetHeight, null)
            color = java.awt.Color(0, 0, 0, 170)
            fillRoundRect(10, 10, 390, 28, 10, 10)
            color = if (captured) java.awt.Color(139, 233, 193)
            else java.awt.Color(210, 220, 232)
            drawString(
                if (captured) "Pointer captured · Ctrl+Alt/Command+G to release"
                else "Click display to capture keyboard and pointer",
                20,
                29,
            )
            inputError?.let { message ->
                color = java.awt.Color(255, 123, 134)
                drawString("Input error: $message", 20, 50)
            }
        }
    }

    private fun capturePointer() {
        captured = true
        hiddenCursor?.let { cursor = it }
        requestFocusInWindow()
        lastScreenPosition = MouseInfo.getPointerInfo()?.location
        recenterPointer()
        repaint()
    }

    private fun releasePointer() {
        captured = false
        expectedWarpPosition = null
        lastScreenPosition = null
        pendingDeltaX = 0
        pendingDeltaY = 0
        cursor = java.awt.Cursor.getDefaultCursor()
        repaint()
    }

    private fun handleMotion(event: MouseEvent) {
        if (!captured) return
        val current = Point(event.xOnScreen, event.yOnScreen)
        val warp = expectedWarpPosition
        if (warp != null && current.distanceSq(warp) <= 9L) {
            expectedWarpPosition = null
            lastScreenPosition = current
            return
        }

        val previous = lastScreenPosition
        lastScreenPosition = current
        if (previous != null) {
            pendingDeltaX = (pendingDeltaX + current.x - previous.x)
                .coerceIn(-32_768L, 32_768L)
            pendingDeltaY = (pendingDeltaY + current.y - previous.y)
                .coerceIn(-32_768L, 32_768L)
        }

        // Warp only near an edge. Warping after every event can produce a
        // synthetic reverse movement on macOS/HiDPI displays and cancel the
        // physical delta before it reaches the guest.
        if (event.x < 48 || event.y < 48 ||
            event.x >= width - 48 || event.y >= height - 48
        ) {
            recenterPointer()
        }
    }

    private fun recenterPointer() {
        val mover = robot ?: return
        val origin = runCatching { locationOnScreen }.getOrNull() ?: return
        val center = Point(origin.x + width / 2, origin.y + height / 2)
        expectedWarpPosition = center
        mover.mouseMove(center.x, center.y)
    }

    private fun releaseInputState() {
        pressedKeys.values.forEach { sendKey(it, pressed = false) }
        pressedKeys.clear()
        flushMouseMotion()
        if (mouseButtons != 0) {
            mouseButtons = 0
            sendMouseNow(0, 0)
        }
    }

    private fun sendKey(key: Ps2Key, pressed: Boolean) {
        runCatching { session.sendKey(key.scanCode, key.extended, pressed) }
            .onSuccess {
                inputError = null
            }
            .onFailure {
                inputError = it.message ?: "keyboard input failed"
                repaint()
            }
    }

    private fun flushMouseMotion() {
        if (pendingDeltaX == 0L && pendingDeltaY == 0L) return
        val deltaX = pendingDeltaX.coerceIn(Int.MIN_VALUE.toLong(), Int.MAX_VALUE.toLong()).toInt()
        val deltaY = pendingDeltaY.coerceIn(Int.MIN_VALUE.toLong(), Int.MAX_VALUE.toLong()).toInt()
        pendingDeltaX = 0
        pendingDeltaY = 0
        sendMouseNow(deltaX, deltaY)
    }

    private fun sendMouseNow(deltaX: Int, deltaY: Int) {
        runCatching { session.sendMouse(deltaX, deltaY, mouseButtons and 0x07) }
            .onSuccess {
                inputError = null
            }
            .onFailure {
                inputError = it.message ?: "mouse input failed"
                repaint()
            }
    }
}

private fun Point.distanceSq(other: Point): Long {
    val dx = x.toLong() - other.x.toLong()
    val dy = y.toLong() - other.y.toLong()
    return dx * dx + dy * dy
}

private data class Ps2Key(val scanCode: Int, val extended: Boolean = false)

private fun Int.toLampButton(): Int = when (this) {
    MouseEvent.BUTTON1 -> 0x01
    MouseEvent.BUTTON2 -> 0x04
    MouseEvent.BUTTON3 -> 0x02
    else -> 0
}

private fun awtToPs2(keyCode: Int): Ps2Key? = when (keyCode) {
    KeyEvent.VK_ESCAPE -> Ps2Key(0x01)
    KeyEvent.VK_1 -> Ps2Key(0x02)
    KeyEvent.VK_2 -> Ps2Key(0x03)
    KeyEvent.VK_3 -> Ps2Key(0x04)
    KeyEvent.VK_4 -> Ps2Key(0x05)
    KeyEvent.VK_5 -> Ps2Key(0x06)
    KeyEvent.VK_6 -> Ps2Key(0x07)
    KeyEvent.VK_7 -> Ps2Key(0x08)
    KeyEvent.VK_8 -> Ps2Key(0x09)
    KeyEvent.VK_9 -> Ps2Key(0x0a)
    KeyEvent.VK_0 -> Ps2Key(0x0b)
    KeyEvent.VK_MINUS -> Ps2Key(0x0c)
    KeyEvent.VK_EQUALS -> Ps2Key(0x0d)
    KeyEvent.VK_BACK_SPACE -> Ps2Key(0x0e)
    KeyEvent.VK_TAB -> Ps2Key(0x0f)
    KeyEvent.VK_Q -> Ps2Key(0x10)
    KeyEvent.VK_W -> Ps2Key(0x11)
    KeyEvent.VK_E -> Ps2Key(0x12)
    KeyEvent.VK_R -> Ps2Key(0x13)
    KeyEvent.VK_T -> Ps2Key(0x14)
    KeyEvent.VK_Y -> Ps2Key(0x15)
    KeyEvent.VK_U -> Ps2Key(0x16)
    KeyEvent.VK_I -> Ps2Key(0x17)
    KeyEvent.VK_O -> Ps2Key(0x18)
    KeyEvent.VK_P -> Ps2Key(0x19)
    KeyEvent.VK_OPEN_BRACKET -> Ps2Key(0x1a)
    KeyEvent.VK_CLOSE_BRACKET -> Ps2Key(0x1b)
    KeyEvent.VK_ENTER -> Ps2Key(0x1c)
    KeyEvent.VK_CONTROL -> Ps2Key(0x1d)
    KeyEvent.VK_A -> Ps2Key(0x1e)
    KeyEvent.VK_S -> Ps2Key(0x1f)
    KeyEvent.VK_D -> Ps2Key(0x20)
    KeyEvent.VK_F -> Ps2Key(0x21)
    KeyEvent.VK_G -> Ps2Key(0x22)
    KeyEvent.VK_H -> Ps2Key(0x23)
    KeyEvent.VK_J -> Ps2Key(0x24)
    KeyEvent.VK_K -> Ps2Key(0x25)
    KeyEvent.VK_L -> Ps2Key(0x26)
    KeyEvent.VK_SEMICOLON -> Ps2Key(0x27)
    KeyEvent.VK_QUOTE -> Ps2Key(0x28)
    KeyEvent.VK_BACK_QUOTE -> Ps2Key(0x29)
    KeyEvent.VK_SHIFT -> Ps2Key(0x2a)
    KeyEvent.VK_BACK_SLASH -> Ps2Key(0x2b)
    KeyEvent.VK_Z -> Ps2Key(0x2c)
    KeyEvent.VK_X -> Ps2Key(0x2d)
    KeyEvent.VK_C -> Ps2Key(0x2e)
    KeyEvent.VK_V -> Ps2Key(0x2f)
    KeyEvent.VK_B -> Ps2Key(0x30)
    KeyEvent.VK_N -> Ps2Key(0x31)
    KeyEvent.VK_M -> Ps2Key(0x32)
    KeyEvent.VK_COMMA -> Ps2Key(0x33)
    KeyEvent.VK_PERIOD -> Ps2Key(0x34)
    KeyEvent.VK_SLASH -> Ps2Key(0x35)
    KeyEvent.VK_ALT, KeyEvent.VK_META -> Ps2Key(0x38)
    KeyEvent.VK_SPACE -> Ps2Key(0x39)
    KeyEvent.VK_CAPS_LOCK -> Ps2Key(0x3a)
    KeyEvent.VK_UP -> Ps2Key(0x48, extended = true)
    KeyEvent.VK_LEFT -> Ps2Key(0x4b, extended = true)
    KeyEvent.VK_RIGHT -> Ps2Key(0x4d, extended = true)
    KeyEvent.VK_DOWN -> Ps2Key(0x50, extended = true)
    KeyEvent.VK_DELETE -> Ps2Key(0x53, extended = true)
    else -> null
}
