/* Copyright (C) 2010-2020 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------------
 * The following license statement only applies to this libretro API header (libretro_vulkan.h)
 * ---------------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef LIBRETRO_VULKAN_H__
#define LIBRETRO_VULKAN_H__

#include "xenia/libretro/libretro.h"
#include "xenia/ui/vulkan/vulkan_api.h"

#define RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION 5
#define RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION 1

struct retro_vulkan_image
{
   VkImageView image_view;
   VkImageLayout image_layout;
   VkImageViewCreateInfo create_info;
};

typedef void (*retro_vulkan_set_image_t)(void *handle,
      const struct retro_vulkan_image *image,
      uint32_t num_semaphores,
      const VkSemaphore *semaphores,
      uint32_t src_queue_family);

typedef uint32_t (*retro_vulkan_get_sync_index_t)(void *handle);
typedef uint32_t (*retro_vulkan_get_sync_index_mask_t)(void *handle);
typedef void (*retro_vulkan_set_command_buffers_t)(void *handle,
      uint32_t num_cmd,
      const VkCommandBuffer *cmd);
typedef void (*retro_vulkan_wait_sync_index_t)(void *handle);
typedef void (*retro_vulkan_lock_queue_t)(void *handle);
typedef void (*retro_vulkan_unlock_queue_t)(void *handle);
typedef void (*retro_vulkan_set_signal_semaphore_t)(void *handle, VkSemaphore semaphore);

typedef const VkApplicationInfo *(*retro_vulkan_get_application_info_t)(void);

struct retro_vulkan_context
{
   VkPhysicalDevice gpu;
   VkDevice device;
   VkQueue queue;
   uint32_t queue_family_index;
   VkQueue presentation_queue;
   uint32_t presentation_queue_family_index;
};

/* This is only used in v1 of the negotiation interface.
 * It is deprecated since it cannot express PDF2 features or optional extensions. */
typedef bool (*retro_vulkan_create_device_t)(
      struct retro_vulkan_context *context,
      VkInstance instance,
      VkPhysicalDevice gpu,
      VkSurfaceKHR surface,
      PFN_vkGetInstanceProcAddr get_instance_proc_addr,
      const char **required_device_extensions,
      unsigned num_required_device_extensions,
      const char **required_device_layers,
      unsigned num_required_device_layers,
      const VkPhysicalDeviceFeatures *required_features);

typedef void (*retro_vulkan_destroy_device_t)(void);

struct retro_hw_render_context_negotiation_interface_vulkan
{
   enum retro_hw_render_context_negotiation_interface_type interface_type;
   unsigned interface_version;

   retro_vulkan_get_application_info_t get_application_info;

   retro_vulkan_create_device_t create_device;

   retro_vulkan_destroy_device_t destroy_device;
};

struct retro_hw_render_interface_vulkan
{
   enum retro_hw_render_interface_type interface_type;
   unsigned interface_version;

   void *handle;

   VkInstance instance;
   VkPhysicalDevice gpu;
   VkDevice device;

   PFN_vkGetDeviceProcAddr get_device_proc_addr;
   PFN_vkGetInstanceProcAddr get_instance_proc_addr;

   VkQueue queue;
   unsigned queue_index;

   retro_vulkan_set_image_t set_image;

   retro_vulkan_get_sync_index_t get_sync_index;

   retro_vulkan_get_sync_index_mask_t get_sync_index_mask;

   retro_vulkan_set_command_buffers_t set_command_buffers;

   retro_vulkan_wait_sync_index_t wait_sync_index;

   retro_vulkan_lock_queue_t lock_queue;
   retro_vulkan_unlock_queue_t unlock_queue;

   retro_vulkan_set_signal_semaphore_t set_signal_semaphore;
};

#endif
