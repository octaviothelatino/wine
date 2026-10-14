/*
 * Copyright 2020 Nikolay Sivov for CodeWeavers
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
#include <stdlib.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winternl.h"
#include "initguid.h"
#include "objidl.h"
#include "dcomp_private.h"
#include "dxgi.h"
#include "d3d11.h"
#include "dwmapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static const GUID composition_frame_key =
        {0x47e50524, 0xcf38, 0x41f9, {0xb0, 0x78, 0x91, 0x27, 0x48, 0x06, 0x4b, 0x91}};

static HRESULT STDMETHODCALLTYPE device_QueryInterface(IDCompositionDevice *iface,
        REFIID iid, void **out)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, iid %s, out %p\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionDevice))
    {
        IUnknown_AddRef(&device->IDCompositionDevice_iface);
        *out = &device->IDCompositionDevice_iface;
        return S_OK;
    }
    else if (device->version >= 2
            && (IsEqualGUID(iid, &IID_IDCompositionDevice2)
            || IsEqualGUID(iid, &IID_IDCompositionDesktopDevice)))
    {
        IUnknown_AddRef(&device->IDCompositionDesktopDevice_iface);
        *out = &device->IDCompositionDesktopDevice_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE device_AddRef(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    ULONG ref = InterlockedIncrement(&device->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE device_Release(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    ULONG ref = InterlockedDecrement(&device->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        if (device->thread)
        {
            WaitForSingleObject(device->thread, INFINITE);
            CloseHandle(device->thread);
        }
        DeleteCriticalSection(&device->cs);
        free(device);
    }

    return ref;
}

static HWND get_composition_destination(HWND hwnd, POINT *offset)
{
    HWND parent;

    offset->x = 0;
    offset->y = 0;

    /* Chromium creates the DirectComposition target as a hidden intermediate
     * child window. The browser content window is its parent and is the
     * drawable that Wine exposes in the host window hierarchy. */
    parent = GetParent(hwnd);
    if ((GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD) && parent)
    {
        /* Preserve the composition target's origin in its immediate host.
         * target_update_surface moves the surface to the common top-level
         * parent so it can stack above sibling render windows. */
        MapWindowPoints(hwnd, parent, offset, 1);
        return parent;
    }

    return hwnd;
}

static void trace_swapchain_buffer(struct composition_visual *visual,
        IDXGISwapChain1 *swapchain, ID2D1Bitmap1 *target_bitmap, UINT buffer_idx,
        D2D1_SIZE_U size)
{
    ID2D1Bitmap *probe_bitmap = NULL;
    IDXGISurface1 *probe_surface = NULL;
    ID2D1Image *old_target = NULL;
    HDC probe_dc;
    HRESULT hr;
    POINT points[4] = {{0, 0}, {(LONG)size.width / 2, (LONG)size.height / 2},
            {(LONG)size.width / 4, (LONG)size.height / 4},
            {(LONG)size.width - 1, (LONG)size.height - 1}};
    unsigned int i;

    hr = IDXGISwapChain1_GetBuffer(swapchain, buffer_idx, &IID_IDXGISurface1,
            (void **)&probe_surface);
    TRACE("swapchain %p probe GetBuffer index %u hr %#lx surface %p.\n",
            swapchain, buffer_idx, hr, probe_surface);
    if (FAILED(hr))
        goto done;

    hr = ID2D1DeviceContext_CreateSharedBitmap(visual->device_context, &IID_IDXGISurface,
            probe_surface, NULL, &probe_bitmap);
    TRACE("swapchain %p probe buffer %u CreateSharedBitmap hr %#lx bitmap %p.\n",
            swapchain, buffer_idx, hr, probe_bitmap);
    if (FAILED(hr))
        goto done;

    hr = ID2D1Bitmap1_CopyFromBitmap(target_bitmap, NULL, probe_bitmap, NULL);
    TRACE("swapchain %p probe buffer %u CopyFromBitmap hr %#lx.\n",
            swapchain, buffer_idx, hr);
    if (FAILED(hr))
        goto done;

    ID2D1DeviceContext_GetTarget(visual->device_context, &old_target);
    ID2D1DeviceContext_SetTarget(visual->device_context, (ID2D1Image *)target_bitmap);
    hr = ID2D1GdiInteropRenderTarget_GetDC(visual->interop, D2D1_DC_INITIALIZE_MODE_COPY,
            &probe_dc);
    TRACE("swapchain %p probe buffer %u GetDC hr %#lx dc %p.\n",
            swapchain, buffer_idx, hr, probe_dc);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(points); ++i)
            TRACE("source probe swapchain %p buffer %u point %ld,%ld GetPixel %#lx.\n",
                    swapchain, buffer_idx, points[i].x, points[i].y,
                    (unsigned long)GetPixel(probe_dc, points[i].x, points[i].y));
        ID2D1GdiInteropRenderTarget_ReleaseDC(visual->interop, NULL);
    }

    ID2D1DeviceContext_SetTarget(visual->device_context, old_target);
    if (old_target)
        ID2D1Image_Release(old_target);

