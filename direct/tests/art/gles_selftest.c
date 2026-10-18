// OpenGL ES through ANGLE on the forwarded Vulkan driver: initialize EGL,
// render into a pbuffer and a texture, and check the Vulkan interop the OpenXR
// runtime uses for GLES frames: ANGLE's VkDevice, a texture's VkImage, and
// handing the image between GL and Vulkan (GL_ANGLE_vulkan_image).
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <link.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "Failed: %s (egl 0x%x gl 0x%x)\n", #x, eglGetError(), glGetError()); return 1; } } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #x, r_); return 1; } } while (0)

#define EGL_DEVICE_EXT 0x322C
#define EGL_VULKAN_VERSION_ANGLE 0x34A8
#define EGL_VULKAN_INSTANCE_ANGLE 0x34A9
#define EGL_VULKAN_PHYSICAL_DEVICE_ANGLE 0x34AB
#define EGL_VULKAN_DEVICE_ANGLE 0x34AC
#define EGL_VULKAN_QUEUE_ANGLE 0x34AF
#define EGL_VULKAN_QUEUE_FAMILIY_INDEX_ANGLE 0x34D0
#define GL_LAYOUT_COLOR_ATTACHMENT_EXT 0x958E

// GL_EXT_semaphore layout names, as glReleaseTexturesANGLE reports them.
static VkImageLayout vulkan_layout(GLenum layout) {
    switch (layout) {
        case 0x958D: return VK_IMAGE_LAYOUT_GENERAL;
        case 0x958E: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case 0x9591: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case 0x9592: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case 0x9593: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

typedef void* (*GetProc)(const char*);
static GetProc angle_proc;

// The same lookup the OpenXR runtime does: ANGLE's own entry points, found
// among the loaded libraries, take ANGLE's handles rather than libEGL's.
static ElfW(Addr) angle_base;
static const ElfW(Dyn)* angle_dynamic;
static int find_angle(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size; (void)data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, "libEGL_angle.so")) return 0;
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            angle_dynamic = (const ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    angle_base = info->dlpi_addr;
    return 1;
}
static void* angle_symbol(const char* name) {
    const ElfW(Sym)* symbols = NULL; const char* strings = NULL; const uint32_t* gnu = NULL; const uint32_t* sysv = NULL;
    // Bionic leaves .dynamic unrelocated: its addresses are offsets from the load base.
    for (const ElfW(Dyn)* d = angle_dynamic; d && d->d_tag != DT_NULL; ++d) {
        const ElfW(Addr) at = d->d_un.d_ptr < angle_base ? angle_base + d->d_un.d_ptr : d->d_un.d_ptr;
        if (d->d_tag == DT_SYMTAB) symbols = (const ElfW(Sym)*)at;
        else if (d->d_tag == DT_STRTAB) strings = (const char*)at;
        else if (d->d_tag == DT_GNU_HASH) gnu = (const uint32_t*)at;
        else if (d->d_tag == DT_HASH) sysv = (const uint32_t*)at;
    }
    if (!symbols || !strings) return NULL;
    uint32_t count = 0;
    if (sysv) count = sysv[1];
    else if (gnu) {
        const uint32_t buckets = gnu[0], offset = gnu[1], words = gnu[2];
        const uint32_t* bucket = gnu + 4 + words * (sizeof(ElfW(Addr)) / 4);
        const uint32_t* chain = bucket + buckets;
        for (uint32_t b = 0; b < buckets; ++b) if (bucket[b] >= count) count = bucket[b] + 1;
        if (count > offset) while (!(chain[count - 1 - offset] & 1)) ++count;
    }
    for (uint32_t i = 0; i < count; ++i)
        if (symbols[i].st_value && !strcmp(strings + symbols[i].st_name, name)) return (void*)(angle_base + symbols[i].st_value);
    return NULL;
}

// Reads texel (10, 10) of each layer; with band, layers are stacked in one
// 2D image band rows apart, as the runtime stores multiview swapchains.
static int check_image(VkDevice device, VkQueue queue, uint32_t family, VkPhysicalDevice physical, VkImage image,
                       uint32_t layers, uint32_t band, VkImageLayout layout, void (*lock)(void*), void (*unlock)(void*), void* egl_device,
                       const unsigned char expected[8]) {
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
    VkCommandPool pool; VK(vkCreateCommandPool(device, &poolInfo, NULL, &pool));
    VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(device, &allocate, &cmd));
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4 * layers, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer; VK(vkCreateBuffer(device, &bufferInfo, NULL, &buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t type = 0;
    while (type < memory.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) ||
           (memory.memoryTypes[type].propertyFlags & 6) != 6)) ++type;
    VkMemoryAllocateInfo memoryInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    VkDeviceMemory bufferMemory; VK(vkAllocateMemory(device, &memoryInfo, NULL, &bufferMemory));
    VK(vkBindBufferMemory(device, buffer, bufferMemory, 0));
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(cmd, &begin));
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, band ? 1 : layers}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    for (uint32_t layer = 0; layer < layers; ++layer) {
        VkBufferImageCopy copy = {4 * layer, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, band ? 0 : layer, 1},
                                  {10, (int32_t)(10 + band * layer), 0}, {1, 1, 1}};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    }
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = layout;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence; VK(vkCreateFence(device, &fenceInfo, NULL, &fence));
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    lock(egl_device);
    VkResult submitted = vkQueueSubmit(queue, 1, &submit, fence);
    unlock(egl_device);
    VK(submitted);
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    unsigned char* pixels; VK(vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, (void**)&pixels));
    int good = 1;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        const unsigned char* p = pixels + 4 * layer;
        printf("Vulkan copy layer %u: %u %u %u %u\n", layer, p[0], p[1], p[2], p[3]);
        good &= !memcmp(p, expected + 4 * layer, 4);
    }
    vkUnmapMemory(device, bufferMemory);
    vkDestroyFence(device, fence, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, bufferMemory, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    return good ? 0 : 1;
}

