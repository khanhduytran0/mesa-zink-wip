/*
 * Copyright 2020 Red Hat, Inc.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */


#include "zink_context.h"
#include "zink_screen.h"
#include "zink_resource.h"
#include "zink_copper.h"
#include "vk_enum_to_str.h"

struct copper_winsys
{
   // probably just embed this all in the pipe_screen
   struct sw_winsys base;

   const struct copper_loader_funcs *loader;
};

#define copper_displaytarget(dt) ((struct copper_displaytarget*)dt)

// not sure if cute or vile
static struct zink_screen *
copper_winsys_screen(struct sw_winsys *ws)
{
    return container_of(ws, struct zink_screen, winsys);
}

static VkSurfaceKHR
copper_CreateSurface(struct zink_screen *screen, struct copper_displaytarget *cdt)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult error = VK_SUCCESS;

    VkStructureType type = cdt->info.bos.sType;
    switch (type) {
    case VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR:
       error = VKSCR(CreateXcbSurfaceKHR)(screen->instance, &cdt->info.xcb, NULL, &surface);
       cdt->type = COPPER_X11;
       break;
    case VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR:
       error = VKSCR(CreateWaylandSurfaceKHR)(screen->instance, &cdt->info.wl, NULL, &surface);
       cdt->type = COPPER_WAYLAND;
       break;
    default:
       unreachable("unsupported!");
    }

    if (error != VK_SUCCESS) {
       return VK_NULL_HANDLE;
    }

    VkBool32 supported;
    error = VKSCR(GetPhysicalDeviceSurfaceSupportKHR)(screen->pdev, screen->gfx_queue, surface, &supported);
    if (!zink_screen_handle_vkresult(screen, error) || !supported) {
       VKSCR(DestroySurfaceKHR)(screen->instance, surface, NULL);
       return VK_NULL_HANDLE;
    }

    return surface;
}

static void
destroy_swapchain(struct zink_screen *screen, struct copper_swapchain *cswap)
{
   if (!cswap)
      return;
   free(cswap->images);
   for (unsigned i = 0; i < cswap->num_images; i++) {
      VKSCR(DestroySemaphore)(screen->dev, cswap->acquires[i], NULL);
   }
   free(cswap->acquires);
   hash_table_foreach(cswap->presents, he) {
      struct util_dynarray *arr = he->data;
      while (util_dynarray_contains(arr, VkSemaphore))
         VKSCR(DestroySemaphore)(screen->dev, util_dynarray_pop(arr, VkSemaphore), NULL);
      util_dynarray_fini(arr);
      free(arr);
   }
   _mesa_hash_table_destroy(cswap->presents, NULL);
   VKSCR(DestroySwapchainKHR)(screen->dev, cswap->swapchain, NULL);
   free(cswap);
}

