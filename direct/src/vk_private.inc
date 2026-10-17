// Uncompressed images (debug.axrb.astc_rgba8: ASTC and ETC2 as RGBA8, EAC as
// R16/RG16) take 2-4 bytes a texel, 4-16 times the compressed data the game
// sized its budgets for. Engines that budget
// textures by vkGetImageMemoryRequirements (Unreal's texture streaming pool)
// then see their pool overflow and keep only low-resolution mips. So these
// images report the memory their compressed data would need, and are backed instead
// by memory from this file: 256 MiB device-local blocks, suballocated, which
// also keeps thousands of textures far below the driver's allocation limit.
// The game's own memory bound to such an image is simply left unused.
struct PrivateImage {
  VkDevice device{};
  VkDeviceSize reported{};      // what the game is told: the ASTC footprint
  VkMemoryRequirements real{};  // what the RGBA8 image needs
  bool queried{};
  uint32_t type{~0u};           // backing memory type and range, once bound
  VkDeviceMemory memory{};
  VkDeviceSize offset{}, size{};
};
struct PrivateBlock {
  VkDevice device{};
  uint32_t type{};
  VkDeviceMemory memory{};
  VkDeviceSize size{};
  std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> length
};
std::mutex g_private_lock;
std::map<VkImage, PrivateImage> g_private;
std::vector<PrivateBlock> g_private_blocks;
constexpr VkDeviceSize kPrivateBlock = VkDeviceSize(256) << 20;

VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
  return alignment > 1 ? (value + alignment - 1) / alignment * alignment : value;
}

// The game-visible size of a compressed image: its blocks (16 bytes for ASTC,
// 8 or 16 for ETC2/EAC), every mip and layer.
VkDeviceSize compressed_footprint(const VkImageCreateInfo &info, int bw, int bh, uint32_t blockBytes) {
  VkDeviceSize bytes = 0;
  for (uint32_t mip = 0; mip < info.mipLevels; ++mip) {
    const VkDeviceSize w = std::max(1u, info.extent.width >> mip), h = std::max(1u, info.extent.height >> mip);
    bytes += (w + bw - 1) / bw * ((h + bh - 1) / bh) * blockBytes * std::max(1u, info.extent.depth >> mip);
  }
  return bytes * info.arrayLayers;
}

void track_private(VkDevice device, VkImage image, const VkImageCreateInfo &info) {
  int bw = 0, bh = 0;
  bool srgb = false;
  texture::Codec codec = texture::Codec::None;
  VkFormat target{};
  if (!texture::astc_rgba8_enabled() || info.tiling != VK_IMAGE_TILING_OPTIMAL ||
      !texture::compressed_format(info.format, bw, bh, srgb, codec, target) || !texture::texel_bytes(target))
    return;
  const uint32_t blockBytes = codec == texture::Codec::Astc ? 16 : texture::block_bytes(codec);
  std::lock_guard guard(g_private_lock);
  g_private[image] = {device, compressed_footprint(info, bw, bh, blockBytes)};
}

bool is_private(VkImage image) {
  std::lock_guard guard(g_private_lock);
  return g_private.count(image) != 0;
}

// The requirements the game sees. False when the image is not a private one.
bool private_requirements(VkDevice device, VkImage image, VkMemoryRequirements *out) {
  {
    std::lock_guard guard(g_private_lock);
    if (!g_private.count(image)) return false;
  }
  VkMemoryRequirements real{};
  host<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, image, &real);
  std::lock_guard guard(g_private_lock);
  auto &entry = g_private[image];
  entry.real = real;
  entry.queried = true;
  *out = real;
  out->size = align_up(std::max<VkDeviceSize>(entry.reported, 1), real.alignment);
  return true;
}

uint32_t private_type(const DeviceCalls &calls, uint32_t bits) {
  for (VkMemoryPropertyFlags wanted : {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), VkMemoryPropertyFlags(0)})
    for (uint32_t i = 0; i < calls.memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (calls.memory.memoryTypes[i].propertyFlags & wanted) == wanted &&
          !(calls.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT))
        return i;
  return ~0u;
}

