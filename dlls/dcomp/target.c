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

#include <stdarg.h>
#include <string.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "dcomp_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static void target_release_surface_bitmap(struct composition_target *target)
{
    if (target->surface_dc && target->surface_old_bitmap)
    {
        SelectObject(target->surface_dc, target->surface_old_bitmap);
        target->surface_old_bitmap = NULL;
    }
    if (target->surface_bitmap)
    {
        DeleteObject(target->surface_bitmap);
        target->surface_bitmap = NULL;
    }
    if (target->surface_dc)
    {
        DeleteDC(target->surface_dc);
        target->surface_dc = NULL;
    }
    target->surface_width = 0;
    target->surface_height = 0;
    target->surface_bits = NULL;
    target->surface_position_valid = FALSE;
    target->surface_visible = FALSE;
    target->surface_frame_bounds_valid = FALSE;
    target->surface_frame_active = FALSE;
    target->surface_frame_has_content = FALSE;
    target->surface_published_valid = FALSE;
    target->frame_geometry_valid = FALSE;
}

void target_destroy_surface(struct composition_target *target)
{
    target_release_surface_bitmap(target);
    if (target->surface_published_bits)
    {
        HeapFree(GetProcessHeap(), 0, target->surface_published_bits);
        target->surface_published_bits = NULL;
        target->surface_published_size = 0;
    }
    if (target->surface)
    {
        DestroyWindow(target->surface);
        target->surface = NULL;
    }
}

static BOOL target_create_surface_bitmap(struct composition_target *target, UINT width, UINT height)
{
    BITMAPINFO bitmap_info = {0};
    void *bits;

    target->surface_dc = CreateCompatibleDC(NULL);
    bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -(LONG)height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    target->surface_bitmap = CreateDIBSection(target->surface_dc, &bitmap_info, DIB_RGB_COLORS,
            &bits, NULL, 0);
    if (!target->surface_dc || !target->surface_bitmap)
    {
        WARN("Failed to create composition surface bitmap, error %lu.\n",
                GetLastError());
        target_release_surface_bitmap(target);
        return FALSE;
    }
    target->surface_bits = bits;
    memset(bits, 0, (size_t)width * height * 4);

    target->surface_old_bitmap = SelectObject(target->surface_dc, target->surface_bitmap);
    if (!target->surface_old_bitmap)
    {
        WARN("Failed to select composition surface bitmap, error %lu.\n", GetLastError());
        target_release_surface_bitmap(target);
        return FALSE;
    }

    target->surface_width = width;
    target->surface_height = height;

    return TRUE;
}

static BOOL target_create_surface(struct composition_target *target, HWND parent, UINT width, UINT height)
{
    /* This is a visual-only surface. Pass pointer hit testing through so the
     * WebView child beneath it continues to receive clicks and hover events. */
    target->surface = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT
            | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"static", NULL,
            WS_CHILD, 0, 0, width, height, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!target->surface)
    {
        WARN("Failed to create composition surface, error %lu.\n", GetLastError());
        return FALSE;
    }

    if (!target_create_surface_bitmap(target, width, height))
    {
        target_destroy_surface(target);
        return FALSE;
    }

    target->surface_owner = parent;

    return TRUE;
}

