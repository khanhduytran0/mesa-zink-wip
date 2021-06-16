/*
 * Copyright 2020 Red Hat, Inc.
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

#include "util/format/u_format.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_box.h"
#include "pipe/p_context.h"
#include "pipe-loader/pipe_loader.h"
#include "state_tracker/st_context.h"
#include "os/os_process.h"
#include "zink/zink_public.h"
#include "zink/zink_instance.h"

#include "dri_screen.h"
#include "dri_context.h"
#include "dri_drawable.h"
#include "dri_helpers.h"
#include "dri_query_renderer.h"

#include <xcb/xcb.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>

struct copper_surface {
   VkSurfaceKHR surface;
   VkSwapchainKHR swapchain;

   // ...
};

#if 0
static __DRIimageExtension driVkImageExtension = {
    .base = { __DRI_IMAGE, 6 },

    .createImageFromRenderbuffer  = dri2_create_image_from_renderbuffer,
    .createImageFromTexture = dri2_create_from_texture,
    .destroyImage = dri2_destroy_image,
};
#endif

// hmm...
static const __DRIextension *drivk_screen_extensions[] = {
   &driTexBufferExtension.base,
   &dri2RendererQueryExtension.base,
   &dri2ConfigQueryExtension.base,
   &dri2FenceExtension.base,
   &dri2NoErrorExtension.base,
   // &driVkImageExtension.base,
   &dri2FlushControlExtension.base,
   NULL
};

static const __DRIconfig **
copper_init_screen(__DRIscreen * sPriv)
{
   // const __DRIcopperLoaderExtension *loader = sPriv->copper.loader;
   const __DRIconfig **configs;
   struct dri_screen *screen;
   struct pipe_screen *pscreen = NULL;

   screen = CALLOC_STRUCT(dri_screen);
   if (!screen)
      return NULL;

   screen->sPriv = sPriv;
   screen->fd = -1; // XXX maybe use this to OOB that it's vulkan

   sPriv->driverPrivate = (void *)screen;

   if (pipe_loader_sw_probe_dri(&screen->dev, NULL /* ooookay */)) {
      dri_init_options(screen);

      pscreen = pipe_loader_create_screen(screen->dev);
   }

   if (!pscreen)
      goto fail;

   configs = dri_init_screen_helper(screen, pscreen);
   if (!configs)
      goto fail;

   sPriv->extensions = drivk_screen_extensions;

   return configs;
fail:
   dri_destroy_screen_helper(screen);
   if (screen->dev)
      pipe_loader_release(&screen->dev, 1);
   FREE(screen);
   return NULL;
}

// copypasta alert

static inline void
drisw_present_texture(struct pipe_context *pipe, __DRIdrawable *dPriv,
                      struct pipe_resource *ptex, struct pipe_box *sub_box)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);
   struct dri_screen *screen = dri_screen(drawable->sPriv);

   screen->base.screen->flush_frontbuffer(screen->base.screen, pipe, ptex, 0, 0, drawable, sub_box);
}

#if 0
static inline void
drisw_invalidate_drawable(__DRIdrawable *dPriv)
{
   struct dri_drawable *drawable = dri_drawable(dPriv);

   drawable->texture_stamp = dPriv->lastStamp - 1;

   p_atomic_inc(&drawable->base.stamp);
}

static inline void
drisw_copy_to_front(struct pipe_context *pipe,
                    __DRIdrawable * dPriv,
                    struct pipe_resource *ptex)
{
   drisw_present_texture(pipe, dPriv, ptex, NULL);

   drisw_invalidate_drawable(dPriv);
}
#endif

/*
 * Backend functions for st_framebuffer interface and swap_buffers.
 */