done:
    if (probe_bitmap)
        ID2D1Bitmap_Release(probe_bitmap);
    if (probe_surface)
        IDXGISurface1_Release(probe_surface);
}

static BOOL composite_frame(struct composition_target *target, HWND destination, POINT offset,
        float visual_offset_x, float visual_offset_y,
        IDXGISurface1 *surface)
{
    ID3D11Texture2D *texture = NULL, *staging = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    BOOL mapped_valid = FALSE, copied = FALSE;
    HRESULT hr;

    hr = IDXGISurface1_QueryInterface(surface, &IID_ID3D11Texture2D, (void **)&texture);
    if (FAILED(hr)) goto done;
    ID3D11Texture2D_GetDesc(texture, &desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) goto done;
    ID3D11Texture2D_GetDevice(texture, &device);
    ID3D11Device_GetImmediateContext(device, &context);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    if (FAILED(hr)) goto done;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)texture);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) goto done;
    mapped_valid = TRUE;
    offset.x += (LONG)visual_offset_x;
    offset.y += (LONG)visual_offset_y;
    copied = target_composite_surface(target, destination, &offset, desc.Width, desc.Height,
            mapped.pData, mapped.RowPitch);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    mapped_valid = FALSE;
done:
    if (mapped_valid)
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    if (staging) ID3D11Texture2D_Release(staging);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    if (texture) ID3D11Texture2D_Release(texture);
    return copied;
}

