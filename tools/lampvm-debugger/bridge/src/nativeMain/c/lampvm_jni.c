#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

extern int64_t lamp_kn_create(const char *program_path,
                              const char *disk_path,
                              int64_t memory_bytes,
                              int64_t cpu_frequency_hz,
                              int32_t core_count,
                              int32_t execution_engine);
extern int32_t lamp_kn_start(int64_t handle);
extern int32_t lamp_kn_pause(int64_t handle, int64_t timeout_millis);
extern int32_t lamp_kn_step(int64_t handle, int32_t core_id,
                            int64_t timeout_millis);
extern int32_t lamp_kn_resume(int64_t handle);
extern int32_t lamp_kn_stop(int64_t handle);
extern void lamp_kn_destroy(int64_t handle);
extern int32_t lamp_kn_state(int64_t handle);
extern int32_t lamp_kn_stats(int64_t handle, uint64_t *output, int32_t capacity);
extern int32_t lamp_kn_cpu(int64_t handle, int32_t core_id,
                           uint64_t *output, int32_t capacity);
extern int32_t lamp_kn_read_memory(int64_t handle, int32_t address,
                                  uint8_t *destination, int32_t size);
extern int32_t lamp_kn_read_framebuffer(int64_t handle,
                                       uint32_t *destination,
                                       int32_t pixel_capacity);
extern int32_t lamp_kn_send_key(int64_t handle, int32_t scan_code,
                                int32_t extended, int32_t pressed);
extern int32_t lamp_kn_send_mouse(int64_t handle, int32_t delta_x,
                                  int32_t delta_y, int32_t buttons);
extern int32_t lamp_kn_serial_read(int64_t handle, uint8_t *destination,
                                  int32_t capacity);
extern int32_t lamp_kn_serial_write(int64_t handle, const uint8_t *source,
                                   int32_t size);
extern int32_t lamp_kn_last_error(int64_t handle, uint8_t *destination,
                                 int32_t capacity);

static void throw_native_error(JNIEnv *env, jlong handle,
                               const char *fallback) {
    uint8_t message[512];
    jclass exception_class;
    const int32_t size = lamp_kn_last_error((int64_t)handle, message,
                                            (int32_t)sizeof(message));
    exception_class = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (exception_class) {
        (*env)->ThrowNew(env, exception_class,
                         size > 0 ? (const char *)message : fallback);
    }
}

JNIEXPORT jlong JNICALL
Java_dev_lampvm_debugger_NativeBindings_create(
    JNIEnv *env, jobject self, jstring program_path, jstring disk_path,
    jlong memory_bytes, jlong cpu_frequency_hz, jint core_count,
    jint execution_engine) {
    const char *program;
    const char *disk;
    int64_t result;
    (void)self;
    if (!program_path || !disk_path) {
        throw_native_error(env, 0, "program and disk paths must not be null");
        return 0;
    }
    program = (*env)->GetStringUTFChars(env, program_path, NULL);
    disk = (*env)->GetStringUTFChars(env, disk_path, NULL);
    if (!program || !disk) {
        if (program) (*env)->ReleaseStringUTFChars(env, program_path, program);
        if (disk) (*env)->ReleaseStringUTFChars(env, disk_path, disk);
        return 0;
    }
    result = lamp_kn_create(program, disk, (int64_t)memory_bytes,
                            (int64_t)cpu_frequency_hz, (int32_t)core_count,
                            (int32_t)execution_engine);
    (*env)->ReleaseStringUTFChars(env, program_path, program);
    (*env)->ReleaseStringUTFChars(env, disk_path, disk);
    if (result == 0) {
        throw_native_error(env, 0, "failed to create Lamp VM");
    }
    return (jlong)result;
}

static void check_status(JNIEnv *env, jlong handle, int32_t status,
                         const char *fallback) {
    if (status != 0) throw_native_error(env, handle, fallback);
}

JNIEXPORT void JNICALL
Java_dev_lampvm_debugger_NativeBindings_start(JNIEnv *env, jobject self,
                                               jlong handle) {
    (void)self;
    check_status(env, handle, lamp_kn_start((int64_t)handle),
                 "failed to start VM");
}

JNIEXPORT void JNICALL
Java_dev_lampvm_debugger_NativeBindings_pause(JNIEnv *env, jobject self,
                                               jlong handle,
                                               jlong timeout_millis) {
    (void)self;
    check_status(env, handle,
                 lamp_kn_pause((int64_t)handle, (int64_t)timeout_millis),
                 "failed to pause VM");
}

JNIEXPORT void JNICALL
Java_dev_lampvm_debugger_NativeBindings_step(JNIEnv *env, jobject self,
                                              jlong handle, jint core_id,
                                              jlong timeout_millis) {
    (void)self;
    check_status(env, handle,
                 lamp_kn_step((int64_t)handle, (int32_t)core_id,
                              (int64_t)timeout_millis),
                 "failed to single-step VM");
}

JNIEXPORT void JNICALL
Java_dev_lampvm_debugger_NativeBindings_resume(JNIEnv *env, jobject self,
                                                jlong handle) {
    (void)self;
    check_status(env, handle, lamp_kn_resume((int64_t)handle),
                 "failed to resume VM");
}

JNIEXPORT void JNICALL
Java_dev_lampvm_debugger_NativeBindings_stop(JNIEnv *env, jobject self,
                                              jlong handle) {
    (void)self;
    check_status(env, handle, lamp_kn_stop((int64_t)handle),
                 "failed to stop VM");
}

JNIEXPORT void JNICALL
Java_dev_lampvm_debugger_NativeBindings_destroy(JNIEnv *env, jobject self,
                                                 jlong handle) {
    (void)env;
    (void)self;
    lamp_kn_destroy((int64_t)handle);
}

