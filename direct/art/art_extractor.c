// android.media.MediaExtractor for Direct mode. AOSP's needs the
// media.extractor service, which Direct mode does not run: every
// setDataSource waits five seconds and then throws. These natives use the
// host's Media Foundation extractor instead, the one behind AMediaExtractor
// (direct/art/ndk_media.cpp, direct/src/media_codec.cpp). mNativeContext holds
// the host extractor's id.
//
// Unreal's movie player (MediaPlayer14) opens a movie with MediaPlayer and
// then reads its tracks with MediaExtractor; without these, every movie fails
// to open and a game that waits for one (Star Wars: Tales from the Galaxy's
// Edge) stays on its loading screen.
#include "../guest/axrb_guest.h"
#include <jni.h>
#include <stdint.h>
#include <string.h>

extern long axrb_mediandk(long op, long id, long a, long b, long c, long d);
AXRB_HOST_FUNCTION(axrb_mediandk, "art.mediandk");

// Shared with direct/src/media_codec.cpp.
enum {
    kExtractorNew = 0, kExtractorSourceFd, kExtractorTrackCount, kExtractorTrack, kExtractorCsd,
    kExtractorSelect, kExtractorSeek, kExtractorSample, kExtractorRead, kExtractorAdvance,
    kExtractorDelete, kExtractorFileFormat,
};
struct TrackInfo {
    char mime[64];
    int64_t duration_us;
    int32_t width, height, sample_rate, channels, max_input_size, frame_rate, bit_rate, is_adts;
    int32_t csd_count, reserved;
};
struct SampleInfo {
    int64_t time_us;
    int32_t track, flags, size, reserved;
};

static jfieldID context;

static long host(long op, long id, long a, long b, long c) { return axrb_mediandk(op, id, a, b, c, 0); }
static jlong id_of(JNIEnv* e, jobject x) { return (*e)->GetLongField(e, x, context); }
static void raise(JNIEnv* e, const char* type, const char* message) {
    jclass error = (*e)->FindClass(e, type);
    if (error) (*e)->ThrowNew(e, error, message);
}
static jlong live(JNIEnv* e, jobject x) {
    const jlong id = id_of(e, x);
    if (!id) raise(e, "java/lang/IllegalStateException", "MediaExtractor was released");
    return id;
}

static void extractor_setup(JNIEnv* e, jobject x) {
    const long id = host(kExtractorNew, 0, 0, 0, 0);
    if (id <= 0) { raise(e, "java/lang/RuntimeException", "Media extractor unavailable"); return; }
    (*e)->SetLongField(e, x, context, id);
}
static void extractor_release(JNIEnv* e, jobject x) {
    const jlong id = id_of(e, x);
    if (!id) return;
    (*e)->SetLongField(e, x, context, 0);
    host(kExtractorDelete, id, 0, 0, 0);
}
static void extractor_source_fd(JNIEnv* e, jobject x, jobject fd, jlong offset, jlong length) {
    const jlong id = live(e, x);
    if (!id) return;
    if (!fd) { raise(e, "java/lang/IllegalArgumentException", "null FileDescriptor"); return; }
    jclass cls = (*e)->GetObjectClass(e, fd);
    jfieldID descriptor = (*e)->GetFieldID(e, cls, "descriptor", "I");
    if (!descriptor) return;
    if (host(kExtractorSourceFd, id, (*e)->GetIntField(e, fd, descriptor), offset, length) < 0)
        raise(e, "java/io/IOException", "Failed to instantiate extractor.");
}
static void extractor_unsupported_source(JNIEnv* e, jobject x, ...) {
    (void)x;
    raise(e, "java/io/IOException", "Only file descriptors are supported by the host media extractor");
}
static jint extractor_track_count(JNIEnv* e, jobject x) {
    const jlong id = live(e, x);
    const long count = id ? host(kExtractorTrackCount, id, 0, 0, 0) : 0;
    return count > 0 ? (jint)count : 0;
}

