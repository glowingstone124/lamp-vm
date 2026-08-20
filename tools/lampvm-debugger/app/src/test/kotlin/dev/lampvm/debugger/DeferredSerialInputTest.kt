package dev.lampvm.debugger

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class DeferredSerialInputTest {
    @Test
    fun preservesInputAcrossPartialStepWrites() {
        val input = DeferredSerialInput()
        val delivered = mutableListOf<Byte>()

        input.enqueue("hello".encodeToByteArray())
        assertEquals(5, input.size)
        assertEquals(2, input.flush { batch ->
            delivered += batch.take(2)
            2
        })
        assertEquals(3, input.size)

        assertEquals(3, input.flush { batch ->
            delivered += batch.toList()
            batch.size
        })
        assertEquals(0, input.size)
        assertContentEquals("hello".encodeToByteArray(), delivered.toByteArray())
    }

    @Test
    fun keepsBytesQueuedWhenGuestRxFifoIsFull() {
        val input = DeferredSerialInput()
        input.enqueue(byteArrayOf(1, 2, 3))

        assertEquals(0, input.flush { 0 })
        assertEquals(3, input.size)
    }

    @Test
    fun rejectsInputBeyondConfiguredCapacity() {
        val input = DeferredSerialInput(capacity = 3)
        input.enqueue(byteArrayOf(1, 2))

        assertFailsWith<IllegalArgumentException> {
            input.enqueue(byteArrayOf(3, 4))
        }
        assertEquals(2, input.size)
    }
}
