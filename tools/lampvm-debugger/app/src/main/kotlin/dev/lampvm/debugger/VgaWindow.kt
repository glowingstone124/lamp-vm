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
import kotlin.math.roundToInt
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
        title = "Octans Display",
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
    private var reportedMouseButtons = 0
    private var lastScreenPosition: Point? = null
    private var expectedWarpPosition: Point? = null
    private var pointerWarpAvailable = false
    private var guestCapsLock = false
    private var guestNumLock = false
    private var pendingDeltaX = 0.0
    private var pendingDeltaY = 0.0
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
                    releaseInputState()
                    releasePointer()
                    event.consume()
                    return
                }
                val key = awtToPs2(event) ?: return
                val identity = event.keyCode * 8 + event.keyLocation
                if (pressedKeys.putIfAbsent(identity, key) == null &&
                    !sendKey(key, pressed = true)
                ) {
                    pressedKeys.remove(identity)
                }
                event.consume()
            }

            override fun keyReleased(event: KeyEvent) {
                val identity = event.keyCode * 8 + event.keyLocation
                pressedKeys[identity]?.let { key ->
                    if (sendKey(key, pressed = false)) {
                        pressedKeys.remove(identity)
                    }
                }
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
                if (captured) "Pointer captured, Ctrl+Alt/Command+G to release"
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
        /* Without Robot (for example before Accessibility permission is
         * granted on macOS) the pointer cannot be recentered. Keep it visible
         * instead of hiding a pointer that will stop at the panel edge. */
        cursor = java.awt.Cursor.getDefaultCursor()
        lastScreenPosition = runCatching { MouseInfo.getPointerInfo()?.location }
            .getOrNull()
        syncHostLockState()
        requestFocusInWindow()
        pointerWarpAvailable = recenterPointer()
        if (pointerWarpAvailable) {
            hiddenCursor?.let { cursor = it }
        }
        repaint()
    }

    private fun syncHostLockState() {
        val toolkit = Toolkit.getDefaultToolkit()
        listOf(
            KeyEvent.VK_CAPS_LOCK to 0x3a,
            KeyEvent.VK_NUM_LOCK to 0x45,
        ).forEach { (keyCode, scanCode) ->
            val enabled = runCatching { toolkit.getLockingKeyState(keyCode) }
                .getOrDefault(false)
            if (enabled) {
                val key = Ps2Key(scanCode)
                if (sendKey(key, pressed = true)) {
                    sendKey(key, pressed = false)
                }
            }
        }
    }

    private fun releasePointer() {
        captured = false
        expectedWarpPosition = null
        pointerWarpAvailable = false
        pendingDeltaX = 0.0
        pendingDeltaY = 0.0
        lastScreenPosition = null
        cursor = java.awt.Cursor.getDefaultCursor()
        repaint()
    }

    private fun handleMotion(event: MouseEvent) {
        if (!captured) return
        expectedWarpPosition?.let { expected ->
            val errorX = event.xOnScreen - expected.x
            val errorY = event.yOnScreen - expected.y
            if (kotlin.math.abs(errorX) <= 3 && kotlin.math.abs(errorY) <= 3) {
                expectedWarpPosition = null
                lastScreenPosition = Point(event.xOnScreen, event.yOnScreen)
                return
            }
        }
        val current = Point(event.xOnScreen, event.yOnScreen)
        val previous = lastScreenPosition
        lastScreenPosition = current
        if (previous == null) return
        val dx = current.x - previous.x
        val dy = current.y - previous.y
        if (dx == 0 && dy == 0) return

        val scale = guestScale()
        pendingDeltaX = (pendingDeltaX + dx / scale).coerceIn(-8_192.0, 8_192.0)
        pendingDeltaY = (pendingDeltaY + dy / scale).coerceIn(-8_192.0, 8_192.0)

        /* Keep normal motion relative and warp only near an edge. A warp for
         * every event can arrive after the next real event and double-count
         * absolute screen coordinates on macOS. */
        if (event.x < 48 || event.y < 48 ||
            event.x >= width - 48 || event.y >= height - 48
        ) {
            recenterPointer()
        }
    }

    private fun recenterPointer(): Boolean {
        val mover = robot ?: return false
        val origin = runCatching { locationOnScreen }.getOrNull() ?: return false
        val target = Point(origin.x + width / 2, origin.y + height / 2)
        expectedWarpPosition = target
        return runCatching {
            mover.mouseMove(target.x, target.y)
            true
        }.getOrElse {
            expectedWarpPosition = null
            pointerWarpAvailable = false
            cursor = java.awt.Cursor.getDefaultCursor()
            false
        }
    }

    private fun releaseInputState() {
        pressedKeys.toMap().forEach { (identity, key) ->
            if (sendKey(key, pressed = false)) {
                pressedKeys.remove(identity)
            }
        }
        listOf(
            guestCapsLock to Ps2Key(0x3a),
            guestNumLock to Ps2Key(0x45),
        ).forEach { (enabled, key) ->
            if (enabled && sendKey(key, pressed = true)) {
                sendKey(key, pressed = false)
            }
        }
        flushMouseMotion()
        if (mouseButtons != 0 || reportedMouseButtons != 0) {
            mouseButtons = 0
            sendMouseNow(0, 0)
        }
    }

    private fun sendKey(key: Ps2Key, pressed: Boolean): Boolean {
        return runCatching {
            session.sendKey(key.scanCode, key.extended, pressed)
        }.fold(
            onSuccess = {
                if (pressed && !key.extended) {
                    when (key.scanCode) {
                        0x3a -> guestCapsLock = !guestCapsLock
                        0x45 -> guestNumLock = !guestNumLock
                    }
                }
                inputError = null
                true
            },
            onFailure = {
                inputError = it.message ?: "keyboard input failed"
                repaint()
                false
            },
        )
    }

    private fun flushMouseMotion() {
        session.flushPendingInput()
        if (pendingDeltaX == 0.0 && pendingDeltaY == 0.0 &&
            mouseButtons == reportedMouseButtons
        ) return
        val deltaX = pendingDeltaX.roundToInt()
        val deltaY = pendingDeltaY.roundToInt()
        if (deltaX == 0 && deltaY == 0 &&
            mouseButtons == reportedMouseButtons
        ) return
        if (sendMouseNow(deltaX, deltaY)) {
            pendingDeltaX -= deltaX.toDouble()
            pendingDeltaY -= deltaY.toDouble()
        }
    }

    private fun sendMouseNow(deltaX: Int, deltaY: Int): Boolean {
        return runCatching {
            session.sendMouse(deltaX, deltaY, mouseButtons and 0x07)
        }.fold(
            onSuccess = {
                reportedMouseButtons = mouseButtons and 0x07
                inputError = null
                true
            },
            onFailure = {
                inputError = it.message ?: "mouse input failed"
                repaint()
                false
            },
        )
    }

    private fun guestScale(): Double {
        if (width <= 0 || height <= 0) return 1.0
        return minOf(width.toDouble() / VGA_WIDTH, height.toDouble() / VGA_HEIGHT)
    }
}

