#pragma once
#if defined(__ANDROID__)
#include "vulkan_backend.h"
#include <cstdint>

namespace axrb::runtime {
// One swapchain image's Vulkan view: a GL 2D texture that ANGLE exports as a
// VkImage. The application's own texture (2D or a multiview array) is copied
// into it when released, layers stacked vertically, so the image is handed to
// Vulkan in one place and never while the application may still render.
struct GlesTransfer {
    uint32_t texture = 0;
    void* eglImage = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool released = false;  // held by Vulkan (glReleaseTexturesANGLE) until the next copy
};

// OpenGL ES applications render through ANGLE (Direct mode selects it as the
// EGL driver). Its Vulkan device is the one the frames are exported from:
// the backend records its copies on ANGLE's queue, under ANGLE's queue lock.
class GlesAngle {
public:
    bool active() const { return device_ != nullptr; }
    // At xrCreateSession: the application's EGLDisplay. Fills the Vulkan
    // binding, ANGLE's queue and its lock for VulkanBackend::initialize.
    bool initialize(void* display, XrGraphicsBindingVulkanKHR& binding, VkQueue& queue, VulkanQueueLock& lock);
    void shutdown() { *this = {}; }
    // Needs the application's context current, as GL swapchain calls do.
    bool create_transfer(uint32_t width, uint32_t height, uint32_t layers, uint32_t internalFormat, GlesTransfer& transfer);
    void destroy_transfer(GlesTransfer& transfer, VkImageLayout layout);
    // At xrReleaseSwapchainImage: copies the application's texture into the
    // transfer image and releases it to Vulkan; layout is where GL left it,
    // and on entry where the last export left it.
    bool hand_over(uint32_t texture, uint32_t target, uint32_t width, uint32_t height, uint32_t layers,
                   GlesTransfer& transfer, VkImageLayout& layout);
private:
    bool current_context() const;
    void watch_errors();
    void* watched_[8]{};
    void (*debugCallback_)(void (*)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, const char*, const void*), const void*) = nullptr;
    void (*enable_)(uint32_t) = nullptr;
    void* display_ = nullptr;
    void* device_ = nullptr;
    void* (*currentContext_)() = nullptr;
    void* (*createImage_)(void*, void*, uint32_t, void*, const int32_t*) = nullptr;
    uint32_t (*destroyImage_)(void*, void*) = nullptr;
    uint32_t (*exportImage_)(void*, void*, void*, void*) = nullptr;
    void (*releaseTextures_)(uint32_t, const uint32_t*, uint32_t*) = nullptr;
    void (*acquireTextures_)(uint32_t, const uint32_t*, const uint32_t*) = nullptr;
    void (*copyImage_)(uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, int32_t,
                       int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) = nullptr;
    void (*genTextures_)(int32_t, uint32_t*) = nullptr;
    void (*deleteTextures_)(int32_t, const uint32_t*) = nullptr;
    void (*bindTexture_)(uint32_t, uint32_t) = nullptr;
    void (*getIntegerv_)(uint32_t, int32_t*) = nullptr;
    void (*texStorage2D_)(uint32_t, int32_t, uint32_t, int32_t, int32_t) = nullptr;
    uint32_t (*getError_)() = nullptr;
    int32_t (*eglError_)() = nullptr;
};
}
#endif