BOOL target_update_surface(struct composition_target *target, HWND parent, const POINT *position,
        UINT width, UINT height, HDC source_dc, const void *source_bits, UINT source_pitch)
{
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HWND root;
    POINT root_position;
    BOOL visible;
    BOOL geometry_changed;
    POINT source = {0, 0};
    SIZE size = {(LONG)width, (LONG)height};
    static unsigned int state_trace_count;
    static unsigned int publish_trace_count;

    if (!parent || !position || !width || !height || (!source_dc && !source_bits))
        return FALSE;

    root = GetAncestor(parent, GA_ROOT);
    if (!root || !IsWindow(root))
        root = parent;
    if (!IsWindow(parent))
    {
        WARN("Composition host %p is no longer a window, root %p.\n", parent, root);
        return FALSE;
    }

    root_position = *position;
    MapWindowPoints(parent, root, &root_position, 1);
    target->surface_frame_x = root_position.x;
    target->surface_frame_y = root_position.y;
    target->surface_frame_bounds_valid = TRUE;
    visible = IsWindowVisible(parent);
    if (state_trace_count < 256)
    {
        TRACE("composition surface host %p root %p valid %d visible %d.\n", parent, root,
                !!root, visible);
        ++state_trace_count;
    }

    /* A hidden destination must not leave a visible replacement surface behind.
     * This matters for browser pages that keep their old target window alive
     * while switching to a new page. */
    if (!visible)
    {
        if (target->surface)
        {
            ShowWindow(target->surface, SW_HIDE);
            target->surface_position_valid = FALSE;
            target->surface_visible = FALSE;
        }
        return TRUE;
    }

    if (!target->surface || target->surface_owner != root)
        target_destroy_surface(target);
    if (!target->surface && !target_create_surface(target, root, width, height))
        return FALSE;

    if (target->surface_width != width || target->surface_height != height)
    {
        target_release_surface_bitmap(target);
        if (!target_create_surface_bitmap(target, width, height))
            return FALSE;
        TRACE("Resized composition surface %p bitmap to %ux%u.\n",
                target->surface, width, height);
    }

    if (source_bits)
    {
        const BYTE *src = source_bits;
        BYTE *dst = target->surface_bits;
        UINT y;

        if (source_pitch < width * 4)
        {
            WARN("Invalid composition surface pitch %u for width %u.\n", source_pitch, width);
            return FALSE;
        }
        for (y = 0; y < height; ++y)
            memcpy(dst + (size_t)y * width * 4, src + (size_t)y * source_pitch, (size_t)width * 4);
    }
    else if (!BitBlt(target->surface_dc, 0, 0, width, height, source_dc, 0, 0, SRCCOPY))
    {
        WARN("Failed to copy composition surface, error %lu.\n", GetLastError());
        return FALSE;
    }

    if (TRACE_ON(dcomp) && width == 1258 && height >= 650 && target->surface_bits
            && target->surface_pixel_trace_count < 16)
    {
        POINT points[4] = {{0, 0}, {(LONG)width / 2, (LONG)height / 2},
                {(LONG)width / 4, (LONG)height / 4}, {(LONG)width - 1, (LONG)height - 1}};
        unsigned int zero_alpha = 0, partial_alpha = 0, opaque_alpha = 0;
        unsigned int colored = 0, nonzero_alpha_colored = 0;
        unsigned int max_color = 0;
        size_t pixel_count = (size_t)width * height;
        const BYTE *pixels = target->surface_bits;
        unsigned int i;

        for (i = 0; i < pixel_count; ++i)
        {
            const BYTE *pixel = pixels + i * 4;
            unsigned int color = max(pixel[0], max(pixel[1], pixel[2]));

            if (!pixel[3])
                ++zero_alpha;
            else if (pixel[3] == 255)
                ++opaque_alpha;
            else
                ++partial_alpha;
            if (color)
                ++colored;
            if (pixel[3] && color)
                ++nonzero_alpha_colored;
            if (color > max_color)
                max_color = color;
        }

        TRACE("surface stats target %p size %ux%u alpha zero %u partial %u opaque %u "
                "colored %u nonzero_alpha_colored %u max_color %u first BGRA %02x %02x %02x %02x.\n",
                target->hwnd, width, height, zero_alpha, partial_alpha, opaque_alpha,
                colored, nonzero_alpha_colored, max_color,
                pixels[0], pixels[1], pixels[2], pixels[3]);
        for (i = 0; i < ARRAY_SIZE(points); ++i)
        {
            const BYTE *pixel = (const BYTE *)target->surface_bits
                    + ((size_t)points[i].y * width + points[i].x) * 4;
            TRACE("surface pixel target %p point %ld,%ld BGRA %02x %02x %02x %02x GetPixel %#lx.\n",
                    target->hwnd, points[i].x, points[i].y,
                    pixel[0], pixel[1], pixel[2], pixel[3],
                    (unsigned long)GetPixel(target->surface_dc, points[i].x, points[i].y));
        }
        ++target->surface_pixel_trace_count;
    }

    geometry_changed = !target->surface_position_valid || !target->surface_visible
            || target->surface_x != root_position.x || target->surface_y != root_position.y
            || target->surface_width != width || target->surface_height != height
            || !IsWindowVisible(target->surface);

    if (!UpdateLayeredWindow(target->surface, NULL, NULL, &size,
            target->surface_dc, &source,
            0, &blend, ULW_ALPHA))
    {
        WARN("Failed to publish composition surface, error %lu.\n", GetLastError());
        return FALSE;
    }
    target->surface_frame_has_content = TRUE;

    if (geometry_changed)
    {
        if (!SetWindowPos(target->surface, HWND_TOP, root_position.x, root_position.y, width, height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW))
        {
            WARN("Failed to position composition surface, error %lu.\n", GetLastError());
            return FALSE;
        }
        target->surface_x = root_position.x;
        target->surface_y = root_position.y;
        target->surface_position_valid = TRUE;
        target->surface_visible = TRUE;
    }
    if (publish_trace_count < 256)
    {
        TRACE("composition surface target %p surface %p host %p root %p published position %ld,%ld size %ux%u visible %d.\n",
                target->hwnd, target->surface, parent, root, root_position.x, root_position.y,
                width, height, visible);
        ++publish_trace_count;
    }
    return TRUE;
}