static void do_composite(struct composition_target *target, struct composition_visual *visual,
        float visual_offset_x, float visual_offset_y)
{
    D2D1_BITMAP_PROPERTIES1 bitmap_desc;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
    ID2D1Bitmap1 *target_bitmap = NULL;
    IDXGISwapChain1 *swapchain = NULL;
    IDXGISwapChain3 *swapchain3 = NULL;
    ID2D1Bitmap *src_bitmap = NULL;
    IDXGISurface1 *surface = NULL;
    ID2D1Image *old_target = NULL;
    HDC src_dc;
    D2D1_SIZE_U size;
    HWND destination;
    POINT offset;
    UINT source_buffer_idx;
    UINT current_buffer_idx;
    UINT present_count = 0;
    BOOL composition_swapchain;
    BOOL copied;
    HRESULT hr;
    static unsigned int trace_count;

    bitmap_desc.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bitmap_desc.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bitmap_desc.dpiX = 0;
    bitmap_desc.dpiY = 0;
    bitmap_desc.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE;
    bitmap_desc.colorContext = NULL;
    if (!visual->content || !visual->device_context || !visual->interop)
        goto done;

    destination = get_composition_destination(target->hwnd, &offset);

    if (FAILED(hr = IUnknown_QueryInterface(visual->content, &IID_IDXGISwapChain1,
            (void **)&swapchain)))
    {
        FIXME("Only IDXGISwapChain1 content is currently supported, hr %#lx.\n", hr);
        goto done;
    }

    if (FAILED(hr = IDXGISwapChain1_GetDesc1(swapchain, &swapchain_desc)))
    {
        ERR("Failed to get the swapchain description, hr %#lx.\n", hr);
        goto done;
    }

    composition_swapchain = IDXGISwapChain1_GetHwnd(swapchain, &destination)
            == DXGI_ERROR_INVALID_CALL;

    if (swapchain_desc.Width == 1258 && swapchain_desc.Height >= 700
            && target->source_pixel_trace_count < 16)
    {
        TRACE("swapchain %p desc size %ux%u buffers %u format %#x effect %#x alpha %#x flags %#x.\n",
                swapchain, swapchain_desc.Width, swapchain_desc.Height,
                swapchain_desc.BufferCount, swapchain_desc.Format,
                swapchain_desc.SwapEffect, swapchain_desc.AlphaMode, swapchain_desc.Flags);
    }

    if (FAILED(IUnknown_QueryInterface((IUnknown *)swapchain, &IID_IDXGISwapChain3,
            (void **)&swapchain3)))
        swapchain3 = NULL;

    current_buffer_idx = swapchain3
            ? IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain3) : 0;
    if (swapchain_desc.BufferCount)
        current_buffer_idx %= swapchain_desc.BufferCount;

    IDXGISwapChain1_GetLastPresentCount(swapchain, &present_count);
    source_buffer_idx = composition_swapchain ? current_buffer_idx
            : present_count && swapchain_desc.BufferCount
            ? (current_buffer_idx + swapchain_desc.BufferCount - 1) % swapchain_desc.BufferCount
            : current_buffer_idx;
    if (swapchain3 && swapchain_desc.Width == 1258 && swapchain_desc.Height >= 700
            && target->source_pixel_trace_count < 16)
    {
        TRACE("swapchain %p current back buffer index %u present count %u, source buffer index %u.\n",
                swapchain, current_buffer_idx, present_count, source_buffer_idx);
    }
    size.width = swapchain_desc.Width;
    size.height = swapchain_desc.Height;

    if (FAILED(hr = ID2D1DeviceContext_CreateBitmap(visual->device_context, size, NULL, 0,
            &bitmap_desc, &target_bitmap)))
    {
        ERR("Failed to create a bitmap, hr %#lx.\n", hr);
        goto done;
    }

    if (swapchain_desc.Width == 1258 && swapchain_desc.Height >= 700
            && swapchain_desc.BufferCount > 1 && target->source_pixel_trace_count < 16)
    {
        unsigned int i;

        for (i = 0; i < swapchain_desc.BufferCount; ++i)
            trace_swapchain_buffer(visual, swapchain, target_bitmap, i, size);
    }

    {
        UINT surface_size = sizeof(surface);

        hr = IDXGISwapChain1_GetPrivateData(swapchain, &composition_frame_key, &surface_size, &surface);
        if (SUCCEEDED(hr) && surface)
        {
            composite_frame(target, destination, offset, visual_offset_x,
                    visual_offset_y, surface);
            goto done;
        }
    }
    if (hr == DXGI_ERROR_NOT_FOUND)
    {
        hr = IDXGISwapChain1_GetBuffer(swapchain, source_buffer_idx, &IID_IDXGISurface1,
                (void **)&surface);
        if (SUCCEEDED(hr) && surface)
        {
            composite_frame(target, destination, offset, visual_offset_x,
                    visual_offset_y, surface);
            goto done;
        }
    }
    TRACE("swapchain %p GetBuffer index %u hr %#lx surface %p.\n",
            swapchain, source_buffer_idx, hr, surface);
    if (FAILED(hr))
    {
        ERR("Failed to get the swapchain front buffer, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID2D1DeviceContext_CreateSharedBitmap(visual->device_context, &IID_IDXGISurface,
            surface, NULL, &src_bitmap);
    TRACE("swapchain %p CreateSharedBitmap hr %#lx bitmap %p.\n", swapchain, hr, src_bitmap);
    if (FAILED(hr))
    {
        ERR("Failed to create a bitmap from dxgi surface, hr %#lx.\n", hr);
        goto done;
    }

    hr = ID2D1Bitmap1_CopyFromBitmap(target_bitmap, NULL, src_bitmap, NULL);
    TRACE("swapchain %p CopyFromBitmap hr %#lx.\n", swapchain, hr);
    if (FAILED(hr))
    {
        ERR("Failed to copy bitmap, hr %#lx.\n", hr);
        goto done;
    }

    ID2D1DeviceContext_GetTarget(visual->device_context, &old_target);
    ID2D1DeviceContext_SetTarget(visual->device_context, (ID2D1Image *)target_bitmap);
    hr = ID2D1GdiInteropRenderTarget_GetDC(visual->interop, D2D1_DC_INITIALIZE_MODE_COPY,
            &src_dc);
    TRACE("swapchain %p GetDC hr %#lx dc %p.\n", swapchain, hr, src_dc);
    if (FAILED(hr))
    {
        ERR("GetDC failed, hr %#lx.\n", hr);
        ID2D1DeviceContext_SetTarget(visual->device_context, old_target);
        if (old_target)
            ID2D1Image_Release(old_target);
        goto done;
    }

    offset.x += (LONG)visual_offset_x;
    offset.y += (LONG)visual_offset_y;
    if (size.width == 1258 && size.height >= 700 && target->source_pixel_trace_count < 16)
    {
        POINT points[4] = {{0, 0}, {(LONG)size.width / 2, (LONG)size.height / 2},
                {(LONG)size.width / 4, (LONG)size.height / 4},
                {(LONG)size.width - 1, (LONG)size.height - 1}};
        unsigned int i;

        for (i = 0; i < ARRAY_SIZE(points); ++i)
            TRACE("source pixel swapchain %p point %ld,%ld GetPixel %#lx.\n", swapchain,
                    points[i].x, points[i].y,
                    (unsigned long)GetPixel(src_dc, points[i].x, points[i].y));
        ++target->source_pixel_trace_count;
    }
    copied = target_update_surface(target, destination, &offset, size.width, size.height, src_dc,
            NULL, 0);
    if ((size.width >= 1000 || size.height >= 700) && trace_count < 128)
    {
        TRACE("composite target %p destination %p visible %d offset %ld,%ld size %ux%u copied %d.\n",
                target->hwnd, destination, IsWindowVisible(destination), offset.x, offset.y,
                size.width, size.height, copied);
        ++trace_count;
    }

    ID2D1GdiInteropRenderTarget_ReleaseDC(visual->interop, NULL);
    ID2D1DeviceContext_SetTarget(visual->device_context, old_target);
    if (old_target)
        ID2D1Image_Release(old_target);

done:
    if (src_bitmap)
        ID2D1Bitmap_Release(src_bitmap);
    if (surface)
        IDXGISurface1_Release(surface);
    if (target_bitmap)
        ID2D1Bitmap1_Release(target_bitmap);
    if (swapchain)
        IDXGISwapChain1_Release(swapchain);
    if (swapchain3)
        IDXGISwapChain3_Release(swapchain3);
}