UNUSED static int64_t
copperSwapBuffers(__DRIdrawable *draw, int64_t target_msc, int64_t divisor,
                  int64_t remainder, const int *rects, int n_rects,
                  int force_copy)
{
   struct dri_context *ctx = dri_get_current(draw->driScreenPriv);
   struct dri_drawable *drawable = dri_drawable(draw);
   struct pipe_resource *ptex;
    
   if (!ctx)
      return 0;
    
   ptex = drawable->textures[ST_ATTACHMENT_BACK_LEFT];
    
   if (ptex) {
      if (ctx->pp)
         pp_run(ctx->pp, ptex, ptex, drawable->textures[ST_ATTACHMENT_DEPTH_STENCIL]);
    
      if (ctx->hud)
         hud_run(ctx->hud, ctx->st->cso_context, ptex);
    
      ctx->st->flush(ctx->st, ST_FLUSH_FRONT, NULL, NULL, NULL);
    
#if 0
      if (drawable->stvis.samples > 1) {
         /* Resolve the back buffer. */
         dri_pipe_blit(ctx->st->pipe,
                       drawable->textures[ST_ATTACHMENT_BACK_LEFT],
                       drawable->msaa_textures[ST_ATTACHMENT_BACK_LEFT]);
      }

      drisw_copy_to_front(ctx->st->pipe, draw, ptex);
#endif

      // zink_queue_present(drawable); or something
   }

   return 0;
}

static void
copper_allocate_textures(struct dri_context *ctx,
                         struct dri_drawable *drawable,
                         const enum st_attachment_type *statts,
                         unsigned statts_count)
{
   struct dri_screen *screen = dri_screen(drawable->sPriv);
   struct pipe_resource templ;
   unsigned width, height;
   boolean resized;
   unsigned i;

   width  = drawable->dPriv->w;
   height = drawable->dPriv->h;

   resized = (drawable->old_w != width ||
              drawable->old_h != height);

   /* remove outdated textures */
   if (resized) {
      for (i = 0; i < ST_ATTACHMENT_COUNT; i++) {
         pipe_resource_reference(&drawable->textures[i], NULL);
         pipe_resource_reference(&drawable->msaa_textures[i], NULL);
      }
   }

   memset(&templ, 0, sizeof(templ));
   templ.target = screen->target;
   templ.width0 = width;
   templ.height0 = height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.last_level = 0;

   for (i = 0; i < statts_count; i++) {
      enum pipe_format format;
      unsigned bind;

      /* the texture already exists or not requested */
      if (drawable->textures[statts[i]])
         continue;

      dri_drawable_get_format(drawable, statts[i], &format, &bind);

      /* if we don't do any present, no need for display targets */
      if (statts[i] != ST_ATTACHMENT_DEPTH_STENCIL)
         bind |= PIPE_BIND_DISPLAY_TARGET;

      if (format == PIPE_FORMAT_NONE)
         continue;

      templ.format = format;
      templ.bind = bind;
      templ.nr_samples = 0;
      templ.nr_storage_samples = 0;

      // XXX port the drisw thing here!
#if 0
      if (statts[i] == ST_ATTACHMENT_FRONT_LEFT &&
          screen->base.screen->resource_create_front &&
          loader->base.version >= 3) {
         drawable->textures[statts[i]] =
            screen->base.screen->resource_create_front(screen->base.screen, &templ, (const void *)drawable);
      } else
#endif
         drawable->textures[statts[i]] =
            screen->base.screen->resource_create(screen->base.screen, &templ);

      if (drawable->stvis.samples > 1) {
         templ.bind = templ.bind &
            ~(PIPE_BIND_SCANOUT | PIPE_BIND_SHARED | PIPE_BIND_DISPLAY_TARGET);
         templ.nr_samples = drawable->stvis.samples;
         templ.nr_storage_samples = drawable->stvis.samples;
         drawable->msaa_textures[statts[i]] =
            screen->base.screen->resource_create(screen->base.screen, &templ);

         dri_pipe_blit(ctx->st->pipe,
                       drawable->msaa_textures[statts[i]],
                       drawable->textures[statts[i]]);
      }
   }

   drawable->old_w = width;
   drawable->old_h = height;
}

static void
copper_update_drawable_info(struct dri_drawable *drawable)
{
   // __DRIdrawable *dPriv = drawable->dPriv;
   // ...
}