static struct copper_swapchain *
copper_CreateSwapchain(struct zink_screen *screen, struct copper_displaytarget *cdt, unsigned w, unsigned h)
{
   VkResult error = VK_SUCCESS;
   struct copper_swapchain *cswap = CALLOC_STRUCT(copper_swapchain);
   if (!cswap)
      return NULL;
   cswap->last_present_prune = 1;

   if (cdt->swapchain) {
      cswap->scci = cdt->swapchain->scci;
      cswap->scci.oldSwapchain = cdt->swapchain->swapchain;
   } else {
      cswap->scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
      cswap->scci.pNext = NULL;
      cswap->scci.surface = cdt->surface;
      cswap->scci.flags = zink_copper_has_srgb(cdt) ? VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR : 0;
      cswap->scci.imageFormat = cdt->formats[0];
      cswap->scci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      cswap->scci.imageArrayLayers = 1;        // XXX stereo
      cswap->scci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      cswap->scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
      cswap->scci.queueFamilyIndexCount = 0;
      cswap->scci.pQueueFamilyIndices = NULL;
      cswap->scci.compositeAlpha = cdt->info.has_alpha ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      cswap->scci.presentMode = cdt->type == COPPER_X11 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR; // XXX swapint
      cswap->scci.clipped = VK_TRUE;                                   // XXX hmm
   }
   cswap->scci.minImageCount = cdt->caps.minImageCount;
   cswap->scci.preTransform = cdt->caps.currentTransform;
   if (cdt->formats[1])
      cswap->scci.pNext = &cdt->format_list;

   /* different display platforms have, by vulkan spec, different sizing methodologies */
   switch (cdt->type) {
   case COPPER_X11:
      /* With Xcb, minImageExtent, maxImageExtent, and currentExtent must always equal the window size.
       * ...
       * Due to above restrictions, it is only possible to create a new swapchain on this
       * platform with imageExtent being equal to the current size of the window.
       */
      cswap->scci.imageExtent.width = cdt->caps.currentExtent.width;
      cswap->scci.imageExtent.height = cdt->caps.currentExtent.height;
      break;
   case COPPER_WAYLAND:
      /* On Wayland, currentExtent is the special value (0xFFFFFFFF, 0xFFFFFFFF), indicating that the
       * surface size will be determined by the extent of a swapchain targeting the surface. Whatever the
       * application sets a swapchain’s imageExtent to will be the size of the window, after the first image is
       * presented.
       */
      cswap->scci.imageExtent.width = w;
      cswap->scci.imageExtent.height = h;
      break;
   default:
      unreachable("unknown display platform");
   }

   error = VKSCR(CreateSwapchainKHR)(screen->dev, &cswap->scci, NULL,
                                &cswap->swapchain);
   if (error != VK_SUCCESS) {
       mesa_loge("CreateSwapchainKHR failed with %s\n", vk_Result_to_str(error));
       free(cswap);
       return NULL;
   }
   cswap->max_acquires = cswap->scci.minImageCount - cdt->caps.minImageCount;
   cswap->last_present = UINT32_MAX;

   return cswap;
}

static bool
copper_GetSwapchainImages(struct zink_screen *screen, struct copper_swapchain *cswap)
{
   VkResult error = VKSCR(GetSwapchainImagesKHR)(screen->dev, cswap->swapchain, &cswap->num_images, NULL);
   if (!zink_screen_handle_vkresult(screen, error))
      return false;
   cswap->images = malloc(sizeof(VkImage) * cswap->num_images);
   cswap->acquires = calloc(cswap->num_images, sizeof(VkSemaphore));
   cswap->presents = _mesa_hash_table_create_u32_keys(NULL);
   error = VKSCR(GetSwapchainImagesKHR)(screen->dev, cswap->swapchain, &cswap->num_images, cswap->images);
   return zink_screen_handle_vkresult(screen, error);
}

static bool
update_caps(struct zink_screen *screen, struct copper_displaytarget *cdt)
{
   VkResult error = VKSCR(GetPhysicalDeviceSurfaceCapabilitiesKHR)(screen->pdev, cdt->surface, &cdt->caps);
   return zink_screen_handle_vkresult(screen, error);
}

static bool
update_swapchain(struct zink_screen *screen, struct copper_displaytarget *cdt, unsigned w, unsigned h)
{
   if (!update_caps(screen, cdt))
      return false;
   struct copper_swapchain *cswap = copper_CreateSwapchain(screen, cdt, w, h);
   if (!cswap)
      return false;
   destroy_swapchain(screen, cdt->old_swapchain);
   cdt->old_swapchain = cdt->swapchain;
   cdt->swapchain = cswap;

   return copper_GetSwapchainImages(screen, cdt->swapchain);
}

static struct sw_displaytarget *
copper_displaytarget_create(struct sw_winsys *ws, unsigned tex_usage,
                            enum pipe_format format, unsigned width,
                            unsigned height, unsigned alignment,
                            const void *loader_private, unsigned *stride)
{
   struct zink_screen *screen = copper_winsys_screen(ws);
   struct copper_displaytarget *cdt;
   const struct copper_loader_info *info = loader_private;
   unsigned nblocksy, size, format_stride;

   cdt = CALLOC_STRUCT(copper_displaytarget);
   if (!cdt)
      return NULL;

   cdt->refcount = 1;
   cdt->loader_private = (void*)loader_private;
   cdt->info = *info;

   enum pipe_format srgb = PIPE_FORMAT_NONE;
   if (screen->info.have_KHR_swapchain_mutable_format) {
      srgb = util_format_is_srgb(format) ? util_format_linear(format) : util_format_srgb(format);
      /* why do these helpers have different default return values? */
      if (srgb == format)
         srgb = PIPE_FORMAT_NONE;
   }
   cdt->formats[0] = zink_get_format(screen, format);
   if (srgb) {
      cdt->format_list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
      cdt->format_list.pNext = NULL;
      cdt->format_list.viewFormatCount = 2;
      cdt->format_list.pViewFormats = cdt->formats;

      cdt->formats[1] = zink_get_format(screen, srgb);
   }

   cdt->surface = copper_CreateSurface(screen, cdt);
   if (!cdt->surface)
      goto out;

   if (!update_swapchain(screen, cdt, width, height))
      goto out;

   *stride = cdt->stride;
   return (struct sw_displaytarget *)cdt;

//moar cleanup
out:
   return NULL;
}

