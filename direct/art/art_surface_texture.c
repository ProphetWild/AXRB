// android.graphics.SurfaceTexture fed by the host media decoder.
//
// In Direct mode nothing ever queues a buffer into a SurfaceTexture: there is
// no gralloc, and MediaPlayer decodes on the host into per-surface RGBA frame
// slots (direct/src/media.cpp). A game that shows a movie through a GL
// external texture (Unreal's movie player: MediaPlayer.setSurface(new
// Surface(new SurfaceTexture(texture))), then updateTexImage each frame) saw
// no picture. Here a SurfaceTexture whose Surface is a MediaPlayer's output
// gets the host's newest frame on updateTexImage: it is uploaded into a 2D
// texture whose EGLImage backs the application's GL_TEXTURE_EXTERNAL_OES
// texture, with the transform a gralloc buffer would have (rows top-down).
//
// The framework's natives keep running for everything else; they are chained
// through the JNI pointer ART holds for each registered native method. If that
// pointer cannot be confirmed, nothing is replaced.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "AXRB.Media", __VA_ARGS__)

extern long axrb_media(int op, long id, long a, long b, long c);
extern long axrb_media_frame(long surface, long address, int width, int height, long previous);

// Host frame slots for SurfaceTextures, apart from HostSurface's counter.
#define MEDIA_HANDLE_BASE (1L << 40)
#define MAX_TEXTURES 16

struct Texture {
    jweak object;          // the SurfaceTexture
    jlong surface;         // android.view.Surface native object created from it
    GLuint name;           // the application's external texture
    jlong media, player;   // host frame slot and player, once a MediaPlayer outputs here
    jobject weak;          // the WeakReference SurfaceTexture posts frame events with
    GLuint upload;         // 2D texture behind the external texture's EGLImage
    EGLImageKHR image;
    int uploadWidth, uploadHeight;
    int width, height;     // newest frame in pixels, waiting for updateTexImage if pending
    uint8_t* pixels;
    int pending;
    long serial;
    jlong timestamp;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct Texture textures[MAX_TEXTURES];
static jfieldID surface_native;
static JavaVM* vm;
static jclass texture_class;
static jmethodID post_event;
static pthread_once_t poller_once = PTHREAD_ONCE_INIT;

static void (*original_init)(JNIEnv*, jobject, jboolean, jint, jboolean, jobject);
static jlong (*original_create)(JNIEnv*, jclass, jobject);
static void (*original_update)(JNIEnv*, jobject);
static void (*original_transform)(JNIEnv*, jobject, jfloatArray);
static jlong (*original_timestamp)(JNIEnv*, jobject);
static void (*original_finalize)(JNIEnv*, jobject);
static void (*original_release)(JNIEnv*, jobject);

static struct Texture* find(JNIEnv* e, jobject texture) {
    for (int i = 0; i < MAX_TEXTURES; ++i)
        if (textures[i].object && (*e)->IsSameObject(e, textures[i].object, texture)) return &textures[i];
    return NULL;
}

static void texture_init(JNIEnv* e, jobject thiz, jboolean detached, jint name, jboolean single, jobject weak) {
    original_init(e, thiz, detached, name, single, weak);
    if ((*e)->ExceptionCheck(e)) return;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_TEXTURES; ++i) {
        // Reuse slots whose SurfaceTexture was collected without release().
        if (textures[i].object && !(*e)->IsSameObject(e, textures[i].object, NULL)) continue;
        if (textures[i].object) (*e)->DeleteWeakGlobalRef(e, textures[i].object);
        if (textures[i].weak) (*e)->DeleteGlobalRef(e, textures[i].weak);
        free(textures[i].pixels);
        memset(&textures[i], 0, sizeof textures[i]);
        textures[i].object = (*e)->NewWeakGlobalRef(e, thiz);
        textures[i].weak = weak ? (*e)->NewGlobalRef(e, weak) : NULL;
        textures[i].name = detached ? 0 : (GLuint)name;
        break;
    }
    pthread_mutex_unlock(&lock);
}

