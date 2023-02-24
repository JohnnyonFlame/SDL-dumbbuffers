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

#if SDL_VIDEO_DRIVER_KMSDRM

#include "SDL_log.h"

#include "SDL_kmsdrmvideo.h"
#include "SDL_kmsdrmdyn.h"
#include <errno.h>
#include <sys/mman.h>


int KMSDRM_Dumb_CreateDumbBuffers(_THIS, SDL_Window *window)
{
    int ret;
    Uint64 has_dumb;
    SDL_WindowData *windata = ((SDL_WindowData *)window->driverdata);
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayForWindow(window)->driverdata;
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);

    KMSDRM_DumbBuffer *buffer;
    struct drm_mode_create_dumb *req_create;
	struct drm_mode_map_dumb *req_map;
	struct drm_mode_destroy_dumb *req_destroy_dumb;

    if (viddata->drm_fd < 0)
        viddata->drm_fd = open(viddata->devpath, O_RDWR | O_CLOEXEC);

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "KMSDRM: Creating dumb buffers.");

    if (!(KMSDRM_drmGetCap(viddata->drm_fd, DRM_CAP_DUMB_BUFFER, &has_dumb) >= 0 && has_dumb)) {
        SDL_SetError("KMSDRM: KMSDRM implementation has no dumb buffer caps? (fd: %d)", viddata->drm_fd);
        return -1;
    }

    for (int i = 0; i < SDL_arraysize(windata->dumb_buffers); i++) {
        buffer = &windata->dumb_buffers[i];
        req_create = &buffer->req_create;
        req_map = &buffer->req_map;
        req_destroy_dumb = &buffer->req_destroy_dumb;

        req_create->width = dispdata->fullscreen_mode.hdisplay;
        req_create->height = dispdata->fullscreen_mode.vdisplay;
        req_create->bpp = 32; //!! HARDCODED SDL_PIXELFORMAT_ARGB8888

        if (KMSDRM_drmIoctl(viddata->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, req_create) < 0) {
            buffer->buf_id = -1;
            SDL_SetError("KMSDRM: Unable to create dumb buffer.");
            goto kmsdrm_fail_createfb;
        }

        req_destroy_dumb->handle = req_create->handle;
        if ((ret = KMSDRM_drmModeAddFB(viddata->drm_fd, req_create->width, req_create->height, 24, 32, req_create->pitch, req_create->handle, &buffer->buf_id))) {
            req_create->handle = -1;
            SDL_SetError("KMSDRM: Unable to create framebuffer: %d.", ret);
            goto kmsdrm_fail_createfb;
        }

        req_map->handle = req_create->handle;
        if (KMSDRM_drmIoctl(viddata->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, req_map) < 0) {
            SDL_SetError("KMSDRM: Map data request failed.");
            goto kmsdrm_fail_createfb;
        }

        buffer->map = mmap(0, req_create->size, PROT_READ | PROT_WRITE, MAP_SHARED, viddata->drm_fd, req_map->offset);
        if (buffer->map == MAP_FAILED) {
            SDL_SetError("KMSDRM: Failed to map framebuffer.");
            goto kmsdrm_fail_createfb;
        }
    }

    windata->back_buffer = 0;
    windata->front_buffer = 1;
    windata->set_crtc = SDL_TRUE;
    windata->waiting_for_flip = SDL_FALSE;
    viddata->dumb_init = SDL_TRUE;

    return 0;
kmsdrm_fail_createfb:
    for (int i = 0; i < SDL_arraysize(windata->dumb_buffers); i++) {
        buffer = &windata->dumb_buffers[i];
        req_create = &buffer->req_create;
        req_map = &buffer->req_map;
        req_destroy_dumb = &buffer->req_destroy_dumb;

        if (buffer->buf_id > 0)
            KMSDRM_drmModeRmFB(viddata->drm_fd, buffer->buf_id);

        if (req_create->handle > 0)
            KMSDRM_drmIoctl(viddata->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, req_destroy_dumb);

        if (buffer->map)
            KMSDRM_drmUnmap(buffer->map, req_create->size);
    }

    return -1;
}