static void put(JNIEnv* e, jobject map, jmethodID method, const char* key, jobject value) {
    jstring name = (*e)->NewStringUTF(e, key);
    jobject previous = (*e)->CallObjectMethod(e, map, method, name, value);
    if (previous) (*e)->DeleteLocalRef(e, previous);
    (*e)->DeleteLocalRef(e, name);
    if (value) (*e)->DeleteLocalRef(e, value);
}
static jobject boxed_int(JNIEnv* e, jint value) {
    jclass cls = (*e)->FindClass(e, "java/lang/Integer");
    return (*e)->CallStaticObjectMethod(e, cls, (*e)->GetStaticMethodID(e, cls, "valueOf", "(I)Ljava/lang/Integer;"), value);
}
static jobject boxed_long(JNIEnv* e, jlong value) {
    jclass cls = (*e)->FindClass(e, "java/lang/Long");
    return (*e)->CallStaticObjectMethod(e, cls, (*e)->GetStaticMethodID(e, cls, "valueOf", "(J)Ljava/lang/Long;"), value);
}
static jobject new_map(JNIEnv* e, jmethodID* putMethod) {
    jclass cls = (*e)->FindClass(e, "java/util/HashMap");
    *putMethod = (*e)->GetMethodID(e, cls, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    return (*e)->NewObject(e, cls, (*e)->GetMethodID(e, cls, "<init>", "()V"));
}

// The same keys AMediaExtractor_getTrackFormat sets.
static jobject extractor_track_format(JNIEnv* e, jobject x, jint index) {
    const jlong id = live(e, x);
    if (!id) return NULL;
    struct TrackInfo info;
    memset(&info, 0, sizeof info);
    if (index < 0 || host(kExtractorTrack, id, index, (long)(uintptr_t)&info, 0) < 0) {
        raise(e, "java/lang/IllegalArgumentException", "Invalid track index");
        return NULL;
    }
    info.mime[sizeof info.mime - 1] = 0;
    jmethodID p;
    jobject map = new_map(e, &p);
    put(e, map, p, "mime", (*e)->NewStringUTF(e, info.mime));
    if (info.duration_us > 0) put(e, map, p, "durationUs", boxed_long(e, info.duration_us));
    if (info.width > 0) put(e, map, p, "width", boxed_int(e, info.width));
    if (info.height > 0) put(e, map, p, "height", boxed_int(e, info.height));
    if (info.frame_rate > 0) put(e, map, p, "frame-rate", boxed_int(e, info.frame_rate));
    if (info.sample_rate > 0) put(e, map, p, "sample-rate", boxed_int(e, info.sample_rate));
    if (info.channels > 0) put(e, map, p, "channel-count", boxed_int(e, info.channels));
    if (info.max_input_size > 0) put(e, map, p, "max-input-size", boxed_int(e, info.max_input_size));
    if (info.bit_rate > 0) put(e, map, p, "bitrate", boxed_int(e, info.bit_rate));
    if (info.is_adts) put(e, map, p, "is-adts", boxed_int(e, 1));
    jclass buffers = (*e)->FindClass(e, "java/nio/ByteBuffer");
    jmethodID allocate = (*e)->GetStaticMethodID(e, buffers, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
    for (int i = 0; i < info.csd_count && i < 2; ++i) {
        const long size = host(kExtractorCsd, id, index, i, 0);
        if (size <= 0) continue;
        jobject buffer = (*e)->CallStaticObjectMethod(e, buffers, allocate, (jint)size);
        void* bytes = buffer ? (*e)->GetDirectBufferAddress(e, buffer) : NULL;
        if (!bytes) continue;
        axrb_mediandk(kExtractorCsd, id, index, i, (long)(uintptr_t)bytes, size);
        const char key[] = {'c', 's', 'd', '-', (char)('0' + i), 0};
        put(e, map, p, key, buffer);
    }
    return map;
}
static jobject extractor_file_format(JNIEnv* e, jobject x) {
    const jlong id = live(e, x);
    if (!id) return NULL;
    char mime[64];
    memset(mime, 0, sizeof mime);
    jmethodID p;
    jobject map = new_map(e, &p);
    if (host(kExtractorFileFormat, id, (long)(uintptr_t)mime, sizeof mime, 0) >= 0 && mime[0]) {
        mime[sizeof mime - 1] = 0;
        put(e, map, p, "mime", (*e)->NewStringUTF(e, mime));
    }
    return map;
}
static void extractor_select(JNIEnv* e, jobject x, jint index) {
    const jlong id = live(e, x);
    if (id && host(kExtractorSelect, id, index, 1, 0) < 0) raise(e, "java/lang/IllegalArgumentException", "Invalid track index");
}
static void extractor_unselect(JNIEnv* e, jobject x, jint index) {
    const jlong id = live(e, x);
    if (id) host(kExtractorSelect, id, index, 0, 0);
}
static void extractor_seek(JNIEnv* e, jobject x, jlong time_us, jint mode) {
    (void)mode;
    const jlong id = live(e, x);
    if (id) host(kExtractorSeek, id, time_us, 0, 0);
}
static jboolean extractor_advance(JNIEnv* e, jobject x) {
    const jlong id = live(e, x);
    return id && host(kExtractorAdvance, id, 0, 0, 0) > 0;
}
static int sample(JNIEnv* e, jobject x, struct SampleInfo* info) {
    const jlong id = live(e, x);
    memset(info, 0, sizeof *info);
    return id && host(kExtractorSample, id, (long)(uintptr_t)info, 0, 0) == 0;
}
static jint extractor_sample_track(JNIEnv* e, jobject x) {
    struct SampleInfo info;
    return sample(e, x, &info) ? info.track : -1;
}
static jlong extractor_sample_time(JNIEnv* e, jobject x) {
    struct SampleInfo info;
    return sample(e, x, &info) ? info.time_us : -1;
}
static jlong extractor_sample_size(JNIEnv* e, jobject x) {
    struct SampleInfo info;
    return sample(e, x, &info) ? info.size : -1;
}
static jint extractor_sample_flags(JNIEnv* e, jobject x) {
    struct SampleInfo info;
    return sample(e, x, &info) ? info.flags : -1;
}
// Direct buffers only; the bytes land at the buffer's address plus offset.
static jint extractor_read(JNIEnv* e, jobject x, jobject buffer, jint offset) {
    const jlong id = live(e, x);
    if (!id || !buffer) return -1;
    uint8_t* bytes = (*e)->GetDirectBufferAddress(e, buffer);
    const jlong capacity = (*e)->GetDirectBufferCapacity(e, buffer);
    if (!bytes || offset < 0 || offset > capacity) return -1;
    const long n = host(kExtractorRead, id, (long)(uintptr_t)(bytes + offset), (long)(capacity - offset), 0);
    return n < 0 ? -1 : (jint)n;
}
static jlong extractor_cached_duration(JNIEnv* e, jobject x) { (void)e; (void)x; return -1; }
static jboolean extractor_cache_end(JNIEnv* e, jobject x) { (void)e; (void)x; return JNI_TRUE; }
static jboolean extractor_crypto(JNIEnv* e, jobject x, jobject info) { (void)e; (void)x; (void)info; return JNI_FALSE; }

int register_art_extractor(JNIEnv* e) {
    jclass cls = (*e)->FindClass(e, "android/media/MediaExtractor");
    if (!cls) return -1;
    // Let libmedia_jni register its natives first (native_init in <clinit>).
    if (!(*e)->GetStaticMethodID(e, cls, "native_init", "()V")) return -1;
    context = (*e)->GetFieldID(e, cls, "mNativeContext", "J");
    if (!context) return -1;
    const JNINativeMethod methods[] = {
        {"native_setup", "()V", (void*)extractor_setup},
        {"native_finalize", "()V", (void*)extractor_release},
        {"release", "()V", (void*)extractor_release},
        {"setDataSource", "(Ljava/io/FileDescriptor;JJ)V", (void*)extractor_source_fd},
        {"setDataSource", "(Landroid/media/MediaDataSource;)V", (void*)extractor_unsupported_source},
        {"nativeSetDataSource", "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
         (void*)extractor_unsupported_source},
        {"getTrackCount", "()I", (void*)extractor_track_count},
        {"getTrackFormatNative", "(I)Ljava/util/Map;", (void*)extractor_track_format},
        {"getFileFormatNative", "()Ljava/util/Map;", (void*)extractor_file_format},
        {"selectTrack", "(I)V", (void*)extractor_select},
        {"unselectTrack", "(I)V", (void*)extractor_unselect},
        {"seekTo", "(JI)V", (void*)extractor_seek},
        {"advance", "()Z", (void*)extractor_advance},
        {"readSampleData", "(Ljava/nio/ByteBuffer;I)I", (void*)extractor_read},
        {"getSampleTrackIndex", "()I", (void*)extractor_sample_track},
        {"getSampleTime", "()J", (void*)extractor_sample_time},
        {"getSampleSize", "()J", (void*)extractor_sample_size},
        {"getSampleFlags", "()I", (void*)extractor_sample_flags},
        {"getSampleCryptoInfo", "(Landroid/media/MediaCodec$CryptoInfo;)Z", (void*)extractor_crypto},
        {"getCachedDuration", "()J", (void*)extractor_cached_duration},
        {"hasCacheReachedEndOfStream", "()Z", (void*)extractor_cache_end},
    };
    return (*e)->RegisterNatives(e, cls, methods, sizeof methods / sizeof methods[0]);
}
