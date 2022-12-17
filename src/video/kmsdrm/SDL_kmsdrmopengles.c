/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_KMSDRM && SDL_VIDEO_OPENGL_EGL

#include "SDL_log.h"

#include "SDL_kmsdrmvideo.h"
#include "SDL_kmsdrmopengles.h"
#include "SDL_kmsdrmdyn.h"
#include <errno.h>

#ifndef EGL_PLATFORM_GBM_MESA
#define EGL_PLATFORM_GBM_MESA 0x31D7
#endif

/* EGL implementation of SDL OpenGL support */
/* Init the Vulkan-INCOMPATIBLE stuff:
   Reopen FD, create gbm dev, create dumb buffer and setup display plane.
   This is to be called late, in WindowCreate(), and ONLY if this is not
   a Vulkan window.
   We are doing this so late to allow Vulkan to work if we build a VK window.
   These things are incompatible with Vulkan, which accesses the same resources
   internally so they must be free when trying to build a Vulkan surface.
*/
int KMSDRM_GBMInit(_THIS, SDL_DisplayData *dispdata)
{
    SDL_VideoData *viddata = (SDL_VideoData *)_this->driverdata;
    int ret = 0;

    if (!SDL_KMSDRM_HAVE_GBM) {
        return SDL_SetError("Couldn't create gbm device.");
    }

    /* Reopen the FD! */
    viddata->drm_fd = open(viddata->devpath, O_RDWR | O_CLOEXEC);

    /* Set the FD we just opened as current DRM master. */
    KMSDRM_drmSetMaster(viddata->drm_fd);

    /* Create the GBM device. */
    viddata->gbm_dev = KMSDRM_gbm_create_device(viddata->drm_fd);
    if (!viddata->gbm_dev) {
        ret = SDL_SetError("Couldn't create gbm device.");
    }

    viddata->gbm_init = SDL_TRUE;
    viddata->dumb_init = SDL_FALSE;

    return ret;
}

void KMSDRM_GBMDestroySurfaces(_THIS, SDL_Window *window)
{
    SDL_WindowData *windata = ((SDL_WindowData *)window->driverdata);
    SDL_EGL_MakeCurrent(_this, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (windata->egl_surface != EGL_NO_SURFACE) {
        SDL_EGL_DestroySurface(_this, windata->egl_surface);
        windata->egl_surface = EGL_NO_SURFACE;
    }

    /***************************/
    /* Destroy the GBM buffers */
    /***************************/

    if (windata->bo) {
        KMSDRM_gbm_surface_release_buffer(windata->gs, windata->bo);
        windata->bo = NULL;
    }

    if (windata->next_bo) {
        KMSDRM_gbm_surface_release_buffer(windata->gs, windata->next_bo);
        windata->next_bo = NULL;
    }

    /***************************/
    /* Destroy the GBM surface */
    /***************************/

    if (windata->gs) {
        KMSDRM_gbm_surface_destroy(windata->gs);
        windata->gs = NULL;
    }
}

/* Deinit the Vulkan-incompatible KMSDRM stuff. */
void KMSDRM_GBMDeinit(_THIS, SDL_DisplayData *dispdata)
{
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);

    /* Destroy GBM device. GBM surface is destroyed by DestroySurfaces(),
       already called when we get here. */
    if (viddata->gbm_dev) {
        KMSDRM_gbm_device_destroy(viddata->gbm_dev);
        viddata->gbm_dev = NULL;
    }

    /* Finally close DRM FD. May be reopen on next non-vulkan window creation. */
    if (viddata->drm_fd >= 0) {
        close(viddata->drm_fd);
        viddata->drm_fd = -1;
    }

    viddata->gbm_init = SDL_FALSE;
}

void KMSDRM_GLES_DefaultProfileConfig(_THIS, int *mask, int *major, int *minor)
{
    /* if SDL was _also_ built with the Raspberry Pi driver (so we're
       definitely a Pi device), default to GLES2. */
#if SDL_VIDEO_DRIVER_RPI
    *mask = SDL_GL_CONTEXT_PROFILE_ES;
    *major = 2;
    *minor = 0;
#endif
}