int KMSDRM_Dumb_CreateWindowFramebuffer(_THIS, SDL_Window * window, Uint32 * format, void ** pixels, int *pitch)
{
    SDL_Surface *surf;
    SDL_WindowData *windata = ((SDL_WindowData *)window->driverdata);
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);

    const SDL_PixelFormatEnum fmt = SDL_PIXELFORMAT_ARGB8888;
    /* Not supported when using accelerated renderers. */
    if (viddata->opengl_mode)
        return SDL_SetError("Cannot mix dumb buffers with OpenGL.");

    KMSDRM_Dumb_DestroyWindowFramebuffer(_this, window);
    surf = SDL_CreateRGBSurfaceWithFormat(0, window->w, window->h, 32, fmt);
    if (!surf) {
        SDL_SetError("Unable to create window framebuffer.");
        return -1;
    }

    *format = fmt;
    *pixels = surf->pixels;
    *pitch = surf->pitch;
    windata->framebuffer = surf;

    return 0;
}

int KMSDRM_Dumb_UpdateWindowFramebuffer(_THIS, SDL_Window * window, const SDL_Rect * rects, int numrects)
{
    SDL_WindowData *windata = ((SDL_WindowData *)window->driverdata);
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayForWindow(window)->driverdata;
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);
    KMSDRM_DumbBuffer *buffer = &windata->dumb_buffers[windata->back_buffer];
    SDL_Surface *surf = windata->framebuffer;
    int swap, ret;
    SDL_bool set_crtc = windata->set_crtc;

    if (viddata->opengl_mode)
        return SDL_SetError("Cannot mix dumb buffers with OpenGL.");

    /* Recreate the GBM / EGL surfaces if the display mode has changed */
    if (windata->egl_surface_dirty) {
        set_crtc = SDL_TRUE;
        KMSDRM_CreateSurfaces(_this, window);
    }

    if (!viddata->dumb_init)
        return -1;

    if (!KMSDRM_WaitPageflip(_this, windata)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Wait for pageflip failed");
        /* fall-tru intentionally */
    }

    for (int i = 0; i < window->h; i++) {
        Uint32 *row_db = buffer->map + (i * buffer->req_create.pitch);
        Uint32 *row_fb = surf->pixels + (i * surf->pitch);
        SDL_memcpy(row_db, row_fb, window->w * surf->format->BytesPerPixel);
    }

    if (set_crtc) {
        ret = KMSDRM_drmModeSetCrtc(viddata->drm_fd, dispdata->crtc->crtc_id,
                                windata->dumb_buffers[windata->back_buffer].buf_id,
                                0, 0, &dispdata->connector->connector_id, 1, &dispdata->mode);
        windata->set_crtc = SDL_FALSE;
    } else {
        ret = KMSDRM_drmModePageFlip(viddata->drm_fd, dispdata->crtc->crtc_id,
                                windata->dumb_buffers[windata->back_buffer].buf_id,
                                DRM_MODE_PAGE_FLIP_EVENT, &windata->waiting_for_flip);
       if (ret == 0) {
           windata->waiting_for_flip = SDL_TRUE;
       }
    }

    swap = windata->back_buffer;
    windata->back_buffer = windata->front_buffer;
    windata->front_buffer = swap;
    return ret;
}

void KMSDRM_Dumb_DestroyWindowFramebuffer(_THIS, SDL_Window * window)
{
    SDL_WindowData *windata = ((SDL_WindowData *)window->driverdata);
    if (windata->framebuffer) {
        SDL_FreeSurface(windata->framebuffer);
        windata->framebuffer = NULL;
    }
}

void KMSDRM_Dumb_DestroySurfaces(_THIS, SDL_Window *window)
{
    SDL_WindowData *windata = ((SDL_WindowData *)window->driverdata);
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);
    KMSDRM_DumbBuffer *buffer;
    struct drm_mode_create_dumb *req_create;
	struct drm_mode_destroy_dumb *req_destroy_dumb;

    if (!viddata->dumb_init)
        return;

    for (int i = 0; i < SDL_arraysize(windata->dumb_buffers); i++) {
        buffer = &windata->dumb_buffers[i];
        req_create = &buffer->req_create;
        req_destroy_dumb = &buffer->req_destroy_dumb;
        
        if (buffer->buf_id > 0)
            KMSDRM_drmModeRmFB(viddata->drm_fd, buffer->buf_id);

        if (req_create->handle > 0)
            KMSDRM_drmIoctl(viddata->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, req_destroy_dumb);

        if (buffer->map)
            KMSDRM_drmUnmap(buffer->map, req_create->size);

        buffer->buf_id = -1;
        req_create->handle = -1;
        buffer->map = MAP_FAILED;
    }

    close(viddata->drm_fd);
    viddata->drm_fd = -1;
    viddata->dumb_init = SDL_FALSE;
}

#endif /* SDL_VIDEO_DRIVER_KMSDRM */

/* vi: set ts=4 sw=4 expandtab: */