static jlong surface_create(JNIEnv* e, jclass cls, jobject texture) {
    const jlong surface = original_create(e, cls, texture);
    if (!surface || !texture) return surface;
    pthread_mutex_lock(&lock);
    struct Texture* t = find(e, texture);
    if (t) t->surface = surface;
    pthread_mutex_unlock(&lock);
    return surface;
}

// A frame arrives as a buffer would: the newest host frame is copied while
// unlocked into a buffer of the poller's, swapped in, and the SurfaceTexture
// posts onFrameAvailable (Unreal updates the texture only after that event).
static void* poll_frames(void* unused) {
    (void)unused;
    JNIEnv* e = NULL;
    if (!vm || (*vm)->AttachCurrentThread(vm, &e, NULL) != JNI_OK) return NULL;
    uint8_t* back[MAX_TEXTURES] = {0};
    size_t capacity[MAX_TEXTURES] = {0};
    for (;;) {
        jobject notify[MAX_TEXTURES];
        int count = 0;
        for (int i = 0; i < MAX_TEXTURES; ++i) {
            pthread_mutex_lock(&lock);
            const jlong media = textures[i].media, player = textures[i].player;
            const long seen = textures[i].serial;
            pthread_mutex_unlock(&lock);
            if (!media) continue;
            const int width = (int)axrb_media(13, player, 0, 0, 0), height = (int)axrb_media(14, player, 0, 0, 0);
            if (width <= 0 || height <= 0 || width > 8192 || height > 8192) continue;
            const size_t bytes = (size_t)width * height * 4;
            if (capacity[i] < bytes) {
                uint8_t* grown = realloc(back[i], bytes);
                if (!grown) continue;
                back[i] = grown;
                capacity[i] = bytes;
            }
            const long serial = axrb_media_frame(media, (long)(uintptr_t)back[i], width, height, seen);
            if (!serial || serial == seen) continue;
            pthread_mutex_lock(&lock);
            struct Texture* t = &textures[i];
            if (t->media == media && t->weak) {
                uint8_t* front = t->pixels;
                t->pixels = back[i];
                back[i] = front;
                const size_t frontCapacity = (size_t)t->width * t->height * 4;
                capacity[i] = front ? frontCapacity : 0;
                t->width = width;
                t->height = height;
                t->serial = serial;
                t->pending = 1;
                notify[count++] = (*e)->NewLocalRef(e, t->weak);
            }
            pthread_mutex_unlock(&lock);
        }
        for (int i = 0; i < count; ++i) {
            (*e)->CallStaticVoidMethod(e, texture_class, post_event, notify[i]);
            if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
            (*e)->DeleteLocalRef(e, notify[i]);
        }
        struct timespec pause = {0, 4000000};
        nanosleep(&pause, NULL);
    }
    return NULL;
}
static void start_poller(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, poll_frames, NULL) == 0) pthread_detach(thread);
}

// Called by HostMedia.surface for a MediaPlayer output that is not a host
// surface: the frame slot the player should decode into, or 0.
JNIEXPORT jlong JNICALL Java_com_axrb_runtime_HostMedia_attachTexture(JNIEnv* e, jclass cls, jobject surface,
                                                                      jlong player) {
    (void)cls;
    if (!surface || !surface_native) return 0;
    const jlong native = (*e)->GetLongField(e, surface, surface_native);
    jlong media = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_TEXTURES && native; ++i) {
        if (!textures[i].object || textures[i].surface != native) continue;
        textures[i].media = MEDIA_HANDLE_BASE + i + 1;
        textures[i].player = player;
        textures[i].serial = 0;
        media = textures[i].media;
        break;
    }
    pthread_mutex_unlock(&lock);
    if (media) {
        if (!vm) (*e)->GetJavaVM(e, &vm);
        pthread_once(&poller_once, start_poller);
        LOG("MediaPlayer frames go to a SurfaceTexture (GL external texture)");
    }
    return media;
}