static BOOL target_resize_surface_bitmap(struct composition_target *target, LONG left, LONG top,
        UINT width, UINT height, BOOL preserve)
{
    BITMAPINFO bitmap_info = {0};
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old_bitmap;
    void *bits;
    UINT y;

    dc = CreateCompatibleDC(NULL);
    if (!dc)
        return FALSE;

    bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -(LONG)height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(dc, &bitmap_info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bitmap)
    {
        DeleteDC(dc);
        return FALSE;
    }
    old_bitmap = SelectObject(dc, bitmap);
    if (!old_bitmap || old_bitmap == HGDI_ERROR)
    {
        DeleteObject(bitmap);
        DeleteDC(dc);
        return FALSE;
    }

    memset(bits, 0, (size_t)width * height * 4);
    if (preserve && target->surface_bits)
    {
        LONG x_offset = target->surface_frame_x - left;
        LONG y_offset = target->surface_frame_y - top;

        for (y = 0; y < target->surface_height; ++y)
            memcpy((BYTE *)bits + ((size_t)(y_offset + y) * width + x_offset) * 4,
                    (BYTE *)target->surface_bits + (size_t)y * target->surface_width * 4,
                    (size_t)target->surface_width * 4);
    }

    if (target->surface_dc && target->surface_old_bitmap)
        SelectObject(target->surface_dc, target->surface_old_bitmap);
    if (target->surface_bitmap)
        DeleteObject(target->surface_bitmap);
    if (target->surface_dc)
        DeleteDC(target->surface_dc);

    target->surface_dc = dc;
    target->surface_bitmap = bitmap;
    target->surface_old_bitmap = old_bitmap;
    target->surface_bits = bits;
    target->surface_width = width;
    target->surface_height = height;
    target->surface_frame_x = left;
    target->surface_frame_y = top;
    target->surface_frame_bounds_valid = TRUE;
    target->surface_position_valid = FALSE;
    target->surface_published_valid = FALSE;
    return TRUE;
}