int KMSDRM_GLES_LoadLibrary(_THIS, const char *path)
{
    /* Just pretend you do this here, but don't do it until KMSDRM_CreateWindow(),
       where we do the same library load we would normally do here.
       because this gets called by SDL_CreateWindow() before KMSDR_CreateWindow(),
       so gbm dev isn't yet created when this is called, AND we can't alter the
       call order in SDL_CreateWindow(). */
#if 0
    NativeDisplayType display = (NativeDisplayType)((SDL_VideoData *)_this->driverdata)->gbm_dev;
    return SDL_EGL_LoadLibrary(_this, path, display, EGL_PLATFORM_GBM_MESA);
#endif
    return 0;
}

void KMSDRM_GLES_UnloadLibrary(_THIS)
{
    /* As with KMSDRM_GLES_LoadLibrary(), we define our own "dummy" unloading function
       so we manually unload the library whenever we want. */
}

SDL_EGL_CreateContext_impl(KMSDRM)

    int KMSDRM_GLES_SetSwapInterval(_THIS, int interval)
{

    if (!_this->egl_data) {
        return SDL_SetError("EGL not initialized");
    }

    if (interval == 0 || interval == 1) {
        _this->egl_data->egl_swapinterval = interval;
    } else {
        return SDL_SetError("Only swap intervals of 0 or 1 are supported");
    }

    return 0;
}

int KMSDRM_GLES_InitWindow(_THIS, SDL_Window *window)
{
    int ret;
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayForWindow(window)->driverdata;
    SDL_VideoDisplay *display = SDL_GetDisplayForWindow(window);
    NativeDisplayType egl_display;

    /* After SDL_CreateWindow, most SDL2 programs will do SDL_CreateRenderer(),
       which will in turn call GL_CreateRenderer() or GLES2_CreateRenderer().
       In order for the GL_CreateRenderer() or GLES2_CreateRenderer() call to
       succeed without an unnecessary window re-creation, we must:
       -Mark the window as being OPENGL
       -Load the GL library (which can't be done until the GBM device has been
       created, so we have to do it here instead of doing it on VideoInit())
       and mark it as loaded by setting gl_config.driver_loaded to 1.
       So if you ever see KMSDRM_CreateWindow() to be called two times in tests,
       don't be shy to debug GL_CreateRenderer() or GLES2_CreateRenderer()
       to find out why!
    */

    /* Reopen FD, create gbm dev, setup display plane, etc,.
        but only when we come here for the first time,
        and only if it's not a VK window. */
    ret = KMSDRM_GBMInit(_this, dispdata);
    if (ret != 0) {
        return SDL_SetError("Can't init GBM on window creation.");
    }

    /* Manually load the GL library. KMSDRM_EGL_LoadLibrary() has already
       been called by SDL_CreateWindow() but we don't do anything there,
       our KMSDRM_EGL_LoadLibrary() is a dummy precisely to be able to load it here.
       If we let SDL_CreateWindow() load the lib, it would be loaded
       before we call KMSDRM_GBMInit(), causing all GLES programs to fail. */
    if (!_this->egl_data) {
        egl_display = (NativeDisplayType)((SDL_VideoData *)_this->driverdata)->gbm_dev;
        if (SDL_EGL_LoadLibrary(_this, NULL, egl_display, EGL_PLATFORM_GBM_MESA) < 0) {
            /* Try again with OpenGL ES 2.0 */
            _this->gl_config.profile_mask = SDL_GL_CONTEXT_PROFILE_ES;
            _this->gl_config.major_version = 2;
            _this->gl_config.minor_version = 0;
            if (SDL_EGL_LoadLibrary(_this, NULL, egl_display, EGL_PLATFORM_GBM_MESA) < 0) {
                return SDL_SetError("Can't load EGL/GL library on window creation.");
            }
        }

        _this->gl_config.driver_loaded = 1;
    }

    /* Create the cursor BO for the display of this window,
        now that we know this is not a VK window. */
    KMSDRM_CreateCursorBO(display);
    return 0;
}

