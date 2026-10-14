/*
 * Copyright 2023 Zhiyi Zhang for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#ifndef __WINE_DCOMP_PRIVATE_H
#define __WINE_DCOMP_PRIVATE_H

#include "dcomp.h"
#include "d2d1_1.h"
#include "wingdi.h"
#include "wine/list.h"

struct composition_device
{
    IDCompositionDevice IDCompositionDevice_iface;
    IDCompositionDesktopDevice IDCompositionDesktopDevice_iface;
    CRITICAL_SECTION cs;
    struct list targets;
    HANDLE thread;
    BOOL thread_exited;
    int version;
    LONG ref;
};

struct composition_target
{
    IDCompositionTarget IDCompositionTarget_iface;
    IDCompositionDevice *device;
    IDCompositionVisual *root;
    BOOL topmost;
    HWND hwnd;
    HWND surface;
    HDC surface_dc;
    HBITMAP surface_bitmap;
    HGDIOBJ surface_old_bitmap;
    void *surface_bits;
    void *surface_published_bits;
    SIZE_T surface_published_size;
    UINT surface_published_width;
    UINT surface_published_height;
    LONG surface_published_x;
    LONG surface_published_y;
    HWND surface_owner;
    UINT surface_width;
    UINT surface_height;
    LONG surface_x;
    LONG surface_y;
    LONG surface_frame_x;
    LONG surface_frame_y;
    BOOL surface_position_valid;
    BOOL surface_visible;
    BOOL surface_frame_bounds_valid;
    BOOL surface_frame_active;
    BOOL surface_frame_has_content;
    BOOL surface_published_valid;
    unsigned int source_pixel_trace_count;
    unsigned int surface_pixel_trace_count;
    struct list entry;
    LONG ref;
};

struct composition_visual
{
    IDCompositionVisual2 IDCompositionVisual2_iface;
    struct composition_device *device;
    ID2D1GdiInteropRenderTarget *interop;
    ID2D1DeviceContext *device_context;
    IUnknown *content;
    float offset_x;
    float offset_y;
    D2D_MATRIX_3X2_F transform;
    BOOL has_transform;
    struct list children;
    struct list entry;
    struct composition_visual *parent;
    BOOL is_root;
    int version;
    LONG ref;
};

static inline struct composition_device *impl_from_IDCompositionDevice(IDCompositionDevice *iface)
{
    return CONTAINING_RECORD(iface, struct composition_device, IDCompositionDevice_iface);
}

static inline struct composition_device *impl_from_IDCompositionDesktopDevice(IDCompositionDesktopDevice *iface)
{
    return CONTAINING_RECORD(iface, struct composition_device, IDCompositionDesktopDevice_iface);
}

static inline struct composition_target *impl_from_IDCompositionTarget(IDCompositionTarget *iface)
{
    return CONTAINING_RECORD(iface, struct composition_target, IDCompositionTarget_iface);
}

static inline struct composition_visual *impl_from_IDCompositionVisual(IDCompositionVisual *iface)
{
    return CONTAINING_RECORD(iface, struct composition_visual, IDCompositionVisual2_iface);
}

static inline struct composition_visual *impl_from_IDCompositionVisual2(IDCompositionVisual2 *iface)
{
    return CONTAINING_RECORD(iface, struct composition_visual, IDCompositionVisual2_iface);
}

HRESULT create_target(struct composition_device *device, HWND hwnd, BOOL topmost, IDCompositionTarget **target);
HRESULT create_visual(struct composition_device *device, int version, REFIID iid, void **visual);
BOOL target_update_surface(struct composition_target *target, HWND parent, const POINT *position,
        UINT width, UINT height, HDC source_dc, const void *source_bits, UINT source_pitch);
void target_begin_surface_frame(struct composition_target *target, HWND parent);
BOOL target_composite_surface(struct composition_target *target, HWND parent, const POINT *position,
        UINT width, UINT height, const void *source_bits, UINT source_pitch);
void target_present_surface_frame(struct composition_target *target, HWND parent);
void target_destroy_surface(struct composition_target *target);

#endif /* __WINE_DCOMP_PRIVATE_H */