static void
copper_displaytarget_destroy(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
   struct zink_screen *screen = copper_winsys_screen(ws);
   struct copper_displaytarget *cdt = copper_displaytarget(dt);
   if (!p_atomic_dec_zero(&cdt->refcount))
      return;
   destroy_swapchain(screen, cdt->swapchain);
   destroy_swapchain(screen, cdt->old_swapchain);
   VKSCR(DestroySurfaceKHR)(screen->instance, cdt->surface, NULL);
   FREE(dt);
}

struct sw_winsys zink_copper = {
   .destroy = NULL,
   .is_displaytarget_format_supported = NULL,
   .displaytarget_create = copper_displaytarget_create,
   .displaytarget_from_handle = NULL,
   .displaytarget_get_handle = NULL,
   .displaytarget_map = NULL,
   .displaytarget_unmap = NULL,
   .displaytarget_display = NULL,
   .displaytarget_destroy = copper_displaytarget_destroy,
};

static bool
copper_acquire(struct zink_screen *screen, struct zink_resource *res, uint64_t timeout)
{
   struct copper_displaytarget *cdt = copper_displaytarget(res->obj->dt);
   if (res->obj->acquire)
      return true;
   res->obj->acquire = VK_NULL_HANDLE;
   VkSemaphore acquire = VK_NULL_HANDLE;
   if (res->obj->new_dt) {
update_swapchain:
      if (!update_swapchain(screen, cdt, res->base.b.width0, res->base.b.height0)) {
         //???
      }
      res->obj->new_dt = false;
      res->layout = VK_IMAGE_LAYOUT_UNDEFINED;
      res->obj->access = 0;
      res->obj->access_stage = 0;
   }
   if (timeout == UINT64_MAX && util_queue_is_initialized(&screen->flush_queue) &&
       p_atomic_read_relaxed(&cdt->swapchain->num_acquires) > cdt->swapchain->max_acquires) {
      util_queue_fence_wait(&res->obj->present_fence);
   }
   VkSemaphoreCreateInfo sci = {
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      NULL,
      0
   };
   VkResult ret;
   if (!acquire) {
      ret = VKSCR(CreateSemaphore)(screen->dev, &sci, NULL, &acquire);
      assert(acquire);
      if (ret != VK_SUCCESS)
         return false;
   }
   unsigned prev = res->obj->dt_idx;
   ret = VKSCR(AcquireNextImageKHR)(screen->dev, cdt->swapchain->swapchain, timeout, acquire, VK_NULL_HANDLE, &res->obj->dt_idx);
   if (ret != VK_SUCCESS && ret != VK_SUBOPTIMAL_KHR) {
      if (ret == VK_ERROR_OUT_OF_DATE_KHR)
         goto update_swapchain;
      VKSCR(DestroySemaphore)(screen->dev, acquire, NULL);
      return false;
   }
   assert(prev != res->obj->dt_idx);
   cdt->swapchain->acquires[res->obj->dt_idx] = res->obj->acquire = acquire;
   res->obj->image = cdt->swapchain->images[res->obj->dt_idx];
   res->obj->acquired = false;
   if (timeout == UINT64_MAX) {
      res->obj->indefinite_acquire = true;
      p_atomic_inc(&cdt->swapchain->num_acquires);
   }
   return ret == VK_SUCCESS;
}