// First fit in an existing block of this type, else a new block. Lock held.
bool private_take(const DeviceCalls &calls, VkDevice device, uint32_t type, VkDeviceSize size, VkDeviceSize alignment,
                  VkDeviceMemory *memory, VkDeviceSize *offset) {
  for (int pass = 0; pass < 2; ++pass) {
    for (auto &block : g_private_blocks) {
      if (block.device != device || block.type != type) continue;
      for (auto it = block.free.begin(); it != block.free.end(); ++it) {
        const VkDeviceSize start = align_up(it->first, alignment), end = it->first + it->second;
        if (start + size > end) continue;
        const VkDeviceSize before = start - it->first, after = end - (start + size), at = it->first;
        block.free.erase(it);
        if (before) block.free[at] = before;
        if (after) block.free[start + size] = after;
        *memory = block.memory;
        *offset = start;
        return true;
      }
    }
    if (pass) break;
    PrivateBlock block{device, type, VK_NULL_HANDLE, std::max(kPrivateBlock, align_up(size, alignment))};
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = block.size;
    allocate.memoryTypeIndex = type;
    if (calls.allocateMemory(device, &allocate, nullptr, &block.memory) != VK_SUCCESS) return false;
    block.free[0] = block.size;
    g_private_blocks.push_back(std::move(block));
    LOGI("vulkan", "uncompressed texture memory: %zu block(s) of type %u", g_private_blocks.size(), type);
  }
  return false;
}

// Binds a private image to its own memory; the game's memory is ignored.
VkResult bind_private(VkDevice device, VkImage image) {
  auto calls = calls_for(device);
  if (!calls) return VK_ERROR_INITIALIZATION_FAILED;
  VkMemoryRequirements real{};
  {
    std::lock_guard guard(g_private_lock);
    auto &entry = g_private[image];
    if (entry.memory) return VK_SUCCESS;
    real = entry.real;
    if (!entry.queried) host<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, image, &real);
  }
  const uint32_t type = private_type(*calls, real.memoryTypeBits);
  if (type == ~0u) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  VkDeviceMemory memory{};
  VkDeviceSize offset = 0;
  {
    std::lock_guard guard(g_private_lock);
    if (!private_take(*calls, device, type, real.size, real.alignment, &memory, &offset)) {
      LOGW("vulkan", "uncompressed texture memory unavailable (%llu bytes)", static_cast<unsigned long long>(real.size));
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    auto &entry = g_private[image];
    entry.type = type;
    entry.memory = memory;
    entry.offset = offset;
    entry.size = real.size;
  }
  return host<PFN_vkBindImageMemory>("vkBindImageMemory")(device, image, memory, offset);
}

// Returns an image's range to its block. Called before or after the image is destroyed.
void release_private(VkImage image) {
  std::lock_guard guard(g_private_lock);
  auto it = g_private.find(image);
  if (it == g_private.end()) return;
  const PrivateImage entry = it->second;
  g_private.erase(it);
  if (!entry.memory) return;
  for (auto &block : g_private_blocks) {
    if (block.memory != entry.memory) continue;
    VkDeviceSize start = entry.offset, length = entry.size;
    auto next = block.free.lower_bound(start);
    if (next != block.free.end() && start + length == next->first) {
      length += next->second;
      next = block.free.erase(next);
    }
    if (next != block.free.begin()) {
      auto previous = std::prev(next);
      if (previous->first + previous->second == start) {
        start = previous->first;
        length += previous->second;
        block.free.erase(previous);
      }
    }
    block.free[start] = length;
    return;
  }
}

void destroy_private_device(VkDevice device) {
  auto calls = calls_for(device);
  std::lock_guard guard(g_private_lock);
  for (auto it = g_private.begin(); it != g_private.end();) it = it->second.device == device ? g_private.erase(it) : std::next(it);
  for (auto it = g_private_blocks.begin(); it != g_private_blocks.end();) {
    if (it->device != device) { ++it; continue; }
    if (calls) calls->freeMemory(device, it->memory, nullptr);
    it = g_private_blocks.erase(it);
  }
}