int KMSDRM_GLES_SwapWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *windata = ((SDL_WindowData *)window->driverdata);
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayForWindow(window)->driverdata;
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);
    KMSDRM_FBInfo *fb_info;
    int ret = 0;

    /* Always wait for the previous issued flip before issuing a new one,
       even if you do async flips. */
    uint32_t flip_flags = DRM_MODE_PAGE_FLIP_EVENT;

    /* Recreate the GBM / EGL surfaces if the display mode has changed */
    if (windata->egl_surface_dirty) {
        KMSDRM_CreateSurfaces(_this, window);
    }

    /* Wait for confirmation that the next front buffer has been flipped, at which
       point the previous front buffer can be released */
    if (!KMSDRM_WaitPageflip(_this, windata)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Wait for previous pageflip failed");
        return 0;
    }

    /* Release the previous front buffer */
    if (windata->bo) {
        KMSDRM_gbm_surface_release_buffer(windata->gs, windata->bo);
        windata->bo = NULL;
    }

    windata->bo = windata->next_bo;

    /* Mark a buffer to becume the next front buffer.
       This won't happen until pagelip completes. */
    if (!(_this->egl_data->eglSwapBuffers(_this->egl_data->egl_display,
                                          windata->egl_surface))) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "eglSwapBuffers failed");
        return 0;
    }

    /* From the GBM surface, get the next BO to become the next front buffer,
       and lock it so it can't be allocated as a back buffer (to prevent EGL
       from drawing into it!) */
    windata->next_bo = KMSDRM_gbm_surface_lock_front_buffer(windata->gs);
    if (!windata->next_bo) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Could not lock front buffer on GBM surface");
        return 0;
    }

    /* Get an actual usable fb for the next front buffer. */
    fb_info = KMSDRM_FBFromBO(_this, windata->next_bo);
    if (fb_info == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Could not get a framebuffer");
        return 0;
    }

    if (!windata->bo) {
        /* On the first swap, immediately present the new front buffer. Before
           drmModePageFlip can be used the CRTC has to be configured to use
           the current connector and mode with drmModeSetCrtc */
        ret = KMSDRM_drmModeSetCrtc(viddata->drm_fd,
                                    dispdata->crtc->crtc_id, fb_info->fb_id, 0, 0,
                                    &dispdata->connector->connector_id, 1, &dispdata->mode);

        if (ret) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Could not set videomode on CRTC.");
            return 0;
        }
    } else {
        /* On subsequent swaps, queue the new front buffer to be flipped during
           the next vertical blank

           Remember: drmModePageFlip() never blocks, it just issues the flip,
           which will be done during the next vblank, or immediately if
           we pass the DRM_MODE_PAGE_FLIP_ASYNC flag.
           Since calling drmModePageFlip() will return EBUSY if we call it
           without having completed the last issued flip, we must pass the
           DRM_MODE_PAGE_FLIP_ASYNC if we don't block on EGL (egl_swapinterval = 0).
           That makes it flip immediately, without waiting for the next vblank
           to do so, so even if we don't block on EGL, the flip will have completed
           when we get here again. */
        if (_this->egl_data->egl_swapinterval == 0 && viddata->async_pageflip_support) {
            flip_flags |= DRM_MODE_PAGE_FLIP_ASYNC;
        }

        ret = KMSDRM_drmModePageFlip(viddata->drm_fd, dispdata->crtc->crtc_id,
                                     fb_info->fb_id, flip_flags, &windata->waiting_for_flip);

        if (ret == 0) {
            windata->waiting_for_flip = SDL_TRUE;
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Could not queue pageflip: %d", ret);
        }

        /* Wait immediately for vsync (as if we only had two buffers).
           Even if we are already doing a WaitPageflip at the beginning of this
           function, this is NOT redundant because here we wait immediately
           after submitting the image to the screen, reducing lag, and if
           we have waited here, there won't be a pending pageflip so the
           WaitPageflip at the beginning of this function will be a no-op.
           Just leave it here and don't worry.
           Run your SDL2 program with "SDL_KMSDRM_DOUBLE_BUFFER=1 <program_name>"
           to enable this. */
        if (windata->double_buffer) {
            if (!KMSDRM_WaitPageflip(_this, windata)) {
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Immediate wait for previous pageflip failed");
                return 0;
            }
        }
    }

    return 1;
}

SDL_EGL_MakeCurrent_impl(KMSDRM)

#endif /* SDL_VIDEO_DRIVER_KMSDRM */

    /* vi: set ts=4 sw=4 expandtab: */