bool
zink_copper_acquire(struct zink_context *ctx, struct zink_resource *res, uint64_t timeout)
{
   assert(res->obj->dt);
   struct copper_displaytarget *cdt = copper_displaytarget(res->obj->dt);
   const struct copper_swapchain *cswap = cdt->swapchain;
   res->obj->new_dt |= res->base.b.width0 != cswap->scci.imageExtent.width ||
                       res->base.b.height0 != cswap->scci.imageExtent.height;
   bool ret = copper_acquire(zink_screen(ctx->base.screen), res, timeout);
   if (cswap != cdt->swapchain)
      ctx->swapchain_size = cdt->swapchain->scci.imageExtent;
   return ret;
}

VkSemaphore
zink_copper_acquire_submit(struct zink_screen *screen, struct zink_resource *res)
{
   assert(res->obj->dt);
   struct copper_displaytarget *cdt = copper_displaytarget(res->obj->dt);
   if (res->obj->acquired)
      return VK_NULL_HANDLE;
   assert(res->obj->acquire);
   res->obj->acquired = true;
   /* this is now owned by the batch */
   cdt->swapchain->acquires[res->obj->dt_idx] = VK_NULL_HANDLE;
   return res->obj->acquire;
}

VkSemaphore
zink_copper_present(struct zink_screen *screen, struct zink_resource *res)
{
   assert(res->obj->dt);
   struct copper_displaytarget *cdt = copper_displaytarget(res->obj->dt);
   assert(!res->obj->present);
   VkSemaphoreCreateInfo sci = {
      VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      NULL,
      0
   };
   assert(res->obj->acquired);
   //error checking
   VkResult ret = VKSCR(CreateSemaphore)(screen->dev, &sci, NULL, &res->obj->present);
   return res->obj->present;
}

struct copper_present_info {
   VkPresentInfoKHR info;
   uint32_t image;
   struct zink_resource *res;
   VkSemaphore sem;
   bool indefinite_acquire;
};

static void
copper_present(void *data, void *gdata, int thread_idx)
{
   struct copper_present_info *cpi = data;
   struct copper_displaytarget *cdt = cpi->res->obj->dt;
   struct zink_screen *screen = gdata;
   VkResult error;
   cpi->info.pResults = &error;

   simple_mtx_lock(&screen->queue_lock);
   VkResult error2 = VKSCR(QueuePresentKHR)(screen->thread_queue, &cpi->info);
   simple_mtx_unlock(&screen->queue_lock);
   unsigned num = 0;
   cdt->swapchain->last_present = cpi->image;
   if (cpi->indefinite_acquire)
      num = p_atomic_dec_return(&cdt->swapchain->num_acquires);
   if (error2 == VK_SUBOPTIMAL_KHR)
      cpi->res->obj->new_dt = true;

   /* it's illegal to destroy semaphores if they're in use by a cmdbuf.
    * but what does "in use" actually mean?
    * in truth, when using timelines, nobody knows. especially not VVL.
    *
    * thus, to avoid infinite error spam and thread-related races,
    * present semaphores need their own free queue based on the
    * last-known completed timeline id so that the semaphore persists through
    * normal cmdbuf submit/signal and then also exists here when it's needed for the present operation
    */
   struct util_dynarray *arr;
   for (; screen->last_finished && cdt->swapchain->last_present_prune != screen->last_finished; cdt->swapchain->last_present_prune++) {
      struct hash_entry *he = _mesa_hash_table_search(cdt->swapchain->presents,
                                                      (void*)(uintptr_t)cdt->swapchain->last_present_prune);
      if (he) {
         arr = he->data;
         while (util_dynarray_contains(arr, VkSemaphore))
            VKSCR(DestroySemaphore)(screen->dev, util_dynarray_pop(arr, VkSemaphore), NULL);
         util_dynarray_fini(arr);
         free(arr);
         _mesa_hash_table_remove(cdt->swapchain->presents, he);
      }
   }
   /* queue this wait semaphore for deletion on completion of the next batch */
   assert(screen->curr_batch > 0);
   uint32_t next = screen->curr_batch + 1;
   struct hash_entry *he = _mesa_hash_table_search(cdt->swapchain->presents, (void*)(uintptr_t)next);
   if (he)
      arr = he->data;
   else {
      arr = malloc(sizeof(struct util_dynarray));
      util_dynarray_init(arr, NULL);
      _mesa_hash_table_insert(cdt->swapchain->presents, (void*)(uintptr_t)next, arr);
   }
   util_dynarray_append(arr, VkSemaphore, cpi->sem);
   free(cpi);
}