void target_begin_surface_frame(struct composition_target *target, HWND parent)
{
    HWND root;

    target->surface_frame_active = FALSE;
    target->surface_frame_has_content = FALSE;
    if (!parent || !IsWindow(parent))
        return;

    root = GetAncestor(parent, GA_ROOT);
    if (!root || !IsWindow(root))
        root = parent;
    if (target->surface && target->surface_owner != root)
        target_destroy_surface(target);

    if (!IsWindowVisible(parent))
    {
        if (target->surface)
        {
            ShowWindow(target->surface, SW_HIDE);
            target->surface_position_valid = FALSE;
            target->surface_visible = FALSE;
        }
        return;
    }

    if (target->surface_bits)
        memset(target->surface_bits, 0, (size_t)target->surface_width * target->surface_height * 4);
    target->surface_frame_active = TRUE;
}

BOOL target_composite_surface(struct composition_target *target, HWND parent, const POINT *position,
        UINT width, UINT height, const void *source_bits, UINT source_pitch)
{
    HWND root;
    POINT root_position;
    LONG left, top, right, bottom;
    UINT x, y;

    if (!target->surface_frame_active || !parent || !position || !width || !height || !source_bits)
        return FALSE;
    if (source_pitch < (size_t)width * 4)
    {
        WARN("Invalid composition source pitch %u for width %u.\n", source_pitch, width);
        return FALSE;
    }

    root = GetAncestor(parent, GA_ROOT);
    if (!root || !IsWindow(root))
        root = parent;
    if (target->surface && target->surface_owner != root)
        target_destroy_surface(target);

    root_position = *position;
    MapWindowPoints(parent, root, &root_position, 1);
    if (!target->surface)
    {
        if (!target_create_surface(target, root, width, height))
            return FALSE;
        target->surface_frame_x = root_position.x;
        target->surface_frame_y = root_position.y;
        target->surface_frame_bounds_valid = TRUE;
    }
    else if (!target->surface_frame_bounds_valid)
    {
        target->surface_frame_x = root_position.x;
        target->surface_frame_y = root_position.y;
        target->surface_frame_bounds_valid = TRUE;
    }

    left = min(target->surface_frame_x, root_position.x);
    top = min(target->surface_frame_y, root_position.y);
    right = max(target->surface_frame_x + (LONG)target->surface_width,
            root_position.x + (LONG)width);
    bottom = max(target->surface_frame_y + (LONG)target->surface_height,
            root_position.y + (LONG)height);
    if (left != target->surface_frame_x || top != target->surface_frame_y
            || right - left != (LONG)target->surface_width
            || bottom - top != (LONG)target->surface_height)
    {
        if (!target_resize_surface_bitmap(target, left, top, right - left, bottom - top,
                target->surface_frame_has_content))
        {
            WARN("Failed to resize composition frame to %ld,%ld-%ld,%ld.\n", left, top,
                    right, bottom);
            return FALSE;
        }
    }

    for (y = 0; y < height; ++y)
    {
        const BYTE *src = (const BYTE *)source_bits + (size_t)y * source_pitch;
        BYTE *dst = (BYTE *)target->surface_bits
                + ((size_t)(root_position.y - target->surface_frame_y + y) * target->surface_width
                + root_position.x - target->surface_frame_x) * 4;

        for (x = 0; x < width; ++x, src += 4, dst += 4)
        {
            unsigned int alpha = src[3], inverse_alpha = 255 - alpha;
            unsigned int channel;

            if (!alpha)
                continue;
            if (alpha == 255)
            {
                memcpy(dst, src, 4);
                continue;
            }
            for (channel = 0; channel < 4; ++channel)
            {
                unsigned int value = src[channel] + (dst[channel] * inverse_alpha + 127) / 255;
                dst[channel] = min(value, 255);
            }
        }
    }

    target->surface_frame_has_content = TRUE;
    return TRUE;
}

