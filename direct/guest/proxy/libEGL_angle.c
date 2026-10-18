// libEGL_angle.so for AXRB Direct: ANGLE's own EGL library, with one GL
// function added. The AOSP image's ANGLE is mounted as libEGL_angle_real.so,
// a dependency of this library, so libEGL's dlsym on this library finds all of
// ANGLE's EGL functions; only eglGetProcAddress is this library's.
//
// The addition is glFramebufferTextureMultisampleMultiviewOVR
// (GL_OVR_multiview_multisampled_render_to_texture), which ANGLE does not
// implement. Unreal attaches its multiview eye buffers with it when the
// project uses MSAA, as Quest games do; libEGL answers with a stub that does
// nothing, and the game draws into a framebuffer with no attachments. Here it
// attaches the views without multisampling (glFramebufferTextureMultiviewOVR).
#ifndef AXRB_LINK_STUB
#include <dlfcn.h>
#include <string.h>

typedef void (*Function)(void);
typedef Function (*GetProcAddress)(const char*);
typedef void (*Multiview)(unsigned target, unsigned attachment, unsigned texture, int level, int baseView, int views);

static GetProcAddress angle_get_proc(void) {
    static GetProcAddress proc;
    if (!proc) {
        void* angle = dlopen("libEGL_angle_real.so", RTLD_NOW | RTLD_NOLOAD);
        if (angle) proc = (GetProcAddress)dlsym(angle, "eglGetProcAddress");
        if (!proc) proc = (GetProcAddress)dlsym(RTLD_NEXT, "eglGetProcAddress");
    }
    return proc;
}

static void multisample_multiview(unsigned target, unsigned attachment, unsigned texture, int level, int samples,
                                  int baseView, int views) {
    (void)samples;
    static Multiview multiview;
    if (!multiview && angle_get_proc()) multiview = (Multiview)angle_get_proc()("glFramebufferTextureMultiviewOVR");
    if (multiview) multiview(target, attachment, texture, level, baseView, views);
}

__attribute__((visibility("default"))) Function eglGetProcAddress(const char* name) {
    if (name && strcmp(name, "glFramebufferTextureMultisampleMultiviewOVR") == 0) return (Function)multisample_multiview;
    return angle_get_proc() ? angle_get_proc()(name) : 0;
}
#endif