void
zink_copper_present_queue(struct zink_screen *screen, struct zink_resource *res)
{
   assert(res->obj->dt);
   struct copper_displaytarget *cdt = copper_displaytarget(res->obj->dt);
   assert(res->obj->acquired);
   assert(res->obj->present);
   struct copper_present_info *cpi = malloc(sizeof(struct copper_present_info));
   cpi->sem = res->obj->present;
   cpi->res = res;
   cpi->indefinite_acquire = res->obj->indefinite_acquire;
   res->obj->last_dt_idx = cpi->image = res->obj->dt_idx;
   cpi->info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
   cpi->info.pNext = NULL;
   cpi->info.waitSemaphoreCount = 1;
   cpi->info.pWaitSemaphores = &cpi->sem;
   cpi->info.swapchainCount = 1;
   cpi->info.pSwapchains = &cdt->swapchain->swapchain;
   cpi->info.pImageIndices = &cpi->image;
   cpi->info.pResults = NULL;
   res->obj->present = VK_NULL_HANDLE;
   if (util_queue_is_initialized(&screen->flush_queue)) {
      util_queue_add_job(&screen->flush_queue, cpi, &res->obj->present_fence,
                         copper_present, NULL, 0);
   } else {
      copper_present(cpi, screen, 0);
   }
   res->obj->acquire = VK_NULL_HANDLE;
   res->obj->indefinite_acquire = res->obj->acquired = false;
   res->obj->dt_idx = UINT32_MAX;
}

void
zink_copper_acquire_readback(struct zink_context *ctx, struct zink_resource *res)
{
   struct zink_screen *screen = zink_screen(ctx->base.screen);
   assert(res->obj->dt);
   struct copper_displaytarget *cdt = copper_displaytarget(res->obj->dt);
   const struct copper_swapchain *cswap = cdt->swapchain;
   uint32_t last_dt_idx = res->obj->last_dt_idx;
   if (!res->obj->acquire)
      copper_acquire(screen, res, UINT64_MAX);
   if (res->obj->last_dt_idx == UINT32_MAX)
      return;
   while (res->obj->dt_idx != last_dt_idx) {
      if (!zink_copper_present_readback(screen, res))
         break;
      while (!copper_acquire(screen, res, 0));
   }
   if (cswap != cdt->swapchain)
      ctx->swapchain_size = cdt->swapchain->scci.imageExtent;
}

bool
zink_copper_present_readback(struct zink_screen *screen, struct zink_resource *res)
{
   VkSubmitInfo si = {0};
   if (res->obj->last_dt_idx == UINT32_MAX)
      return true;
   si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   si.signalSemaphoreCount = 1;
   VkPipelineStageFlags mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   si.pWaitDstStageMask = &mask;
   VkSemaphore acquire = zink_copper_acquire_submit(screen, res);
   VkSemaphore present = zink_copper_present(screen, res);
   if (screen->threaded)
      util_queue_finish(&screen->flush_queue);
   si.waitSemaphoreCount = !!acquire;
   si.pWaitSemaphores = &acquire;
   si.pSignalSemaphores = &present;
   VkResult error = VKSCR(QueueSubmit)(screen->thread_queue, 1, &si, VK_NULL_HANDLE);
   if (!zink_screen_handle_vkresult(screen, error))
      return false;

   zink_copper_present_queue(screen, res);
   error = VKSCR(QueueWaitIdle)(screen->queue);
   return zink_screen_handle_vkresult(screen, error);
}

bool
zink_copper_update(struct pipe_screen *pscreen, struct pipe_resource *pres, int *w, int *h)
{
   struct zink_resource *res = zink_resource(pres);
   struct zink_screen *screen = zink_screen(pscreen);
   assert(res->obj->dt);
   struct copper_displaytarget *cdt = copper_displaytarget(res->obj->dt);
   if (cdt->type != COPPER_X11) {
      *w = res->base.b.width0;
      *h = res->base.b.height0;
      return true;
   }
   if (!update_caps(screen, cdt)) {
      debug_printf("zink: failed to update swapchain capabilities");
      return false;
   }
   *w = cdt->caps.currentExtent.width;
   *h = cdt->caps.currentExtent.height;
   return true;
}
