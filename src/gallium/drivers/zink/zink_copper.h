/*
 * Copyright © 2021 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 * 
 * Authors:
 *    Mike Blumenkrantz <michael.blumenkrantz@gmail.com>
 */

#ifndef ZINK_COPPER_H
#define ZINK_COPPER_H

#include <vulkan/vulkan.h>

struct copper_swapchain {
   VkSwapchainKHR swapchain;
   VkImage *images;
   unsigned last_present;
   unsigned num_images;
   VkSemaphore *acquires;
   uint32_t last_present_prune;
   struct hash_table *presents;
   VkSwapchainCreateInfoKHR scci;
   unsigned num_acquires;
   unsigned max_acquires;
};

enum copper_type {
   COPPER_X11,
   COPPER_WAYLAND,
};

struct copper_loader_info {
   union {
      VkBaseOutStructure bos;
#ifdef VK_USE_PLATFORM_XCB_KHR
      VkXcbSurfaceCreateInfoKHR xcb;
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
      VkWaylandSurfaceCreateInfoKHR wl;
#endif
   };
   bool has_alpha;
};

struct copper_displaytarget
{
   unsigned refcount;
   VkFormat formats[2];
   unsigned width;
   unsigned height;
   unsigned stride;
   void *loader_private;

   VkSurfaceKHR surface;
   struct copper_swapchain *swapchain;
   struct copper_swapchain *old_swapchain;

   struct copper_loader_info info;

   VkSurfaceCapabilitiesKHR caps;
   VkImageFormatListCreateInfoKHR format_list;
   enum copper_type type;
};

struct zink_screen;
struct zink_resource;

static inline bool
zink_copper_has_srgb(const struct copper_displaytarget *cdt)
{
   return cdt->formats[1] != VK_FORMAT_UNDEFINED;
}

static inline bool
zink_copper_last_present_eq(const struct copper_displaytarget *cdt, uint32_t idx)
{
   return cdt->swapchain->last_present == idx;
}

bool
zink_copper_acquire(struct zink_context *ctx, struct zink_resource *res, uint64_t timeout);
VkSemaphore
zink_copper_acquire_submit(struct zink_screen *screen, struct zink_resource *res);
VkSemaphore
zink_copper_present(struct zink_screen *screen, struct zink_resource *res); 
void
zink_copper_present_queue(struct zink_screen *screen, struct zink_resource *res);
void
zink_copper_acquire_readback(struct zink_context *ctx, struct zink_resource *res);
bool
zink_copper_present_readback(struct zink_screen *screen, struct zink_resource *res);
void
zink_copper_deinit_displaytarget(struct zink_screen *screen, struct copper_displaytarget *cdt);
#endif