static int upload(struct Texture* t) {
    if (!t->pending || !t->pixels || !t->name) return 0;
    t->pending = 0;
    const int width = t->width, height = t->height;
    if (width != t->uploadWidth || height != t->uploadHeight) {
        static PFNEGLDESTROYIMAGEKHRPROC destroy;
        if (!destroy) destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        if (t->image && destroy) destroy(eglGetCurrentDisplay(), t->image);
        if (t->upload) glDeleteTextures(1, &t->upload);
        t->image = EGL_NO_IMAGE_KHR;
        t->upload = 0;
        t->uploadWidth = width;
        t->uploadHeight = height;
    }
    GLint previous2D = 0, previousExternal = 0, unpackBuffer = 0, alignment = 4, rowLength = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous2D);
    glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &previousExternal);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    if (!t->upload) {
        glGenTextures(1, &t->upload);
        glBindTexture(GL_TEXTURE_2D, t->upload);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        static PFNEGLCREATEIMAGEKHRPROC create;
        static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target;
        if (!create) create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        if (!target) target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        const EGLint attributes[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
        t->image = create ? create(eglGetCurrentDisplay(), eglGetCurrentContext(), EGL_GL_TEXTURE_2D_KHR,
                                   (EGLClientBuffer)(uintptr_t)t->upload, attributes) : EGL_NO_IMAGE_KHR;
        if (t->image != EGL_NO_IMAGE_KHR && target) {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, t->name);
            target(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)t->image);
        }
        LOG("SurfaceTexture %u shows %dx%d host frames (image %p, gl 0x%x)", t->name, width, height, t->image,
            glGetError());
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, t->upload);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, t->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)unpackBuffer);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous2D);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)previousExternal);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    t->timestamp = (jlong)now.tv_sec * 1000000000 + now.tv_nsec;
    return 1;
}

static void texture_update(JNIEnv* e, jobject thiz) {
    pthread_mutex_lock(&lock);
    struct Texture* t = find(e, thiz);
    const int fed = t && t->media;
    if (fed) upload(t);
    pthread_mutex_unlock(&lock);
    if (!fed) original_update(e, thiz);
}

static void texture_transform(JNIEnv* e, jobject thiz, jfloatArray matrix) {
    pthread_mutex_lock(&lock);
    struct Texture* t = find(e, thiz);
    const int fed = t && t->media && t->serial;
    pthread_mutex_unlock(&lock);
    if (!fed) { original_transform(e, thiz, matrix); return; }
    // A top-down buffer in GL's bottom-up texture space, as GLConsumer reports it.
    const jfloat flip[16] = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1};
    (*e)->SetFloatArrayRegion(e, matrix, 0, 16, flip);
}

static jlong texture_timestamp(JNIEnv* e, jobject thiz) {
    pthread_mutex_lock(&lock);
    struct Texture* t = find(e, thiz);
    const jlong timestamp = t && t->media && t->serial ? t->timestamp : -1;
    pthread_mutex_unlock(&lock);
    return timestamp >= 0 ? timestamp : original_timestamp(e, thiz);
}

static void forget(JNIEnv* e, jobject thiz) {
    pthread_mutex_lock(&lock);
    struct Texture* t = find(e, thiz);
    if (t) {
        // GL objects belong to a context that may not be current here; the
        // 2D texture and EGLImage are left to the context's destruction.
        if (t->media) axrb_media(16, t->media, 0, 0, 0);
        (*e)->DeleteWeakGlobalRef(e, t->object);
        if (t->weak) (*e)->DeleteGlobalRef(e, t->weak);
        free(t->pixels);
        memset(t, 0, sizeof *t);
    }
    pthread_mutex_unlock(&lock);
}
static void texture_finalize(JNIEnv* e, jobject thiz) { forget(e, thiz); original_finalize(e, thiz); }
static void texture_release(JNIEnv* e, jobject thiz) { forget(e, thiz); original_release(e, thiz); }