private data class Ps2Key(val scanCode: Int, val extended: Boolean = false)

private fun Int.toLampButton(): Int = when (this) {
    MouseEvent.BUTTON1 -> 0x01
    MouseEvent.BUTTON2 -> 0x04
    MouseEvent.BUTTON3 -> 0x02
    else -> 0
}

private fun awtToPs2(event: KeyEvent): Ps2Key? = when (event.keyCode) {
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
    KeyEvent.VK_ENTER -> Ps2Key(0x1c, event.keyLocation == KeyEvent.KEY_LOCATION_NUMPAD)
    KeyEvent.VK_CONTROL -> Ps2Key(0x1d, event.keyLocation == KeyEvent.KEY_LOCATION_RIGHT)
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
    KeyEvent.VK_SHIFT -> Ps2Key(
        if (event.keyLocation == KeyEvent.KEY_LOCATION_RIGHT) 0x36 else 0x2a,
    )
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
    KeyEvent.VK_ALT -> Ps2Key(
        0x38,
        event.keyLocation == KeyEvent.KEY_LOCATION_RIGHT,
    )
    KeyEvent.VK_ALT_GRAPH -> Ps2Key(0x38, extended = true)
    KeyEvent.VK_META -> Ps2Key(
        if (event.keyLocation == KeyEvent.KEY_LOCATION_RIGHT) 0x5c else 0x5b,
        extended = true,
    )
    KeyEvent.VK_SPACE -> Ps2Key(0x39)
    KeyEvent.VK_CAPS_LOCK -> Ps2Key(0x3a)
    KeyEvent.VK_UP -> Ps2Key(0x48, extended = true)
    KeyEvent.VK_LEFT -> Ps2Key(0x4b, extended = true)
    KeyEvent.VK_RIGHT -> Ps2Key(0x4d, extended = true)
    KeyEvent.VK_DOWN -> Ps2Key(0x50, extended = true)
    KeyEvent.VK_DELETE -> Ps2Key(0x53, extended = true)
    KeyEvent.VK_INSERT -> Ps2Key(0x52, extended = true)
    KeyEvent.VK_HOME -> Ps2Key(0x47, extended = true)
    KeyEvent.VK_END -> Ps2Key(0x4f, extended = true)
    KeyEvent.VK_PAGE_UP -> Ps2Key(0x49, extended = true)
    KeyEvent.VK_PAGE_DOWN -> Ps2Key(0x51, extended = true)
    KeyEvent.VK_NUM_LOCK -> Ps2Key(0x45)
    KeyEvent.VK_SCROLL_LOCK -> Ps2Key(0x46)
    KeyEvent.VK_F1 -> Ps2Key(0x3b)
    KeyEvent.VK_F2 -> Ps2Key(0x3c)
    KeyEvent.VK_F3 -> Ps2Key(0x3d)
    KeyEvent.VK_F4 -> Ps2Key(0x3e)
    KeyEvent.VK_F5 -> Ps2Key(0x3f)
    KeyEvent.VK_F6 -> Ps2Key(0x40)
    KeyEvent.VK_F7 -> Ps2Key(0x41)
    KeyEvent.VK_F8 -> Ps2Key(0x42)
    KeyEvent.VK_F9 -> Ps2Key(0x43)
    KeyEvent.VK_F10 -> Ps2Key(0x44)
    KeyEvent.VK_F11 -> Ps2Key(0x57)
    KeyEvent.VK_F12 -> Ps2Key(0x58)
    KeyEvent.VK_NUMPAD0 -> keypadKey(event, 0x52)
    KeyEvent.VK_NUMPAD1 -> keypadKey(event, 0x4f)
    KeyEvent.VK_NUMPAD2 -> keypadKey(event, 0x50)
    KeyEvent.VK_NUMPAD3 -> keypadKey(event, 0x51)
    KeyEvent.VK_NUMPAD4 -> keypadKey(event, 0x4b)
    KeyEvent.VK_NUMPAD5 -> keypadKey(event, 0x4c)
    KeyEvent.VK_NUMPAD6 -> keypadKey(event, 0x4d)
    KeyEvent.VK_NUMPAD7 -> keypadKey(event, 0x47)
    KeyEvent.VK_NUMPAD8 -> keypadKey(event, 0x48)
    KeyEvent.VK_NUMPAD9 -> keypadKey(event, 0x49)
    KeyEvent.VK_DECIMAL -> keypadKey(event, 0x53)
    KeyEvent.VK_ADD -> Ps2Key(0x4e)
    KeyEvent.VK_SUBTRACT -> Ps2Key(0x4a)
    KeyEvent.VK_MULTIPLY -> Ps2Key(0x37)
    KeyEvent.VK_DIVIDE -> Ps2Key(0x35, extended = true)
    else -> null
}

private fun keypadKey(event: KeyEvent, scanCode: Int): Ps2Key {
    /* AWT uses the same VK_NUMPAD* codes for both modes. The key character
     * tells us whether this event represents a digit or navigation; the
     * resulting Ps/2 key is retained for the matching release event. */
    val numeric = event.keyChar in '0'..'9' ||
        (scanCode == 0x53 && event.keyChar == '.')
    return Ps2Key(scanCode, extended = !numeric)
}