static unsigned int composite_visual_tree(struct composition_target *target,
        struct composition_visual *visual, float parent_offset_x, float parent_offset_y)
{
    struct composition_visual *child;
    float offset_x = parent_offset_x + visual->offset_x;
    float offset_y = parent_offset_y + visual->offset_y;
    unsigned int count = 0;

    if (visual->has_transform)
    {
        if (visual->transform._11 != 1.0f || visual->transform._12 != 0.0f
                || visual->transform._21 != 0.0f || visual->transform._22 != 1.0f)
            FIXME("Only translation components of visual transforms are supported.\n");
        offset_x += visual->transform._31;
        offset_y += visual->transform._32;
    }

    /* A visual's content is behind its children. Keep the traversal order
     * explicit so each child is painted above the content and the previous
     * sibling. This is the minimum tree behavior needed by browser controls
     * that use more than one visual. */
    if (visual->content)
    {
        do_composite(target, visual, offset_x, offset_y);
        ++count;
    }

    LIST_FOR_EACH_ENTRY(child, &visual->children, struct composition_visual, entry)
        count += composite_visual_tree(target, child, offset_x, offset_y);

    return count;
}

static DWORD WINAPI composite_thread_proc(void *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    unsigned int count, frequency, refresh_period;
    struct composition_target *target;
    struct composition_visual *visual;
    HDC hdc;

    /* TODO: Implement and use D3DKMTWaitForVerticalBlankEvent() */
    hdc = GetDC(0);
    frequency = GetDeviceCaps(hdc, VREFRESH);
    refresh_period = 1000 / frequency;
    ReleaseDC(0, hdc);

    while (TRUE)
    {
        EnterCriticalSection(&device->cs);

        count = 0;
        LIST_FOR_EACH_ENTRY(target, &device->targets, struct composition_target, entry)
        {
            HWND destination;
            POINT offset;

            if (!target->root)
                continue;

            visual = impl_from_IDCompositionVisual(target->root);
            if (!visual)
                continue;
            destination = get_composition_destination(target->hwnd, &offset);
            target_begin_surface_frame(target, destination);
            count += composite_visual_tree(target, visual, 0.0f, 0.0f);
            target_present_surface_frame(target, destination);
        }

        if (!count)
        {
            device->thread_exited = TRUE;
            LeaveCriticalSection(&device->cs);
            break;
        }

        LeaveCriticalSection(&device->cs);
        Sleep(refresh_period);
    }

    return 0;
}