static bool
copper_flush_frontbuffer(struct dri_context *ctx,
                         struct dri_drawable *drawable,
                         enum st_attachment_type statt)
{
   *(int *)0 = 0;
   return false;
}

static void
copper_update_tex_buffer(struct dri_drawable *drawable,
                         struct dri_context *ctx,
                         struct pipe_resource *res)
{
   *(int *)0 = 0;
}

static void
copper_flush_swapbuffers(struct dri_context *ctx,
                         struct dri_drawable *drawable)
{
   *(int *)0 = 0;
}

struct copper_drawable {
   struct dri_drawable base;
   VkSurfaceKHR surface;
   union {
      VkBaseOutStructure bos;
      VkXcbSurfaceCreateInfoKHR xcb;
   } sci;
   VkSwapchainCreateInfoKHR scci;
};

// XXX this frees its second argument as a side effect - regardless of success
// - since the point is to use it as the superclass initializer before we add
// our own state. kindagross but easier than fixing the object model first.
static struct copper_drawable *
copper_create_drawable(__DRIdrawable *dPriv, struct dri_drawable *base)
{
   struct copper_drawable *_ret = CALLOC_STRUCT(copper_drawable);

   if (!_ret)
      goto out;
   struct dri_drawable *ret = &_ret->base;

   // copy all the elements
   *ret = *base;

   // relocate references to the old struct
   ret->base.visual = &ret->stvis;
   ret->base.st_manager_private = (void *) ret;
   dPriv->driverPrivate = ret;

   // and fill in the vtable
   ret->allocate_textures = copper_allocate_textures;
   ret->update_drawable_info = copper_update_drawable_info;
   ret->flush_frontbuffer = copper_flush_frontbuffer;
   ret->update_tex_buffer = copper_update_tex_buffer;
   ret->flush_swapbuffers = copper_flush_swapbuffers;

out:
   free(base);
   return _ret;
}

static boolean
copper_create_buffer(__DRIscreen * sPriv,
                     __DRIdrawable * dPriv,
                     const struct gl_config *visual, boolean isPixmap)
{
   struct copper_drawable *drawable = NULL;
   VkResult error;

   if (!dri_create_buffer(sPriv, dPriv, visual, isPixmap))
      return FALSE;

   drawable = copper_create_drawable(dPriv, dPriv->driverPrivate);
   if (!drawable)
      return FALSE;

   error = sPriv->copper_loader->SetSurfaceCreateInfo(dPriv->loaderPrivate,
                                                      &drawable->sci.bos);
   if (error != VK_SUCCESS) {
      free(drawable);
      return FALSE;
   }

   struct dri_screen *screen = sPriv->driverPrivate;
   struct pipe_screen *pscreen = screen->base.screen;

   VkStructureType type = drawable->sci.bos.sType;
   if (type == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR)
      error = vkCreateXcbSurfaceKHR(zink_screen_get_instance(pscreen),
                                    &drawable->sci.xcb, NULL,
                                    &drawable->surface);
   // else if (etc.) {

   return TRUE;
}

const __DRIcopperExtension driCopperExtension = {
   .base = { __DRI_COPPER, 1 },
   // XXX
};

const struct __DriverAPIRec galliumvk_driver_api = {
   .InitScreen = copper_init_screen,
   .DestroyScreen = dri_destroy_screen,
   .CreateContext = dri_create_context,
   .DestroyContext = dri_destroy_context,
   .CreateBuffer = copper_create_buffer,
   .DestroyBuffer = dri_destroy_buffer, // XXX cuprify
   .SwapBuffers = NULL,
   .MakeCurrent = dri_make_current,
   .UnbindContext = dri_unbind_context,
   .CopySubBuffer = NULL, //copper_copy_sub_buffer,
};

const __DRIextension *galliumvk_driver_extensions[] = {
   &driCoreExtension.base,
   &driImageDriverExtension.base,
   &driCopperExtension.base,
   &gallium_config_options.base,
   NULL
};

/* vim: set sw=3 ts=8 sts=3 expandtab: */