int main(void) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    REQUIRE(display != EGL_NO_DISPLAY);
    EGLint major = 0, minor = 0;
    REQUIRE(eglInitialize(display, &major, &minor));
    printf("EGL %d.%d vendor=%s\nEGL version=%s\nEGL extensions=%s\n", major, minor,
           eglQueryString(display, EGL_VENDOR), eglQueryString(display, EGL_VERSION),
           eglQueryString(display, EGL_EXTENSIONS));
    const EGLint configAttributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE};
    EGLConfig config;
    EGLint configs = 0;
    REQUIRE(eglChooseConfig(display, configAttributes, &config, 1, &configs) && configs == 1);
    const EGLint surfaceAttributes[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    REQUIRE(surface != EGL_NO_SURFACE);
    const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    REQUIRE(context != EGL_NO_CONTEXT);
    REQUIRE(eglMakeCurrent(display, surface, surface, context));
    printf("GL vendor=%s\nGL renderer=%s\nGL version=%s\nGLSL=%s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
           glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    printf("GL extensions=%s\n", glGetString(GL_EXTENSIONS));
    // Named by Direct mode's libGLESv2_angle.so shim, beside ANGLE's multiview.
    REQUIRE(strstr((const char*)glGetString(GL_EXTENSIONS), "GL_OVR_multiview_multisampled_render_to_texture"));

    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4] = {0};
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("pbuffer pixel %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    REQUIRE(pixel[0] == 255 && pixel[1] == 0 && pixel[3] == 255);

    // ANGLE's handles and Vulkan objects.
    dl_iterate_phdr(find_angle, NULL);
    angle_proc = (GetProc)angle_symbol("eglGetProcAddress");
    REQUIRE(angle_proc);
    EGLDisplay (*currentDisplay)(void) = angle_proc("eglGetCurrentDisplay");
    EGLContext (*currentContext)(void) = angle_proc("eglGetCurrentContext");
    EGLBoolean (*queryDisplay)(EGLDisplay, EGLint, EGLAttrib*) = angle_proc("eglQueryDisplayAttribEXT");
    EGLBoolean (*queryDevice)(void*, EGLint, EGLAttrib*) = angle_proc("eglQueryDeviceAttribEXT");
    const char* (*queryDeviceString)(void*, EGLint) = angle_proc("eglQueryDeviceStringEXT");
    void (*lockQueue)(void*) = angle_proc("eglLockVulkanQueueANGLE");
    void (*unlockQueue)(void*) = angle_proc("eglUnlockVulkanQueueANGLE");
    EGLImageKHR (*createImage)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*) = angle_proc("eglCreateImageKHR");
    EGLBoolean (*destroyImage)(EGLDisplay, EGLImageKHR) = angle_proc("eglDestroyImageKHR");
    EGLBoolean (*exportImage)(EGLDisplay, EGLImageKHR, void*, void*) = angle_proc("eglExportVkImageANGLE");
    void (*releaseTextures)(GLuint, const GLuint*, GLenum*) = angle_proc("glReleaseTexturesANGLE");
    void (*acquireTextures)(GLuint, const GLuint*, const GLenum*) = angle_proc("glAcquireTexturesANGLE");
    REQUIRE(currentDisplay && currentContext && queryDisplay && queryDevice && queryDeviceString && lockQueue &&
            unlockQueue && createImage && destroyImage && exportImage && releaseTextures && acquireTextures);
    EGLDisplay angleDisplay = currentDisplay();
    EGLContext angleContext = currentContext();
    printf("libEGL display %p context %p; ANGLE display %p context %p\n", display, context, angleDisplay, angleContext);
    EGLAttrib device = 0;
    REQUIRE(queryDisplay(angleDisplay, EGL_DEVICE_EXT, &device) && device);
    printf("device extensions=%s\n", queryDeviceString((void*)device, EGL_EXTENSIONS));
    EGLAttrib values[6] = {0};
    const EGLint names[6] = {EGL_VULKAN_VERSION_ANGLE, EGL_VULKAN_INSTANCE_ANGLE, EGL_VULKAN_PHYSICAL_DEVICE_ANGLE,
                             EGL_VULKAN_DEVICE_ANGLE, EGL_VULKAN_QUEUE_ANGLE, EGL_VULKAN_QUEUE_FAMILIY_INDEX_ANGLE};
    for (int i = 0; i < 6; ++i) {
        REQUIRE(queryDevice((void*)device, names[i], &values[i]));
        printf("device attribute 0x%x = 0x%llx\n", names[i], (unsigned long long)values[i]);
    }
    VkInstance instance = (VkInstance)values[1];
    VkPhysicalDevice physical = (VkPhysicalDevice)values[2];
    VkDevice vkDevice = (VkDevice)values[3];
    VkQueue queue = (VkQueue)values[4];
    uint32_t family = (uint32_t)values[5];
    uint32_t count = 8; VkPhysicalDevice physicals[8];
    VK(vkEnumeratePhysicalDevices(instance, &count, physicals));
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) found |= physicals[i] == physical;
    REQUIRE(found);
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical, &properties);
    printf("ANGLE's Vulkan device: %s\n", properties.deviceName);

    // A 2D texture: render, export, release to Vulkan, copy, acquire back.
    GLuint textures[2] = {0}, framebuffer = 0;
    glGenTextures(2, textures);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, 256, 256);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
    REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    const EGLint imageAttributes[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
    EGLImageKHR image = createImage(angleDisplay, angleContext, EGL_GL_TEXTURE_2D_KHR,
                                    (EGLClientBuffer)(uintptr_t)textures[0], imageAttributes);
    REQUIRE(image != EGL_NO_IMAGE_KHR);
    VkImage vkImage = VK_NULL_HANDLE;
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    REQUIRE(exportImage(angleDisplay, image, &vkImage, &info) && vkImage);
    printf("2D texture image %p format=%d %ux%u layers=%u mips=%u usage=0x%x flags=0x%x\n", (void*)vkImage, info.format,
           info.extent.width, info.extent.height, info.arrayLayers, info.mipLevels, info.usage, info.flags);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLenum layout = 0;
    releaseTextures(1, &textures[0], &layout);
    printf("released layout 0x%x\n", layout);
    REQUIRE(vulkan_layout(layout) != VK_IMAGE_LAYOUT_UNDEFINED);
    const unsigned char blue[8] = {0, 0, 255, 255, 0, 0, 255, 255};
    REQUIRE(!check_image(vkDevice, queue, family, physical, vkImage, 1, 0, vulkan_layout(layout),
                         lockQueue, unlockQueue, (void*)device, blue));
    acquireTextures(1, &textures[0], &layout);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(100, 100, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("after acquire %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    REQUIRE(pixel[1] == 255 && pixel[2] == 0);
    destroyImage(angleDisplay, image);

    // A two-layer array texture (multiview swapchains) cannot be an EGL image
    // source, and ANGLE imports Vulkan images with one layer only, so the
    // runtime copies the layers into a 2D texture twice as tall and exports that.
    glBindTexture(GL_TEXTURE_2D_ARRAY, textures[1]);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 128, 128, 2);
    // Unreal attaches multiview eye buffers through the multisampled entry
    // point, which ANGLE lacks; Direct mode's libEGL_angle.so shim adds it.
    void (*multiview)(GLenum, GLenum, GLuint, GLint, GLsizei, GLint, GLsizei) =
        (void (*)(GLenum, GLenum, GLuint, GLint, GLsizei, GLint, GLsizei))eglGetProcAddress("glFramebufferTextureMultisampleMultiviewOVR");
    REQUIRE(multiview);
    multiview(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textures[1], 0, 4, 0, 2);
    REQUIRE(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    for (int layer = 0; layer < 2; ++layer) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textures[1], 0, layer);
        glClearColor(layer, 0, 1 - layer, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    GLuint tall = 0;
    glGenTextures(1, &tall);
    glBindTexture(GL_TEXTURE_2D, tall);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 128, 256);
    image = createImage(angleDisplay, angleContext, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)tall, imageAttributes);
    REQUIRE(image != EGL_NO_IMAGE_KHR);
    REQUIRE(exportImage(angleDisplay, image, &vkImage, &info) && vkImage);
    void (*copyImage)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei,
                      GLsizei, GLsizei) = angle_proc("glCopyImageSubData");
    REQUIRE(copyImage);
    for (int layer = 0; layer < 2; ++layer)
        copyImage(textures[1], GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, tall, GL_TEXTURE_2D, 0, 0, 128 * layer, 0, 128, 128, 1);
    releaseTextures(1, &tall, &layout);
    printf("stacked layers released layout 0x%x (gl 0x%x)\n", layout, glGetError());
    REQUIRE(vulkan_layout(layout) != VK_IMAGE_LAYOUT_UNDEFINED);
    const unsigned char layered[8] = {0, 0, 255, 255, 255, 0, 0, 255};
    REQUIRE(!check_image(vkDevice, queue, family, physical, vkImage, 2, 128, vulkan_layout(layout),
                         lockQueue, unlockQueue, (void*)device, layered));
    acquireTextures(1, &tall, &layout);
    destroyImage(angleDisplay, image);
    glDeleteTextures(1, &tall);
    glDeleteTextures(2, textures);

    puts("GLES_SELFTEST_PASS");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return 0;
}