JNIEXPORT jint JNICALL
Java_dev_lampvm_debugger_NativeBindings_state(JNIEnv *env, jobject self,
                                               jlong handle) {
    (void)env;
    (void)self;
    return (jint)lamp_kn_state((int64_t)handle);
}

JNIEXPORT jlongArray JNICALL
Java_dev_lampvm_debugger_NativeBindings_stats(JNIEnv *env, jobject self,
                                               jlong handle) {
    uint64_t values[10];
    jlongArray result;
    (void)self;
    const int32_t status = lamp_kn_stats((int64_t)handle, values, 10);
    if (status != 0) {
        throw_native_error(env, handle, "failed to read VM statistics");
        return NULL;
    }
    result = (*env)->NewLongArray(env, 10);
    if (result) {
        (*env)->SetLongArrayRegion(env, result, 0, 10, (const jlong *)values);
    }
    return result;
}

JNIEXPORT jlongArray JNICALL
Java_dev_lampvm_debugger_NativeBindings_cpu(JNIEnv *env, jobject self,
                                             jlong handle, jint core_id) {
    uint64_t values[43];
    jlongArray result;
    (void)self;
    const int32_t status = lamp_kn_cpu((int64_t)handle, (int32_t)core_id,
                                       values, 43);
    if (status != 0) {
        throw_native_error(env, handle, "failed to read CPU state");
        return NULL;
    }
    result = (*env)->NewLongArray(env, 43);
    if (result) {
        (*env)->SetLongArrayRegion(env, result, 0, 43, (const jlong *)values);
    }
    return result;
}

JNIEXPORT jbyteArray JNICALL
Java_dev_lampvm_debugger_NativeBindings_readMemory(
    JNIEnv *env, jobject self, jlong handle, jint address, jint size) {
    jbyteArray result;
    jbyte *bytes;
    int32_t status;
    (void)self;
    if (size < 0) {
        throw_native_error(env, handle, "memory size must not be negative");
        return NULL;
    }
    result = (*env)->NewByteArray(env, size);
    if (!result || size == 0) return result;
    bytes = (*env)->GetByteArrayElements(env, result, NULL);
    if (!bytes) return NULL;
    status = lamp_kn_read_memory((int64_t)handle, (int32_t)address,
                                 (uint8_t *)bytes, (int32_t)size);
    (*env)->ReleaseByteArrayElements(env, result, bytes,
                                     status == 0 ? 0 : JNI_ABORT);
    if (status != 0) {
        throw_native_error(env, handle, "failed to read guest memory");
        return NULL;
    }
    return result;
}

JNIEXPORT jintArray JNICALL
Java_dev_lampvm_debugger_NativeBindings_readFramebuffer(
    JNIEnv *env, jobject self, jlong handle) {
    enum { WIDTH = 640, HEIGHT = 480, PIXELS = WIDTH * HEIGHT };
    uint32_t *pixels;
    jintArray result;
    int32_t status;
    (void)self;
    pixels = malloc((size_t)PIXELS * sizeof(uint32_t));
    if (!pixels) return NULL;
    status = lamp_kn_read_framebuffer((int64_t)handle, pixels, PIXELS);
    if (status != 0) {
        free(pixels);
        throw_native_error(env, handle, "failed to read VM framebuffer");
        return NULL;
    }
    result = (*env)->NewIntArray(env, PIXELS);
    if (result) {
        (*env)->SetIntArrayRegion(env, result, 0, PIXELS,
                                  (const jint *)pixels);
    }
    free(pixels);
    return result;
}

JNIEXPORT void JNICALL
Java_dev_lampvm_debugger_NativeBindings_sendKey(
    JNIEnv *env, jobject self, jlong handle, jint scan_code,
    jboolean extended, jboolean pressed) {
    (void)self;
    check_status(env, handle,
                 lamp_kn_send_key((int64_t)handle, (int32_t)scan_code,
                                  extended ? 1 : 0, pressed ? 1 : 0),
                 "failed to send guest keyboard input");
}

JNIEXPORT void JNICALL
Java_dev_lampvm_debugger_NativeBindings_sendMouse(
    JNIEnv *env, jobject self, jlong handle, jint delta_x, jint delta_y,
    jint buttons) {
    (void)self;
    check_status(env, handle,
                 lamp_kn_send_mouse((int64_t)handle, (int32_t)delta_x,
                                    (int32_t)delta_y, (int32_t)buttons),
                 "failed to send guest mouse input");
}

JNIEXPORT jbyteArray JNICALL
Java_dev_lampvm_debugger_NativeBindings_serialRead(
    JNIEnv *env, jobject self, jlong handle, jint capacity) {
    uint8_t *buffer;
    int32_t count;
    jbyteArray result;
    (void)self;
    if (capacity <= 0) return (*env)->NewByteArray(env, 0);
    buffer = malloc((size_t)capacity);
    if (!buffer) return NULL;
    count = lamp_kn_serial_read((int64_t)handle, buffer, capacity);
    result = (*env)->NewByteArray(env, count);
    if (result && count > 0) {
        (*env)->SetByteArrayRegion(env, result, 0, count,
                                   (const jbyte *)buffer);
    }
    free(buffer);
    return result;
}

JNIEXPORT jint JNICALL
Java_dev_lampvm_debugger_NativeBindings_serialWrite(
    JNIEnv *env, jobject self, jlong handle, jbyteArray data) {
    jbyte *bytes;
    jsize size;
    int32_t count;
    (void)self;
    if (!data) return 0;
    size = (*env)->GetArrayLength(env, data);
    bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes) return 0;
    count = lamp_kn_serial_write((int64_t)handle,
                                 (const uint8_t *)bytes, (int32_t)size);
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
    return (jint)count;
}
