// Video memory by the heaps' live budgets, for every allocation through VMA (vk.h maps vmaCreateImage, vmaCreateBuffer
// and vmaAllocateMemory here).
//
// An RX 580 (8 GiB, AMD 23.10) has a 6 GiB device-local heap and a 2 GiB device-local, host-visible one. The heap limits
// of vk_device.c (vram_heap_limits) are fixed when the device is made: each heap's budget then, less a tenth for the
// memory VMA does not see. That memory grows later: AMD's FidelityFX (FSR 3.1 upscaling contexts, up to six, and frame
// generation with its swapchain and context) allocates its own, outside VMA. With FSR 3.1 and frame generation on at
// 1080p it reached 882 MiB, and heap 0 held 4644 MiB of VMA's beside it: 5526 of a 5526 MiB budget, while heap 2 had
// 1757 MiB free. The next upload submit failed with VK_ERROR_OUT_OF_DEVICE_MEMORY and the game ended (2026-11-11).
// Only texture images looked at the budget (vk_texture.c image_alloc); render targets, pool memory, mirrors and the
// rest were placed in heap 0 up to VMA's fixed limit.
//
// Here each allocation leaves out the memory types of a device-local heap whose use, as the driver reports it
// (VK_EXT_memory_budget: VMA's budget, which counts the memory VMA does not see), is within the reserve of its budget,
// and VMA places it in the other device-local heap, or system memory, as it does past the fixed limits. The reserve
// (NX2NAVI_VRAM_RESERVE_MB, 512 with low_end_gpu = 1, else 0, off) is room left for allocations VMA does not make, which can come at
// once: AMD's contexts are made when the upscaler or the frame generation setting changes. An allocation that finds
// no room that way is made as asked, as before. Allocations from a VMA pool and those for one memory type alone (the
// headroom's anchors, vk_headroom.c) are made as asked.
#define VK_VMA_BUDGET_IMPL
#include <stdlib.h>
#include "vk.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

static VkDeviceSize reserve_bytes(void) {
    static int64_t r = -1;
    if (r < 0) {
        const char *e = cfg_get("NX2NAVI_VRAM_RESERVE_MB");
        long mb = e && *e ? atol(e) : 0;
        r = (int64_t)(mb < 0 ? 0 : mb) << 20;
    }
    return (VkDeviceSize)r;
}

static volatile int64_t n_moved;     // allocations placed outside a full heap
static volatile uint32_t full_seen;  // heaps already logged as full (bit per heap)

// The memory types of bits that may take a new allocation: those of device-local heaps within reserve of their budget
// are left out. Returns bits itself when nothing is left out or nothing would be left.
static uint32_t budget_bits(VmaAllocator vma, uint32_t bits) {
    VkDeviceSize reserve = reserve_bytes();
    if (!reserve) return bits;
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(vma, &mp);
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(vma, b);
    uint32_t full = 0;
    for (uint32_t h = 0; h < mp->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if ((mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && b[h].budget &&
            b[h].usage + reserve > b[h].budget)
            full |= 1u << h;
    if (!full) return bits;
    uint32_t out = 0;
    for (uint32_t t = 0; t < mp->memoryTypeCount; t++)
        if ((bits >> t & 1) && !(full >> mp->memoryTypes[t].heapIndex & 1)) out |= 1u << t;
    if (!out || out == bits) return bits;
    uint32_t told = __atomic_fetch_or(&full_seen, full, __ATOMIC_RELAXED);
    for (uint32_t h = 0; h < VK_MAX_MEMORY_HEAPS; h++)
        if ((full >> h & 1) && !(told >> h & 1))
            rt_log("vk: video memory heap %u full: %llu of its %llu MiB budget in use (%llu MiB of it VMA's), less %llu MiB "
                   "kept free (NX2NAVI_VRAM_RESERVE_MB): new allocations go to the other heaps while it is", h,
                   (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20),
                   (unsigned long long)(b[h].statistics.blockBytes >> 20), (unsigned long long)(reserve >> 20));
    return out;
}

static void note_moved(void) {
    int64_t n = __atomic_add_fetch(&n_moved, 1, __ATOMIC_RELAXED);
    if (!(n & (n - 1)) && n >= 16)
        rt_log("vk: video memory: %lld allocations placed outside a full heap so far", (long long)n);
}

uint64_t vk_vma_moved(void) { return (uint64_t)__atomic_load_n(&n_moved, __ATOMIC_RELAXED); }

// Device-local types of bits (0: all), from the allocator's properties: whether the request could land in a heap
// budget_bits leaves out
static uint32_t all_bits(VmaAllocator vma, uint32_t bits) {
    if (bits) return bits;
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(vma, &mp);
    return mp->memoryTypeCount >= 32 ? ~0u : (1u << mp->memoryTypeCount) - 1;
}

VkResult vk_vma_create_image(VmaAllocator vma, const VkImageCreateInfo *ici, const VmaAllocationCreateInfo *ai, VkImage *img,
                             VmaAllocation *a, VmaAllocationInfo *info) {
    if (!ai->pool) {
        uint32_t bits = all_bits(vma, ai->memoryTypeBits), nb = budget_bits(vma, bits);
        if (nb != bits) {
            VmaAllocationCreateInfo b = *ai;
            b.memoryTypeBits = nb;
            if (vmaCreateImage(vma, ici, &b, img, a, info) == VK_SUCCESS) return note_moved(), VK_SUCCESS;
        }
    }
    return vmaCreateImage(vma, ici, ai, img, a, info);
}

VkResult vk_vma_create_buffer(VmaAllocator vma, const VkBufferCreateInfo *bci, const VmaAllocationCreateInfo *ai,
                              VkBuffer *buf, VmaAllocation *a, VmaAllocationInfo *info) {
    if (!ai->pool) {
        uint32_t bits = all_bits(vma, ai->memoryTypeBits), nb = budget_bits(vma, bits);
        if (nb != bits) {
            VmaAllocationCreateInfo b = *ai;
            b.memoryTypeBits = nb;
            if (vmaCreateBuffer(vma, bci, &b, buf, a, info) == VK_SUCCESS) return note_moved(), VK_SUCCESS;
        }
    }
    return vmaCreateBuffer(vma, bci, ai, buf, a, info);
}

VkResult vk_vma_allocate_memory(VmaAllocator vma, const VkMemoryRequirements *req, const VmaAllocationCreateInfo *ai,
                                VmaAllocation *a, VmaAllocationInfo *info) {
    uint32_t bits = req->memoryTypeBits & all_bits(vma, ai->memoryTypeBits);
    if (!ai->pool && (bits & (bits - 1))) {   // more than one type: one alone is asked for on purpose
        uint32_t nb = budget_bits(vma, bits);
        if (nb != bits) {
            VkMemoryRequirements r = *req;
            r.memoryTypeBits = nb;
            if (vmaAllocateMemory(vma, &r, ai, a, info) == VK_SUCCESS) return note_moved(), VK_SUCCESS;
        }
    }
    return vmaAllocateMemory(vma, req, ai, a, info);
}
