// libGLESv2_angle.so for AXRB Direct: ANGLE's own GL library (mounted as
// libGLESv2_angle_real.so, a dependency, so dlsym on this library finds all
// of its functions), with the extension list completed.
//
// libEGL_angle.so adds glFramebufferTextureMultisampleMultiviewOVR, which
// ANGLE lacks. Unreal attaches its multiview eye buffers with it only when
// the context also names GL_OVR_multiview_multisampled_render_to_texture;
// otherwise its MSAA eye framebuffer is left without attachments and every
// frame stays black. This library adds the name wherever ANGLE offers
// GL_OVR_multiview2.
#ifndef AXRB_LINK_STUB
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned char GLubyte;
#define GL_EXTENSIONS 0x1F03

static const char kAdded[] = "GL_OVR_multiview_multisampled_render_to_texture";

static void* angle_function(const char* name) {
    static void* angle;
    if (!angle) angle = dlopen("libGLESv2_angle_real.so", RTLD_NOW | RTLD_NOLOAD);
    void* function = angle ? dlsym(angle, name) : NULL;
    return function ? function : dlsym(RTLD_NEXT, name);
}

// ANGLE's string differs per context version; each is extended once and kept.
static const GLubyte* extended(const GLubyte* original) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static struct { const GLubyte* original; char* extended; } cache[16];
    if (!original || !strstr((const char*)original, "GL_OVR_multiview2") || strstr((const char*)original, kAdded))
        return original;
    const GLubyte* result = original;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 16; ++i) {
        // Same address and text (a destroyed context's string can be reused).
        if (cache[i].original == original) {
            const size_t length = strlen((const char*)original);
            if (strncmp(cache[i].extended, (const char*)original, length) == 0 && cache[i].extended[length] == ' ') {
                result = (const GLubyte*)cache[i].extended;
                break;
            }
            free(cache[i].extended);
            cache[i].original = NULL;
        }
        if (!cache[i].original) {
            const size_t length = strlen((const char*)original);
            char* text = malloc(length + sizeof kAdded + 1);
            if (!text) break;
            memcpy(text, original, length);
            text[length] = ' ';
            memcpy(text + length + 1, kAdded, sizeof kAdded);
            cache[i].original = original;
            cache[i].extended = text;
            result = (const GLubyte*)text;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

__attribute__((visibility("default"))) const GLubyte* glGetString(GLenum name) {
    static const GLubyte* (*angle)(GLenum);
    if (!angle) angle = (const GLubyte* (*)(GLenum))angle_function("glGetString");
    const GLubyte* value = angle ? angle(name) : NULL;
    return name == GL_EXTENSIONS ? extended(value) : value;
}
#endif
