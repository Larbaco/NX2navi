// DLSS Frame Generation through NVIDIA Streamline (vk_dlssg.c, nx2navi_dlssg.dll).
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

int vk_dlssg_wanted(void);   // NX2NAVI_FRAMEGEN = dlss with DLSS or FSR 3.1 upscaling on (read once)
int vk_dlssg_early(void);    // before the instance exists: loads the DLL and Streamline; 1 when its proxies are there
// vk_device.c: the instance and device through Streamline's proxies when vk_dlssg_early succeeded, else the loader's
VkResult vk_dlssg_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out);
VkResult vk_dlssg_enumerate(VkInstance inst, uint32_t *n, VkPhysicalDevice *pd);   // through the interposer too
PFN_vkVoidFunction vk_dlssg_instance_proc(VkInstance inst, const char *name);   // the interposer's, or NULL
VkResult vk_dlssg_create_device(VkInstance inst, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out);
void vk_dlssg_device_ready(VkDevice dev);   // after the device: checks DLSS-G and takes the swapchain hooks
int vk_dlssg_active(void);
// gfx_present.c's swapchain calls while active
VkResult vk_dlssg_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc);
void vk_dlssg_destroy_swapchain(VkDevice d, VkSwapchainKHR sc);
VkResult vk_dlssg_get_images(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *img);
VkResult vk_dlssg_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx);
VkResult vk_dlssg_present(VkQueue q, const VkPresentInfoKHR *pi);
void vk_dlssg_frame(VkCommandBuffer cb, VkExtent2D ext);   // each present, before its command buffer is submitted
// Frames are numbered by the game's presents (frame n ends with the n-th). Reflex's frame timing while active:
void vk_dlssg_frame_start(void);    // the game starts a frame (nvn_api_obj.c nvnWindowAcquireTexture): Reflex's sleep
void vk_dlssg_game_present(void);   // the game presents it (vk_backend.c vk_be_present): counts it
// its submissions went out (vk_backend.c: vk_be_present, or replay_main with asynchronous replay): the render-submit-end
// marker; frame is gfx_counters.frames of its present, the same count
void vk_dlssg_submitted(uint64_t frame);
void vk_dlssg_present_frame(uint64_t frame);   // the present thread's next blit and present are of this frame
uint64_t vk_dlssg_shown(void);   // the frames the display got, generated ones included (present thread); 0 when off
void vk_dlssg_release(void);
