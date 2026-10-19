#include "gles_angle.h"
#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <link.h>
#include <cstring>
#include <mutex>

namespace axrb::runtime {
namespace {
// EGL_EXT_device_query, EGL_ANGLE_device_vulkan and GL_EXT_semaphore values
// (ANGLE's eglext_angle.h; checked against the bundled ANGLE by gles_selftest).
constexpr EGLint kDevice = 0x322C;
constexpr EGLint kVulkanInstance = 0x34A9;
constexpr EGLint kVulkanPhysicalDevice = 0x34AB;
constexpr EGLint kVulkanDevice = 0x34AC;
constexpr EGLint kVulkanQueue = 0x34AF;
constexpr EGLint kVulkanQueueFamily = 0x34D0;
constexpr GLenum kLayoutGeneral = 0x958D;
constexpr GLenum kLayoutColorAttachment = 0x958E;
constexpr GLenum kLayoutShaderReadOnly = 0x9591;
constexpr GLenum kLayoutTransferSrc = 0x9592;
constexpr GLenum kLayoutTransferDst = 0x9593;

struct Library {
    ElfW(Addr) base = 0;
    const ElfW(Dyn)* dynamic = nullptr;
};

int find_library(dl_phdr_info* info, size_t, void* data) {
    const char* name = info->dlpi_name;
    if (!name) return 0;
    const size_t length = std::strlen(name);
    constexpr char kName[] = "libEGL_angle.so";
    if (length < sizeof(kName) - 1 || std::strcmp(name + length - (sizeof(kName) - 1), kName) != 0) return 0;
    auto& library = *static_cast<Library*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            library.dynamic = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    library.base = info->dlpi_addr;
    return 1;
}

// libEGL loaded ANGLE in the system namespace, out of this library's reach
// by dlopen; its exports are read from the loaded image instead.
void* find_symbol(const Library& library, const char* name) {
    const ElfW(Sym)* symbols = nullptr;
    const char* strings = nullptr;
    const uint32_t* gnu = nullptr;
    const uint32_t* sysv = nullptr;
    // Bionic leaves .dynamic unrelocated: its addresses are offsets from the base.
    for (const ElfW(Dyn)* d = library.dynamic; d && d->d_tag != DT_NULL; ++d) {
        const ElfW(Addr) at = d->d_un.d_ptr < library.base ? library.base + d->d_un.d_ptr : d->d_un.d_ptr;
        if (d->d_tag == DT_SYMTAB) symbols = reinterpret_cast<const ElfW(Sym)*>(at);
        else if (d->d_tag == DT_STRTAB) strings = reinterpret_cast<const char*>(at);
        else if (d->d_tag == DT_GNU_HASH) gnu = reinterpret_cast<const uint32_t*>(at);
        else if (d->d_tag == DT_HASH) sysv = reinterpret_cast<const uint32_t*>(at);
    }
    if (!symbols || !strings) return nullptr;
    uint32_t count = 0;
    if (sysv) {
        count = sysv[1];
    } else if (gnu) {
        // The symbol count is one past the last chain's end.
        const uint32_t buckets = gnu[0], offset = gnu[1], words = gnu[2];
        const uint32_t* bucket = gnu + 4 + words * (sizeof(ElfW(Addr)) / 4);
        const uint32_t* chain = bucket + buckets;
        for (uint32_t b = 0; b < buckets; ++b) count = bucket[b] >= count ? bucket[b] + 1 : count;
        if (count > offset) while (!(chain[count - 1 - offset] & 1)) ++count;
    }
    for (uint32_t i = 0; i < count; ++i)
        if (symbols[i].st_value && std::strcmp(strings + symbols[i].st_name, name) == 0)
            return reinterpret_cast<void*>(library.base + symbols[i].st_value);
    return nullptr;
}

VkImageLayout vulkan_layout(GLenum layout) {
    switch (layout) {
        case kLayoutGeneral: return VK_IMAGE_LAYOUT_GENERAL;
        case kLayoutColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case kLayoutShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case kLayoutTransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case kLayoutTransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

GLenum gl_layout(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_GENERAL: return kLayoutGeneral;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return kLayoutColorAttachment;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return kLayoutShaderReadOnly;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return kLayoutTransferSrc;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return kLayoutTransferDst;
        default: return GL_NONE;
    }
}

// The game's GL errors as ANGLE explains them (validation stays on: see
// forceGlErrorChecking in prepare_art_session.py). Each distinct message is
// logged a few times, so a call failing every frame cannot flood the log, and
// again at 100, 1000, ... occurrences to show whether it keeps failing.
void GL_APIENTRY log_gl_error(GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* message, const void*) {
    if (type != GL_DEBUG_TYPE_ERROR_KHR || !message) return;
    static std::mutex mutex;
    static char seen[64][96];
    static int counts[64], distinct = 0;
    std::lock_guard<std::mutex> lock(mutex);
    int slot = 0;
    while (slot < distinct && std::strncmp(seen[slot], message, sizeof seen[0] - 1) != 0) ++slot;
    if (slot == distinct) {
        if (distinct == 64) return;
        std::strncpy(seen[distinct++], message, sizeof seen[0] - 1);
    }
    const int count = ++counts[slot];
    if (count <= 3) __android_log_print(ANDROID_LOG_WARN, "AXRB.GLES", "GL error: %s", message);
    else if (count == 100 || count == 1000 || count == 10000 || count == 100000)
        __android_log_print(ANDROID_LOG_WARN, "AXRB.GLES", "GL error x%d: %s", count, message);
}

template <typename T>
bool resolve(void* (*proc)(const char*), const char* name, T& function) {
    function = reinterpret_cast<T>(proc(name));
    if (!function) __android_log_print(ANDROID_LOG_WARN, "AXRB.GLES", "ANGLE has no %s", name);
    return function != nullptr;
}
}  // namespace

bool GlesAngle::initialize(void* display, XrGraphicsBindingVulkanKHR& binding, VkQueue& queue, VulkanQueueLock& lock) {
    Library library;
    dl_iterate_phdr(find_library, &library);
    auto* proc = reinterpret_cast<void* (*)(const char*)>(find_symbol(library, "eglGetProcAddress"));
    if (!proc) {
        __android_log_print(ANDROID_LOG_INFO, "AXRB.GLES", "OpenGL ES driver is not ANGLE; frames use pixel readback");
        return false;
    }
    void* (*currentDisplay)() = nullptr;
    EGLBoolean (*queryDisplay)(void*, EGLint, EGLAttrib*) = nullptr;
    EGLBoolean (*queryDevice)(void*, EGLint, EGLAttrib*) = nullptr;
    void (*lockQueue)(void*) = nullptr;
    void (*unlockQueue)(void*) = nullptr;
    if (!resolve(proc, "eglGetCurrentDisplay", currentDisplay) || !resolve(proc, "eglGetCurrentContext", currentContext_) ||
        !resolve(proc, "eglQueryDisplayAttribEXT", queryDisplay) || !resolve(proc, "eglQueryDeviceAttribEXT", queryDevice) ||
        !resolve(proc, "eglLockVulkanQueueANGLE", lockQueue) || !resolve(proc, "eglUnlockVulkanQueueANGLE", unlockQueue) ||
        !resolve(proc, "eglCreateImageKHR", createImage_) || !resolve(proc, "eglDestroyImageKHR", destroyImage_) ||
        !resolve(proc, "eglExportVkImageANGLE", exportImage_) || !resolve(proc, "glReleaseTexturesANGLE", releaseTextures_) ||
        !resolve(proc, "glAcquireTexturesANGLE", acquireTextures_) || !resolve(proc, "glCopyImageSubData", copyImage_) ||
        !resolve(proc, "glGenTextures", genTextures_) || !resolve(proc, "glDeleteTextures", deleteTextures_) ||
        !resolve(proc, "glBindTexture", bindTexture_) || !resolve(proc, "glGetIntegerv", getIntegerv_) ||
        !resolve(proc, "glTexStorage2D", texStorage2D_) || !resolve(proc, "glGetError", getError_) ||
        !resolve(proc, "eglGetError", eglError_)) {
        *this = {};
        return false;
    }
    debugCallback_ = reinterpret_cast<decltype(debugCallback_)>(proc("glDebugMessageCallbackKHR"));
    enable_ = reinterpret_cast<decltype(enable_)>(proc("glEnable"));
    // libEGL hands applications ANGLE's own display; prefer the current one.
    void* angleDisplay = currentDisplay();
    if (!angleDisplay) angleDisplay = display;
    EGLAttrib device = 0;
    EGLAttrib values[5]{};
    const EGLint names[5] = {kVulkanInstance, kVulkanPhysicalDevice, kVulkanDevice, kVulkanQueue, kVulkanQueueFamily};
    bool queried = angleDisplay && queryDisplay(angleDisplay, kDevice, &device) && device;
    for (int i = 0; queried && i < 5; ++i) queried = queryDevice(reinterpret_cast<void*>(device), names[i], &values[i]);
    if (!queried || !values[0] || !values[1] || !values[2] || !values[3]) {
        __android_log_print(ANDROID_LOG_WARN, "AXRB.GLES", "ANGLE's Vulkan device is unavailable (display %p); frames use pixel readback", angleDisplay);
        *this = {};
        return false;
    }
    display_ = angleDisplay;
    device_ = reinterpret_cast<void*>(device);
    binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
    binding.instance = reinterpret_cast<VkInstance>(values[0]);
    binding.physicalDevice = reinterpret_cast<VkPhysicalDevice>(values[1]);
    binding.device = reinterpret_cast<VkDevice>(values[2]);
    binding.queueFamilyIndex = static_cast<uint32_t>(values[4]);
    queue = reinterpret_cast<VkQueue>(values[3]);
    lock = {lockQueue, unlockQueue, device_};
    __android_log_print(ANDROID_LOG_INFO, "AXRB.GLES", "OpenGL ES through ANGLE: Vulkan device %p queue %p (family %u)",
                        static_cast<void*>(binding.device), static_cast<void*>(queue), binding.queueFamilyIndex);
    return true;
}

void GlesAngle::watch_errors() {
    void* context = currentContext_();
    for (void* watched : watched_) if (watched == context) return;
    for (void*& slot : watched_) {
        if (slot) continue;
        slot = context;
        if (debugCallback_ && enable_) {
            enable_(GL_DEBUG_OUTPUT_KHR);
            debugCallback_(log_gl_error, nullptr);
        }
        return;
    }
}

bool GlesAngle::current_context() const {
    if (currentContext_ && currentContext_()) return true;
    static bool reported = false;
    if (!reported) __android_log_print(ANDROID_LOG_WARN, "AXRB.GLES", "no ANGLE context is current on this thread; swapchain image not handed over");
    reported = true;
    return false;
}

bool GlesAngle::create_transfer(uint32_t width, uint32_t height, uint32_t layers, uint32_t internalFormat,
                                GlesTransfer& transfer) {
    transfer = {};
    if (!active() || !current_context()) return false;
    watch_errors();
    GLint maxSize = 0;
    getIntegerv_(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (static_cast<uint64_t>(height) * layers > static_cast<uint64_t>(maxSize)) {
        __android_log_print(ANDROID_LOG_ERROR, "AXRB.GLES", "%u layers of %u rows exceed the %d-row texture limit", layers, height, maxSize);
        return false;
    }
    GLint previous = 0;
    getIntegerv_(GL_TEXTURE_BINDING_2D, &previous);
    genTextures_(1, &transfer.texture);
    bindTexture_(GL_TEXTURE_2D, transfer.texture);
    texStorage2D_(GL_TEXTURE_2D, 1, internalFormat, static_cast<GLsizei>(width), static_cast<GLsizei>(height * layers));
    bindTexture_(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    const EGLint attributes[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
    transfer.eglImage = createImage_(display_, currentContext_(), EGL_GL_TEXTURE_2D_KHR,
                                     reinterpret_cast<void*>(static_cast<uintptr_t>(transfer.texture)), attributes);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (!transfer.eglImage || !exportImage_(display_, transfer.eglImage, &transfer.image, &info) || !transfer.image) {
        __android_log_print(ANDROID_LOG_ERROR, "AXRB.GLES", "exporting a %ux%u texture as a Vulkan image failed (egl 0x%x gl 0x%x)",
                            width, height * layers, eglError_(), getError_());
        destroy_transfer(transfer, VK_IMAGE_LAYOUT_UNDEFINED);
        return false;
    }
    transfer.format = info.format;
    return true;
}

void GlesAngle::destroy_transfer(GlesTransfer& transfer, VkImageLayout layout) {
    if (active() && transfer.texture && current_context()) {
        // Back to GL before deleting, so ANGLE does not keep it as externally held.
        const GLenum glLayout = gl_layout(layout);
        if (transfer.released) acquireTextures_(1, &transfer.texture, &glLayout);
        if (transfer.eglImage) destroyImage_(display_, transfer.eglImage);
        deleteTextures_(1, &transfer.texture);
    }
    transfer = {};
}

bool GlesAngle::hand_over(uint32_t texture, uint32_t target, uint32_t width, uint32_t height, uint32_t layers,
                          GlesTransfer& transfer, VkImageLayout& layout) {
    if (!active() || !transfer.texture || !current_context()) return false;
    if (transfer.released) {
        const GLenum glLayout = gl_layout(layout);
        acquireTextures_(1, &transfer.texture, &glLayout);
    }
    for (uint32_t layer = 0; layer < layers; ++layer)
        copyImage_(texture, target, 0, 0, 0, target == GL_TEXTURE_2D ? 0 : static_cast<GLint>(layer),
                   transfer.texture, GL_TEXTURE_2D, 0, 0, static_cast<GLint>(height * layer), 0,
                   static_cast<GLsizei>(width), static_cast<GLsizei>(height), 1);
    // ANGLE submits the copy before returning; the backend's commands follow
    // it on the same queue.
    GLenum released = GL_NONE;
    releaseTextures_(1, &transfer.texture, &released);
    transfer.released = true;
    layout = vulkan_layout(released);
    static bool reported = false;
    if (!reported) {
        __android_log_print(ANDROID_LOG_INFO, "AXRB.GLES", "first GL frame handed to Vulkan: %ux%u layers=%u layout 0x%x gl 0x%x",
                            width, height, layers, released, getError_());
        reported = true;
    }
    return true;
}
}
#endif