BOOL target_present_surface_frame(struct composition_target *target, HWND parent)
{
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    POINT source = {0, 0};
    POINT position = {target->surface_frame_x, target->surface_frame_y};
    SIZE size = {(LONG)target->surface_width, (LONG)target->surface_height};
    BOOL geometry_changed;
    BOOL frame_changed;
    SIZE_T frame_size = (SIZE_T)target->surface_width * target->surface_height * 4;

    if (!target->surface_frame_active)
        return FALSE;
    target->surface_frame_active = FALSE;
    if (!parent || !IsWindow(parent))
        return FALSE;
    if (!IsWindowVisible(parent))
    {
        if (target->surface)
        {
            ShowWindow(target->surface, SW_HIDE);
            target->surface_position_valid = FALSE;
            target->surface_visible = FALSE;
        }
        return FALSE;
    }
    if (!target->surface_frame_has_content || !target->surface || !target->surface_bits)
        return FALSE;

    geometry_changed = !target->surface_position_valid || !target->surface_visible
            || target->surface_x != position.x || target->surface_y != position.y
            || !IsWindowVisible(target->surface);
    frame_changed = !target->surface_published_valid
            || target->surface_published_width != target->surface_width
            || target->surface_published_height != target->surface_height
            || target->surface_published_x != position.x
            || target->surface_published_y != position.y
            || !target->surface_published_bits
            || target->surface_published_size < frame_size
            || memcmp(target->surface_published_bits, target->surface_bits, frame_size);
    if (!frame_changed && !geometry_changed)
        return TRUE;

    if (!UpdateLayeredWindow(target->surface, NULL, NULL, &size, target->surface_dc,
            &source, 0, &blend, ULW_ALPHA))
    {
        WARN("Failed to publish composition frame, error %lu.\n", GetLastError());
        return FALSE;
    }

    if (!target->surface_published_bits || target->surface_published_size < frame_size)
    {
        void *published_bits = HeapAlloc(GetProcessHeap(), 0, frame_size);

        if (published_bits)
        {
            if (target->surface_published_bits)
                HeapFree(GetProcessHeap(), 0, target->surface_published_bits);
            target->surface_published_bits = published_bits;
            target->surface_published_size = frame_size;
        }
    }
    if (target->surface_published_bits && target->surface_published_size >= frame_size)
    {
        memcpy(target->surface_published_bits, target->surface_bits, frame_size);
        target->surface_published_width = target->surface_width;
        target->surface_published_height = target->surface_height;
        target->surface_published_x = position.x;
        target->surface_published_y = position.y;
        target->surface_published_valid = TRUE;
    }
    else
        target->surface_published_valid = FALSE;

    if (geometry_changed)
    {
        if (!SetWindowPos(target->surface, HWND_TOP, position.x, position.y, size.cx, size.cy,
                SWP_NOACTIVATE | SWP_SHOWWINDOW))
        {
            WARN("Failed to position composition frame, error %lu.\n", GetLastError());
            return FALSE;
        }
        target->surface_x = position.x;
        target->surface_y = position.y;
        target->surface_position_valid = TRUE;
        target->surface_visible = TRUE;
    }
    return TRUE;
}

static const WCHAR *wine_window_topmost_composed = L"wine_window_topmost_composed";
static const WCHAR *wine_window_non_topmost_composed = L"wine_window_non_topmost_composed";