static HRESULT STDMETHODCALLTYPE device_Commit(IDCompositionDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);
    HRESULT hr = S_OK;
    BOOL restarted = FALSE;
    static unsigned int trace_count;

    EnterCriticalSection(&device->cs);
    if (!device->thread || device->thread_exited)
    {
        if (device->thread)
        {
            WaitForSingleObject(device->thread, INFINITE);
            CloseHandle(device->thread);
        }
        device->thread_exited = FALSE;
        device->thread = CreateThread(NULL, 0, composite_thread_proc, iface, 0, NULL);
        restarted = TRUE;
    }

    if (trace_count < 128)
    {
        TRACE("device %p Commit thread %p restarted %d thread_exited %d.\n",
                device, device->thread, restarted, device->thread_exited);
        ++trace_count;
    }

    LeaveCriticalSection(&device->cs);

    return hr;
}

static HRESULT STDMETHODCALLTYPE device_WaitForCommitCompletion(IDCompositionDevice *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_GetFrameStatistics(IDCompositionDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTargetForHwnd(IDCompositionDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, hwnd %p, topmost %d, target %p\n", iface, hwnd, topmost, target);

    return create_target(device, hwnd, topmost, target);
}

static HRESULT STDMETHODCALLTYPE device_CreateVisual(IDCompositionDevice *iface,
        IDCompositionVisual **visual)
{
    struct composition_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, visual %p\n", iface, visual);

    return create_visual(device, 1, &IID_IDCompositionVisual, (void **)visual);
}

static HRESULT STDMETHODCALLTYPE device_CreateSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateVirtualSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHandle(IDCompositionDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHwnd(IDCompositionDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTranslateTransform(IDCompositionDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateScaleTransform(IDCompositionDevice *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRotateTransform(IDCompositionDevice *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSkewTransform(IDCompositionDevice *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateMatrixTransform(IDCompositionDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTransformGroup(IDCompositionDevice *iface,
        IDCompositionTransform **transforms, UINT elements,
        IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n", iface, transforms,
            elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTranslateTransform3D(IDCompositionDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, translate_transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateScaleTransform3D(IDCompositionDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRotateTransform3D(IDCompositionDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateMatrixTransform3D(IDCompositionDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTransform3DGroup(IDCompositionDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n", iface,
            transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateEffectGroup(IDCompositionDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateRectangleClip(IDCompositionDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateAnimation(IDCompositionDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CheckDeviceState(IDCompositionDevice *iface,
        BOOL *valid)
{
    FIXME("iface %p, valid %p stub!\n", iface, valid);
    return E_NOTIMPL;
}

static const struct IDCompositionDeviceVtbl device_vtbl =
{
    /* IUnknown methods */
    device_QueryInterface,
    device_AddRef,
    device_Release,
    /* IDCompositionDevice methods */
    device_Commit,
    device_WaitForCommitCompletion,
    device_GetFrameStatistics,
    device_CreateTargetForHwnd,
    device_CreateVisual,
    device_CreateSurface,
    device_CreateVirtualSurface,
    device_CreateSurfaceFromHandle,
    device_CreateSurfaceFromHwnd,
    device_CreateTranslateTransform,
    device_CreateScaleTransform,
    device_CreateRotateTransform,
    device_CreateSkewTransform,
    device_CreateMatrixTransform,
    device_CreateTransformGroup,
    device_CreateTranslateTransform3D,
    device_CreateScaleTransform3D,
    device_CreateRotateTransform3D,
    device_CreateMatrixTransform3D,
    device_CreateTransform3DGroup,
    device_CreateEffectGroup,
    device_CreateRectangleClip,
    device_CreateAnimation,
    device_CheckDeviceState,
};

static HRESULT STDMETHODCALLTYPE desktop_device_QueryInterface(IDCompositionDesktopDevice *iface,
        REFIID iid, void **out)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE desktop_device_AddRef(IDCompositionDesktopDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE desktop_device_Release(IDCompositionDesktopDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE desktop_device_Commit(IDCompositionDesktopDevice *iface)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p.\n", iface);

    return IDCompositionDevice_Commit(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE desktop_device_WaitForCommitCompletion(IDCompositionDesktopDevice *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_GetFrameStatistics(IDCompositionDesktopDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateVisual(IDCompositionDesktopDevice *iface,
        IDCompositionVisual2 **visual)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p, visual %p\n", iface, visual);

    return create_visual(device, 2, &IID_IDCompositionVisual2, (void **)visual);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFactory(IDCompositionDesktopDevice *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **surface_factory)
{
    FIXME("iface %p, rendering_device %p, surface_factory %p stub!\n", iface, rendering_device,
            surface_factory);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurface(IDCompositionDesktopDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateVirtualSurface(IDCompositionDesktopDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, width %u, height %u, format %#x, alpha_mode %#x, surface %p stub!\n", iface,
            width, height, pixel_format, alpha_mode, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTranslateTransform(IDCompositionDesktopDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateScaleTransform(IDCompositionDesktopDevice *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRotateTransform(IDCompositionDesktopDevice *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSkewTransform(IDCompositionDesktopDevice *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateMatrixTransform(IDCompositionDesktopDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTransformGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform **transforms, UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, transform_group %p stub!\n", iface, transforms,
            elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTranslateTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, translate_transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateScaleTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRotateTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateMatrixTransform3D(IDCompositionDesktopDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform_3d %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTransform3DGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms_3d %p, elements %u, transform_3d_group %p stub!\n", iface,
            transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateEffectGroup(IDCompositionDesktopDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateRectangleClip(IDCompositionDesktopDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateAnimation(IDCompositionDesktopDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateTargetForHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct composition_device *device = impl_from_IDCompositionDesktopDevice(iface);

    TRACE("iface %p, hwnd %p, topmost %d, target %p\n", iface, hwnd, topmost, target);

    return IDCompositionDevice_CreateTargetForHwnd(&device->IDCompositionDevice_iface, hwnd,
            topmost, target);
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFromHandle(IDCompositionDesktopDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE desktop_device_CreateSurfaceFromHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    return E_NOTIMPL;
}

static const struct IDCompositionDesktopDeviceVtbl desktop_device_vtbl =
{
    /* IUnknown methods */
    desktop_device_QueryInterface,
    desktop_device_AddRef,
    desktop_device_Release,
    /* IDCompositionDevice2 methods */
    desktop_device_Commit,
    desktop_device_WaitForCommitCompletion,
    desktop_device_GetFrameStatistics,
    desktop_device_CreateVisual,
    desktop_device_CreateSurfaceFactory,
    desktop_device_CreateSurface,
    desktop_device_CreateVirtualSurface,
    desktop_device_CreateTranslateTransform,
    desktop_device_CreateScaleTransform,
    desktop_device_CreateRotateTransform,
    desktop_device_CreateSkewTransform,
    desktop_device_CreateMatrixTransform,
    desktop_device_CreateTransformGroup,
    desktop_device_CreateTranslateTransform3D,
    desktop_device_CreateScaleTransform3D,
    desktop_device_CreateRotateTransform3D,
    desktop_device_CreateMatrixTransform3D,
    desktop_device_CreateTransform3DGroup,
    desktop_device_CreateEffectGroup,
    desktop_device_CreateRectangleClip,
    desktop_device_CreateAnimation,
    /* IDCompositionDesktopDevice methods */
    desktop_device_CreateTargetForHwnd,
    desktop_device_CreateSurfaceFromHandle,
    desktop_device_CreateSurfaceFromHwnd,
};

static HRESULT create_device(int version, REFIID iid, void **out)
{
    struct composition_device *device;
    HRESULT hr;

    if (!out)
        return E_INVALIDARG;

    device = calloc(1, sizeof(*device));
    if (!device)
        return E_OUTOFMEMORY;

    InitializeCriticalSection(&device->cs);
    list_init(&device->targets);
    device->IDCompositionDevice_iface.lpVtbl = &device_vtbl;
    device->IDCompositionDesktopDevice_iface.lpVtbl = &desktop_device_vtbl;
    device->version = version;
    device->ref = 1;
    hr = IDCompositionDevice_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
    IDCompositionDevice_Release(&device->IDCompositionDevice_iface);
    return hr;
}

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p\n", dxgi_device, debugstr_guid(iid), device);

    if (!IsEqualIID(iid, &IID_IDCompositionDevice))
        return E_NOINTERFACE;

    return create_device(1, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p\n", rendering_device, debugstr_guid(iid), device);

    if (!IsEqualIID(iid, &IID_IDCompositionDevice)
            && !IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        return E_NOINTERFACE;

    return create_device(2, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    FIXME("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    return E_NOTIMPL;
}
