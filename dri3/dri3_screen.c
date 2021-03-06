/*
 * Copyright © 2013 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifdef HAVE_XORG_CONFIG_H
#include <xorg-config.h>
#endif

#include "dri3_priv.h"
#include <syncsdk.h>
#include <misync.h>
#include <misyncshm.h>
#include <randrstr.h>
#include <drm_fourcc.h>

static inline Bool has_open(dri3_screen_info_ptr info) {
    if (info == NULL)
        return FALSE;

    return info->open != NULL ||
        (info->version >= 1 && info->open_client != NULL);
}

int
dri3_open(ClientPtr client, ScreenPtr screen, RRProviderPtr provider, int *fd)
{
    dri3_screen_priv_ptr        ds = dri3_screen_priv(screen);
    dri3_screen_info_ptr        info = ds->info;
    int                         rc;

    if (!has_open(info))
        return BadMatch;

    if (info->version >= 1 && info->open_client != NULL)
        rc = (*info->open_client) (client, screen, provider, fd);
    else
        rc = (*info->open) (screen, provider, fd);

    if (rc != Success)
        return rc;

    return Success;
}

int
dri3_pixmap_from_fds(PixmapPtr *ppixmap, ScreenPtr screen,
                     CARD8 num_fds, int *fds,
                     CARD16 width, CARD16 height,
                     CARD32 *strides, CARD32 *offsets,
                     CARD8 depth, CARD8 bpp, CARD64 modifier)
{
    dri3_screen_priv_ptr        ds = dri3_screen_priv(screen);
    dri3_screen_info_ptr        info = ds->info;
    PixmapPtr                   pixmap;

    if (!info)
        return BadImplementation;

    if (info->version >= 2 && info->pixmap_from_fds != NULL) {
        pixmap = (*info->pixmap_from_fds) (screen, num_fds, fds, width, height,
                                           strides, offsets, depth, bpp, modifier);
    } else if (info->pixmap_from_fd != NULL && num_fds == 1 &&
               modifier == DRM_FORMAT_MOD_INVALID) {
        pixmap = (*info->pixmap_from_fd) (screen, fds[0], width, height,
                                          strides[0], depth, bpp);
    } else {
        return BadImplementation;
    }

    if (!pixmap)
        return BadAlloc;

    *ppixmap = pixmap;
    return Success;
}

int
dri3_fds_from_pixmap(PixmapPtr pixmap, int *fds,
                     CARD32 *strides, CARD32 *offsets,
                     CARD64 *modifier)
{
    ScreenPtr                   screen = pixmap->drawable.pScreen;
    dri3_screen_priv_ptr        ds = dri3_screen_priv(screen);
    dri3_screen_info_ptr        info = ds->info;

    if (!info)
        return 0;

    if (info->version >= 2 && info->fds_from_pixmap != NULL) {
        return (*info->fds_from_pixmap)(screen, pixmap, fds, strides, offsets,
                                        modifier);
    } else if (info->fd_from_pixmap != NULL) {
        CARD16 stride;
        CARD32 size;

        fds[0] = (*info->fd_from_pixmap)(screen, pixmap, &stride, &size);
        if (fds[0] < 0)
            return 0;

        strides[0] = stride;
        offsets[0] = 0;
        *modifier = DRM_FORMAT_MOD_INVALID;
        return 1;
    } else {
        return 0;
    }
}

static int
cache_formats_and_modifiers(ScreenPtr screen)
{
    dri3_screen_priv_ptr        ds = dri3_screen_priv(screen);
    dri3_screen_info_ptr        info = ds->info;
    CARD32                     *formats = NULL;
    CARD64                     *modifiers = NULL;
    int                         i;

    if (ds->formats_cached)
        return Success;

    if (!info)
        return BadImplementation;

    if (!info->get_formats || !info->get_modifiers) {
        ds->formats = NULL;
        ds->num_formats = 0;
        ds->formats_cached = TRUE;
        return Success;
    }

    (*info->get_formats) (screen, &ds->num_formats, &formats);
    ds->formats = calloc(ds->num_formats, sizeof(dri3_dmabuf_format_rec));
    if (!ds->formats)
        return BadAlloc;

    for (i = 0; i < ds->num_formats; i++) {
        dri3_dmabuf_format_ptr iter = &ds->formats[i];

        iter->format = formats[i];
        (*info->get_modifiers) (screen, formats[i],
                                &iter->num_modifiers,
                                &modifiers);

        iter->modifiers = malloc(iter->num_modifiers * sizeof(CARD64));
        if (iter->modifiers == NULL)
            goto error;

        memcpy(iter->modifiers, modifiers,
               iter->num_modifiers * sizeof(CARD64));
        goto done;

error:
        iter->num_modifiers = 0;
        free(iter->modifiers);
done:
        free(modifiers);
    }
    free(formats);
    ds->formats_cached = TRUE;

    return Success;
}

int
dri3_get_supported_modifiers(ScreenPtr screen, DrawablePtr drawable,
                             CARD8 depth, CARD8 bpp,
                             CARD32 *num_intersect_modifiers,
                             CARD64 **intersect_modifiers,
                             CARD32 *num_screen_modifiers,
                             CARD64 **screen_modifiers)
{
    dri3_screen_priv_ptr        ds = dri3_screen_priv(screen);
    dri3_screen_info_ptr        info = ds->info;
    int                         i, j;
    int                         ret;
    CARD32                      num_drawable_mods;
    CARD64                     *drawable_mods;
    CARD64                     *intersect_mods = NULL;
    CARD64                     *screen_mods = NULL;
    CARD32                      format;
    dri3_dmabuf_format_ptr      screen_format = NULL;

    ret = cache_formats_and_modifiers(screen);
    if (ret != Success)
        return ret;

    format = drm_format_for_depth(depth, bpp);
    if (format == 0)
        return BadValue;

    /* Find screen-global modifiers from cache
     */
    for (i = 0; i < ds->num_formats; i++) {
        if (ds->formats[i].format == format) {
            screen_format = &ds->formats[i];
            break;
        }
    }
    if (screen_format == NULL)
        return BadMatch;

    if (screen_format->num_modifiers == 0) {
        *num_screen_modifiers = 0;
        *num_intersect_modifiers = 0;
        return Success;
    }

    if (info->get_drawable_modifiers)
        (*info->get_drawable_modifiers) (drawable, format,
                                         &num_drawable_mods,
                                         &drawable_mods);

    /* We're allocating slightly more memory than necessary but it reduces
     * the complexity of finding the intersection set.
     */
    screen_mods = malloc(screen_format->num_modifiers * sizeof(CARD64));
    if (!screen_mods)
        return BadAlloc;
    if (num_drawable_mods > 0) {
        intersect_mods = malloc(screen_format->num_modifiers * sizeof(CARD64));
        if (!intersect_mods) {
            free(screen_mods);
            return BadAlloc;
        }
    }

    *num_screen_modifiers = 0;
    *num_intersect_modifiers = 0;
    for (i = 0; i < screen_format->num_modifiers; i++) {
        CARD64 modifier = screen_format->modifiers[i];
        Bool intersect = FALSE;

        for (j = 0; j < num_drawable_mods; j++) {
            if (drawable_mods[j] == modifier) {
                intersect = TRUE;
                break;
            }
        }

        if (intersect) {
            intersect_mods[*num_intersect_modifiers] = modifier;
            *num_intersect_modifiers += 1;
        } else {
            screen_mods[*num_screen_modifiers] = modifier;
            *num_screen_modifiers += 1;
        }
    }

    assert(*num_intersect_modifiers + *num_screen_modifiers == screen_format->num_modifiers);

    *intersect_modifiers = intersect_mods;
    *screen_modifiers = screen_mods;
    free(drawable_mods);

    return Success;
}
