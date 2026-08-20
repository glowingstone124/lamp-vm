package dev.lampvm.debugger

internal class DeferredSerialInput(
    private val capacity: Int = DEFAULT_CAPACITY,
) {
    private val pending = ArrayDeque<Byte>()

    val size: Int
        get() = pending.size

    fun enqueue(data: ByteArray) {
        require(data.size <= capacity - pending.size) {
            "terminal input queue is full (${pending.size}/$capacity bytes)"
        }
        data.forEach(pending::addLast)
    }

    fun flush(
        batchSize: Int = DEFAULT_BATCH_SIZE,
        writer: (ByteArray) -> Int,
    ): Int {
        if (pending.isEmpty()) return 0
        require(batchSize > 0) { "serial write batch size must be positive" }

        val batch = ByteArray(minOf(batchSize, pending.size))
        val iterator = pending.iterator()
        for (index in batch.indices) {
            batch[index] = iterator.next()
        }
        val written = writer(batch)
        check(written in 0..batch.size) {
            "native serial write returned invalid count $written/${batch.size}"
        }
        repeat(written) { pending.removeFirst() }
        return written
    }

    fun clear() {
        pending.clear()
    }

    private companion object {
        const val DEFAULT_BATCH_SIZE = 8_192
        const val DEFAULT_CAPACITY = 1024 * 1024
    }
}