// The JNI function ART holds for a registered native: ArtMethod::data_, after
// the 16 bytes of class, flags, dex index and method index. Accepted only if
// it points into a loaded framework library.
static void* registered(JNIEnv* e, jclass cls, const char* name, const char* signature, int isStatic) {
    jmethodID method = isStatic ? (*e)->GetStaticMethodID(e, cls, name, signature) : (*e)->GetMethodID(e, cls, name, signature);
    if (!method || ((uintptr_t)method & 3)) { (*e)->ExceptionClear(e); return NULL; }
    void* function = *(void**)((uint8_t*)method + 16);
    Dl_info info;
    if (!function || !dladdr(function, &info) || !info.dli_fname ||
        (!strstr(info.dli_fname, "libandroid_runtime") && !strstr(info.dli_fname, "libhwui"))) return NULL;
    return function;
}

int register_art_surface_texture(JNIEnv* e) {
    jclass textureClass = (*e)->FindClass(e, "android/graphics/SurfaceTexture");
    jclass surfaceClass = (*e)->FindClass(e, "android/view/Surface");
    if (!textureClass || !surfaceClass) { (*e)->ExceptionClear(e); return 0; }
    surface_native = (*e)->GetFieldID(e, surfaceClass, "mNativeObject", "J");
    texture_class = (*e)->NewGlobalRef(e, textureClass);
    post_event = (*e)->GetStaticMethodID(e, textureClass, "postEventFromNative", "(Ljava/lang/ref/WeakReference;)V");
    original_init = registered(e, textureClass, "nativeInit", "(ZIZLjava/lang/ref/WeakReference;)V", 0);
    original_update = registered(e, textureClass, "nativeUpdateTexImage", "()V", 0);
    original_transform = registered(e, textureClass, "nativeGetTransformMatrix", "([F)V", 0);
    original_timestamp = registered(e, textureClass, "nativeGetTimestamp", "()J", 0);
    original_finalize = registered(e, textureClass, "nativeFinalize", "()V", 0);
    original_release = registered(e, textureClass, "nativeRelease", "()V", 0);
    original_create = registered(e, surfaceClass, "nativeCreateFromSurfaceTexture", "(Landroid/graphics/SurfaceTexture;)J", 1);
    if (!surface_native || !post_event || !original_init || !original_update || !original_transform || !original_timestamp ||
        !original_finalize || !original_release || !original_create) {
        (*e)->ExceptionClear(e);
        surface_native = NULL;
        LOG("SurfaceTexture natives not found; movies on GL textures have no picture");
        return 0;
    }
    const JNINativeMethod textureMethods[] = {
        {"nativeInit", "(ZIZLjava/lang/ref/WeakReference;)V", (void*)texture_init},
        {"nativeUpdateTexImage", "()V", (void*)texture_update},
        {"nativeGetTransformMatrix", "([F)V", (void*)texture_transform},
        {"nativeGetTimestamp", "()J", (void*)texture_timestamp},
        {"nativeFinalize", "()V", (void*)texture_finalize},
        {"nativeRelease", "()V", (void*)texture_release},
    };
    const JNINativeMethod surfaceMethods[] = {
        {"nativeCreateFromSurfaceTexture", "(Landroid/graphics/SurfaceTexture;)J", (void*)surface_create},
    };
    if ((*e)->RegisterNatives(e, textureClass, textureMethods, sizeof textureMethods / sizeof textureMethods[0]) != 0 ||
        (*e)->RegisterNatives(e, surfaceClass, surfaceMethods, 1) != 0) {
        (*e)->ExceptionClear(e);
        return -1;
    }
    return 0;
}
