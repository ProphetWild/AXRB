#pragma once
// Refract's ASTC -> BC3 compute kernel, with AXRB-owned staging and lifetimes.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <memory>
#include <mutex>
#include <vector>
#include <type_traits>
#include "astc_bc3_spirv.h"
namespace axrb::texture {
class GpuAstc {
public:
    struct Push {
        uint32_t sourceOffset, destinationOffset, rowBlocks, sliceBlocks, blocksX, blocksY, outBlocksX, outBlocksY, width, height, format;
    };
    struct Job {
        GpuAstc* owner;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorPool setPool = VK_NULL_HANDLE;
        ~Job() { owner->release(*this); }
    };
    GpuAstc(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, PFN_vkSetDeviceLoaderData loader)
        : device_(device), loader_(loader) { ready_ = loader && createGpu(gdpa); }
    ~GpuAstc() {
        if (!ready_) return;
        for (auto pool : descriptorPools_) destroyDescriptorPool_(device_, pool, nullptr);
        destroyPipeline_(device_, pipeline_, nullptr);
        destroyPipelineLayout_(device_, pipelineLayout_, nullptr);
        destroySetLayout_(device_, setLayout_, nullptr);
    }
    bool ready() const { return ready_; }
    // Where the kernel's input comes from: copied on the GPU, at execution, out
    // of the game's own upload buffer. Mapped game memory is usually uncached
    // (measured 0.04-0.3 GB/s for CPU reads on an RTX 5090), so the CPU never
    // touches it; and the bytes are read exactly when the game's copy would.
    struct Input { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0, size = 0, stagingOffset = 0; };
    std::shared_ptr<Job> record(uint32_t family, VkBuffer buffer, VkDeviceSize bytes,
                               const std::vector<Push>& regions, const std::vector<uint32_t>& slices, const Input& input = {}) {
        auto job = std::make_shared<Job>(); job->owner = this;
        // Descriptor pools are externally synchronized. Each job owns a separate
        // command pool, so reset/free of different application buffers is safe.
        std::unique_lock lock(mutex_);
        if (!allocateSet(*job)) return {};
        VkDescriptorBufferInfo binding{buffer, 0, bytes};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i=0;i<2;++i) {
            writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=job->set;
            writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo=&binding;
        }
        updateSets_(device_,2,writes,0,nullptr);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.queueFamilyIndex=family;
        if (createCommandPool_(device_,&pool,nullptr,&job->pool)!=VK_SUCCESS) return {};
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool=job->pool; alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount=1;
        if (allocateCommands_(device_,&alloc,&job->command)!=VK_SUCCESS || loader_(device_,job->command)!=VK_SUCCESS) return {};
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (beginCommands_(job->command,&begin)!=VK_SUCCESS) return {};
        if (input.buffer) {
            const VkBufferCopy region{input.offset, input.stagingOffset, input.size};
            copyBuffer_(job->command, input.buffer, buffer, 1, &region);
            VkMemoryBarrier copied{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            copied.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; copied.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
            barrier_(job->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&copied,0,nullptr,0,nullptr);
        }
        bindPipeline_(job->command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
        bindSets_(job->command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout_,0,1,&job->set,0,nullptr);
        for (size_t i=0;i<regions.size();++i) {
            const auto& p=regions[i];
            pushConstants_(job->command,pipelineLayout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push),&p);
            dispatch_(job->command,(p.outBlocksX+3)/4,(p.outBlocksY+3)/4,slices[i]);
        }
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier_(job->command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        if (endCommands_(job->command)!=VK_SUCCESS) return {};
        return job;
    }
private:
    void release(Job& job) {
        std::lock_guard lock(mutex_);
        if (job.pool) destroyCommandPool_(device_,job.pool,nullptr);
        if (job.set) freeSets_(device_,job.setPool,1,&job.set);
    }
    // A job keeps its set until the game's command buffer is done with it, and
    // a level load records hundreds of uploads before submitting (Deadpool: over
    // 500 in one buffer). A fixed pool ran dry and sent the rest to the CPU
    // decoder, so pools are added as needed; freed sets are reused first.
    bool allocateSet(Job& job) {
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorSetCount = 1; setInfo.pSetLayouts = &setLayout_;
        for (size_t tried = 0; tried < descriptorPools_.size(); ++tried) {
            const size_t index = (currentPool_ + tried) % descriptorPools_.size();
            setInfo.descriptorPool = descriptorPools_[index];
            if (allocateSets_(device_, &setInfo, &job.set) == VK_SUCCESS) {
                currentPool_ = index; job.setPool = descriptorPools_[index]; return true;
            }
        }
        if (descriptorPools_.size() >= kMaxPools) return false;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (!createPool(pool)) return false;
        descriptorPools_.push_back(pool);
        currentPool_ = descriptorPools_.size() - 1;
        setInfo.descriptorPool = pool;
        if (allocateSets_(device_, &setInfo, &job.set) != VK_SUCCESS) return false;
        job.setPool = pool;
        return true;
    }
    bool createPool(VkDescriptorPool& pool) {
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kMaxSets};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; poolInfo.maxSets = kMaxSets;
        poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize;
        return createDescriptorPool_(device_, &poolInfo, nullptr, &pool) == VK_SUCCESS;
    }
    VkDevice device_;
    PFN_vkSetDeviceLoaderData loader_;
    bool ready_=false;
    std::mutex mutex_;
    bool createGpu(PFN_vkGetDeviceProcAddr gdpa) {
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(gdpa(device_, name)); return fn != nullptr; };
        PFN_vkCreateShaderModule createShader = nullptr; PFN_vkDestroyShaderModule destroyShader = nullptr;
        PFN_vkCreateDescriptorSetLayout createSetLayout = nullptr; PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
        PFN_vkCreateComputePipelines createPipelines = nullptr;
        bool ok = load(createShader, "vkCreateShaderModule") && load(destroyShader, "vkDestroyShaderModule") &&
            load(createSetLayout, "vkCreateDescriptorSetLayout") && load(destroySetLayout_, "vkDestroyDescriptorSetLayout") &&
            load(createPipelineLayout, "vkCreatePipelineLayout") && load(destroyPipelineLayout_, "vkDestroyPipelineLayout") &&
            load(createPipelines, "vkCreateComputePipelines") && load(destroyPipeline_, "vkDestroyPipeline") &&
            load(createDescriptorPool_, "vkCreateDescriptorPool") && load(destroyDescriptorPool_, "vkDestroyDescriptorPool") &&
            load(allocateSets_, "vkAllocateDescriptorSets") && load(freeSets_, "vkFreeDescriptorSets") &&
            load(updateSets_, "vkUpdateDescriptorSets") && load(createCommandPool_, "vkCreateCommandPool") &&
            load(destroyCommandPool_, "vkDestroyCommandPool") && load(allocateCommands_, "vkAllocateCommandBuffers") &&
            load(beginCommands_, "vkBeginCommandBuffer") && load(endCommands_, "vkEndCommandBuffer") &&
            load(bindPipeline_, "vkCmdBindPipeline") && load(bindSets_, "vkCmdBindDescriptorSets") &&
            load(pushConstants_, "vkCmdPushConstants") && load(dispatch_, "vkCmdDispatch") && load(barrier_, "vkCmdPipelineBarrier") &&
            load(copyBuffer_, "vkCmdCopyBuffer");
        if (!ok) return false;
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = sizeof(shaders::kAstcBc3); shaderInfo.pCode = shaders::kAstcBc3;
        VkShaderModule shader = VK_NULL_HANDLE;
        if (createShader(device_, &shaderInfo, nullptr, &shader) != VK_SUCCESS) return false;
        VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                   {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 2; setInfo.pBindings = bindings;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1; layoutInfo.pSetLayouts = &setLayout_; layoutInfo.pushConstantRangeCount = 1; layoutInfo.pPushConstantRanges = &range;
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
        VkDescriptorPool firstPool = VK_NULL_HANDLE;
        ok = createSetLayout(device_, &setInfo, nullptr, &setLayout_) == VK_SUCCESS &&
             createPipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) == VK_SUCCESS &&
             (pipelineInfo.layout = pipelineLayout_, createPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_) == VK_SUCCESS) &&
             createPool(firstPool);
        if (firstPool) descriptorPools_.push_back(firstPool);
        destroyShader(device_, shader, nullptr);
        if (!ok) {
            for (auto pool : descriptorPools_) destroyDescriptorPool_(device_, pool, nullptr);
            descriptorPools_.clear();
            if (pipeline_) destroyPipeline_(device_, pipeline_, nullptr);
            if (pipelineLayout_) destroyPipelineLayout_(device_, pipelineLayout_, nullptr);
            if (setLayout_) destroySetLayout_(device_, setLayout_, nullptr);
        }
        return ok;
    }

    static constexpr uint32_t kMaxSets = 256;
    static constexpr size_t kMaxPools = 64;  // 16384 uploads in flight
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> descriptorPools_;
    size_t currentPool_ = 0;
    PFN_vkCreateDescriptorPool createDescriptorPool_ = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroySetLayout_ = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout_ = nullptr;
    PFN_vkDestroyPipeline destroyPipeline_ = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool_ = nullptr;
    PFN_vkAllocateDescriptorSets allocateSets_ = nullptr;
    PFN_vkFreeDescriptorSets freeSets_ = nullptr;
    PFN_vkUpdateDescriptorSets updateSets_ = nullptr;
    PFN_vkCreateCommandPool createCommandPool_ = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool_ = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommands_ = nullptr;
    PFN_vkBeginCommandBuffer beginCommands_ = nullptr;
    PFN_vkEndCommandBuffer endCommands_ = nullptr;
    PFN_vkCmdBindPipeline bindPipeline_ = nullptr;
    PFN_vkCmdBindDescriptorSets bindSets_ = nullptr;
    PFN_vkCmdPushConstants pushConstants_ = nullptr;
    PFN_vkCmdDispatch dispatch_ = nullptr;
    PFN_vkCmdPipelineBarrier barrier_ = nullptr;
    PFN_vkCmdCopyBuffer copyBuffer_ = nullptr;
};
}