static HRESULT STDMETHODCALLTYPE target_QueryInterface(IDCompositionTarget *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionTarget))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE target_AddRef(IDCompositionTarget *iface)
{
    struct composition_target *target = impl_from_IDCompositionTarget(iface);
    ULONG ref = InterlockedIncrement(&target->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE target_Release(IDCompositionTarget *iface)
{
    struct composition_target *target = impl_from_IDCompositionTarget(iface);
    ULONG ref = InterlockedDecrement(&target->ref);
    struct composition_device *device;
    struct composition_visual *root_visual;
    IDCompositionVisual *root;
    const WCHAR *prop;

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        device = impl_from_IDCompositionDevice(target->device);
        root = NULL;
        root_visual = NULL;

        prop = target->topmost ? wine_window_topmost_composed : wine_window_non_topmost_composed;
        RemovePropW(target->hwnd, prop);

        EnterCriticalSection(&device->cs);
        list_remove(&target->entry);
        if (target->root)
        {
            root = target->root;
            root_visual = impl_from_IDCompositionVisual(root);
            root_visual->is_root = FALSE;
            target->root = NULL;
        }
        LeaveCriticalSection(&device->cs);
        IDCompositionDevice_Release(target->device);
        target_destroy_surface(target);
        if (root)
            IDCompositionVisual_Release(root);
        free(target);
    }

    return ref;
}

static HRESULT STDMETHODCALLTYPE target_SetRoot(IDCompositionTarget *iface,
        IDCompositionVisual *visual)
{
    struct composition_target *target = impl_from_IDCompositionTarget(iface);
    struct composition_device *device = impl_from_IDCompositionDevice(target->device);
    struct composition_visual *composition_visual;
    IDCompositionVisual *old_root;

    TRACE("iface %p, visual %p\n", iface, visual);

    old_root = NULL;
    EnterCriticalSection(&device->cs);
    if (visual)
    {
        composition_visual = impl_from_IDCompositionVisual(visual);
        if (composition_visual->is_root)
        {
            LeaveCriticalSection(&device->cs);
            return E_INVALIDARG;
        }

        composition_visual->is_root = TRUE;
        IDCompositionVisual_AddRef(visual);
    }

    if (target->root)
    {
        composition_visual = impl_from_IDCompositionVisual(target->root);
        composition_visual->is_root = FALSE;
        old_root = target->root;
    }
    target->root = visual;
    LeaveCriticalSection(&device->cs);

    if (old_root)
        IDCompositionVisual_Release(old_root);
    return S_OK;
}

static const struct IDCompositionTargetVtbl target_vtbl =
{
    /* IUnknown methods */
    target_QueryInterface,
    target_AddRef,
    target_Release,
    /* IDCompositionTarget methods */
    target_SetRoot,
};

HRESULT create_target(struct composition_device *device, HWND hwnd, BOOL topmost,
        IDCompositionTarget **new_target)
{
    struct composition_target *target;
    const WCHAR *prop;
    WCHAR class_name[64], title[128];
    RECT rect;
    DWORD pid = 0;

    if (!hwnd || hwnd == GetDesktopWindow() || !new_target)
        return E_INVALIDARG;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return E_ACCESSDENIED;

    class_name[0] = 0;
    title[0] = 0;
    rect.left = rect.top = rect.right = rect.bottom = 0;
    GetClassNameW(hwnd, class_name, ARRAY_SIZE(class_name));
    GetWindowTextW(hwnd, title, ARRAY_SIZE(title));
    GetWindowRect(hwnd, &rect);
    TRACE("target window %p class %s title %s parent %p root %p rect %ld,%ld-%ld,%ld "
            "style %#lx exstyle %#lx visible %d.\n", hwnd, debugstr_w(class_name),
            debugstr_w(title), GetParent(hwnd), GetAncestor(hwnd, GA_ROOT), rect.left,
            rect.top, rect.right, rect.bottom, (unsigned long)GetWindowLongW(hwnd, GWL_STYLE),
            (unsigned long)GetWindowLongW(hwnd, GWL_EXSTYLE), IsWindowVisible(hwnd));

    if ((topmost && GetPropW(hwnd, wine_window_topmost_composed))
            || (!topmost && GetPropW(hwnd, wine_window_non_topmost_composed)))
        return DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED;

    target = calloc(1, sizeof(*target));
    if (!target)
        return E_OUTOFMEMORY;

    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    EnterCriticalSection(&device->cs);
    list_add_tail(&device->targets, &target->entry);
    LeaveCriticalSection(&device->cs);
    target->IDCompositionTarget_iface.lpVtbl = &target_vtbl;
    target->ref = 1;
    target->hwnd = hwnd;
    target->topmost = topmost;
    target->device = &device->IDCompositionDevice_iface;
    *new_target = &target->IDCompositionTarget_iface;

    prop = target->topmost ? wine_window_topmost_composed : wine_window_non_topmost_composed;
    SetPropW(target->hwnd, prop, (HANDLE)1);
    return S_OK;
}
