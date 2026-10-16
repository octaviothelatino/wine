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

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "dcomp_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

static void visual_clear_frame(struct composition_visual *visual)
{
    if (visual->cached_surface)
        IDXGISurface1_Release(visual->cached_surface);
    if (visual->readback_texture)
        ID3D11Texture2D_Release(visual->readback_texture);
    free(visual->cached_pixels);
    visual->cached_surface = NULL;
    visual->readback_texture = NULL;
    visual->cached_pixels = NULL;
    visual->cached_width = visual->cached_height = 0;
}

static HRESULT STDMETHODCALLTYPE visual_QueryInterface(IDCompositionVisual2 *iface, REFIID iid,
        void **out)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, iid %s, out %p stub!\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionVisual)
            || (visual->version >= 2 && IsEqualGUID(iid, &IID_IDCompositionVisual2)))
    {
        IUnknown_AddRef(&visual->IDCompositionVisual2_iface);
        *out = &visual->IDCompositionVisual2_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE visual_AddRef(IDCompositionVisual2 *iface)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);
    ULONG ref = InterlockedIncrement(&visual->ref);

    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE visual_Release(IDCompositionVisual2 *iface)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);
    ULONG ref = InterlockedDecrement(&visual->ref);
    struct composition_device *device;
    struct composition_visual *child, *next;
    struct list children;
    ID2D1GdiInteropRenderTarget *interop;
    ID2D1DeviceContext *device_context;
    IUnknown *content;

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (ref)
        return ref;

    device = visual->device;
    list_init(&children);

    /* The compositor thread holds this lock while traversing the visual tree.
     * Detach mutable state before releasing any COM objects, so it cannot
     * observe a half-destroyed visual. */
    EnterCriticalSection(&device->cs);
    LIST_FOR_EACH_ENTRY_SAFE(child, next, &visual->children, struct composition_visual, entry)
    {
        list_remove(&child->entry);
        child->parent = NULL;
        list_add_tail(&children, &child->entry);
    }
    interop = visual->interop;
    device_context = visual->device_context;
    content = visual->content;
    visual->interop = NULL;
    visual->device_context = NULL;
    visual->content = NULL;
    visual_clear_frame(visual);
    LeaveCriticalSection(&device->cs);

    LIST_FOR_EACH_ENTRY_SAFE(child, next, &children, struct composition_visual, entry)
    {
        list_remove(&child->entry);
        IDCompositionVisual_Release((IDCompositionVisual *)&child->IDCompositionVisual2_iface);
    }
    if (interop)
        ID2D1GdiInteropRenderTarget_Release(interop);
    if (device_context)
        ID2D1DeviceContext_Release(device_context);
    if (content)
        IUnknown_Release(content);
    free(visual);
    IDCompositionDevice_Release(&device->IDCompositionDevice_iface);

    return ref;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetXAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetX(IDCompositionVisual2 *iface, float offset_x)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, offset_x %f.\n", iface, offset_x);
    EnterCriticalSection(&visual->device->cs);
    visual->offset_x = offset_x;
    LeaveCriticalSection(&visual->device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetYAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetY(IDCompositionVisual2 *iface, float offset_y)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, offset_y %f.\n", iface, offset_y);
    EnterCriticalSection(&visual->device->cs);
    visual->offset_y = offset_y;
    LeaveCriticalSection(&visual->device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransformObject(IDCompositionVisual2 *iface,
        IDCompositionTransform *transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransform(IDCompositionVisual2 *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);

    TRACE("iface %p, matrix %p.\n", iface, matrix);
    if (!matrix)
        return E_INVALIDARG;

    EnterCriticalSection(&visual->device->cs);
    visual->transform = *matrix;
    visual->has_transform = TRUE;
    LeaveCriticalSection(&visual->device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransformParent(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual)
{
    FIXME("iface %p, visual %p stub!\n", iface, visual);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetEffect(IDCompositionVisual2 *iface,
        IDCompositionEffect *effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetBitmapInterpolationMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode)
{
    FIXME("iface %p, interpolation_mode %d stub!\n", iface, interpolation_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetBorderMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BORDER_MODE border_mode)
{
    FIXME("iface %p, border_mode %d stub!\n", iface, border_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetClipObject(IDCompositionVisual2 *iface,
        IDCompositionClip *clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

/* C++ method: SetClip(THIS_ const D2D_RECT_F &rect).  Use pointer instead of reference */
static HRESULT STDMETHODCALLTYPE visual_SetClip(IDCompositionVisual2 *iface, const D2D_RECT_F *rect)
{
    FIXME("iface %p, rect %p stub!\n", iface, rect);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetContent(IDCompositionVisual2 *iface, IUnknown *content)
{
    struct composition_visual *visual = impl_from_IDCompositionVisual2(iface);
    ID2D1GdiInteropRenderTarget *old_interop;
    ID2D1DeviceContext *old_device_context;
    IUnknown *old_content;
    ID2D1GdiInteropRenderTarget *interop;
    ID2D1DeviceContext *device_context;
    IDXGISwapChain1 *dxgi_swapchain;
    IDXGIDevice *dxgi_device;
    ID2D1Device *d2d_device;
    HRESULT hr;
    static unsigned int trace_count;

    FIXME("iface %p, content %p semi-stub!\n", iface, content);

    if (content)
    {
        if (FAILED(IUnknown_QueryInterface(content, &IID_IDXGISwapChain1,
                (void **)&dxgi_swapchain)))
        {
            FIXME("Only IDXGISwapChain1 is currently supported.\n");
            return E_INVALIDARG;
        }

        hr = IDXGISwapChain1_GetDevice(dxgi_swapchain, &IID_IDXGIDevice, (void **)&dxgi_device);
        IDXGISwapChain1_Release(dxgi_swapchain);
        if (FAILED(hr))
        {
            ERR("Failed to get the swapchain device, hr %#lx.\n", hr);
            return hr;
        }

        hr = D2D1CreateDevice(dxgi_device, NULL, &d2d_device);
        IDXGIDevice_Release(dxgi_device);
        if (FAILED(hr))
        {
            ERR("Failed to create a D2D device, hr %#lx.\n", hr);
            return hr;
        }

        hr = ID2D1Device_CreateDeviceContext(d2d_device, D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                &device_context);
        ID2D1Device_Release(d2d_device);
        if (FAILED(hr))
        {
            ERR("Failed to create a D2D device context, hr %#lx.\n", hr);
            return hr;
        }

        if (FAILED(hr = ID2D1DeviceContext_QueryInterface(device_context,
                &IID_ID2D1GdiInteropRenderTarget, (void **)&interop)))
        {
            ERR("Failed to get a ID2D1GdiInteropRenderTarget, hr %#lx.\n", hr);
            ID2D1DeviceContext_Release(device_context);
            return hr;
        }
    }

    /* Swap the three pointers as one operation. The compositor holds the
     * same lock while it reads them, so it can never use a released object. */
    old_interop = NULL;
    old_device_context = NULL;
    old_content = NULL;
    EnterCriticalSection(&visual->device->cs);
    old_interop = visual->interop;
    old_device_context = visual->device_context;
    old_content = visual->content;
    visual_clear_frame(visual);
    visual->interop = NULL;
    visual->device_context = NULL;
    visual->content = NULL;
    if (content)
    {
        IUnknown_AddRef(content);
        visual->content = content;
        visual->device_context = device_context;
        visual->interop = interop;
    }
    LeaveCriticalSection(&visual->device->cs);

    if (old_interop)
        ID2D1GdiInteropRenderTarget_Release(old_interop);
    if (old_device_context)
        ID2D1DeviceContext_Release(old_device_context);
    if (old_content)
        IUnknown_Release(old_content);

    if (trace_count < 128)
    {
        TRACE("visual %p content association new %p stored %p device_context %p interop %p.\n",
                visual, content, visual->content, visual->device_context, visual->interop);
        ++trace_count;
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_AddVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual, BOOL insert_above, IDCompositionVisual *reference_visual)
{
    struct composition_visual *parent = impl_from_IDCompositionVisual2(iface);
    struct composition_visual *child, *reference;

    TRACE("iface %p, visual %p, insert_above %d, reference_visual %p.\n", iface, visual,
            insert_above, reference_visual);

    if (!visual || visual == (IDCompositionVisual *)iface)
        return E_INVALIDARG;

    child = impl_from_IDCompositionVisual(visual);
    EnterCriticalSection(&parent->device->cs);
    if (child->parent)
    {
        LeaveCriticalSection(&parent->device->cs);
        return E_INVALIDARG;
    }

    if (reference_visual)
    {
        reference = impl_from_IDCompositionVisual(reference_visual);
        if (reference == child || reference->parent != parent)
        {
            LeaveCriticalSection(&parent->device->cs);
            return E_INVALIDARG;
        }
    }
    else
        reference = NULL;

    child->parent = parent;
    IDCompositionVisual_AddRef(visual);

    if (reference)
    {
        if (insert_above)
            list_add_after(&reference->entry, &child->entry);
        else
            list_add_before(&reference->entry, &child->entry);
    }
    else if (insert_above)
        list_add_head(&parent->children, &child->entry);
    else
        list_add_tail(&parent->children, &child->entry);

    LeaveCriticalSection(&parent->device->cs);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_RemoveVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *visual)
{
    struct composition_visual *parent = impl_from_IDCompositionVisual2(iface);
    struct composition_visual *child;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (!visual)
        return E_INVALIDARG;

    child = impl_from_IDCompositionVisual(visual);
    EnterCriticalSection(&parent->device->cs);
    if (child->parent != parent)
    {
        LeaveCriticalSection(&parent->device->cs);
        return E_INVALIDARG;
    }

    list_remove(&child->entry);
    child->parent = NULL;
    LeaveCriticalSection(&parent->device->cs);
    IDCompositionVisual_Release(visual);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_RemoveAllVisuals(IDCompositionVisual2 *iface)
{
    struct composition_visual *parent = impl_from_IDCompositionVisual2(iface);
    struct composition_visual *child, *next;
    struct list children;

    TRACE("iface %p.\n", iface);

    list_init(&children);
    EnterCriticalSection(&parent->device->cs);
    LIST_FOR_EACH_ENTRY_SAFE(child, next, &parent->children, struct composition_visual, entry)
    {
        list_remove(&child->entry);
        child->parent = NULL;
        list_add_tail(&children, &child->entry);
    }
    LeaveCriticalSection(&parent->device->cs);

    LIST_FOR_EACH_ENTRY_SAFE(child, next, &children, struct composition_visual, entry)
    {
        list_remove(&child->entry);
        IDCompositionVisual_Release((IDCompositionVisual *)&child->IDCompositionVisual2_iface);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetCompositeMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_COMPOSITE_MODE composite_mode)
{
    FIXME("iface %p, composite_mode %d stub!\n", iface, composite_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOpacityMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_OPACITY_MODE opacity_mode)
{
    FIXME("iface %p, opacity_mode %d stub!\n", iface, opacity_mode);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetBackFaceVisibility(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BACKFACE_VISIBILITY visibility)
{
    FIXME("iface %p, visibility %d stub!\n", iface, visibility);
    return E_NOTIMPL;
}

static const struct IDCompositionVisual2Vtbl visual2_vtbl =
{
    /* IUnknown methods */
    visual_QueryInterface,
    visual_AddRef,
    visual_Release,
    /* IDCompositionVisual methods */
    visual_SetOffsetXAnimation,
    visual_SetOffsetX,
    visual_SetOffsetYAnimation,
    visual_SetOffsetY,
    visual_SetTransformObject,
    visual_SetTransform,
    visual_SetTransformParent,
    visual_SetEffect,
    visual_SetBitmapInterpolationMode,
    visual_SetBorderMode,
    visual_SetClipObject,
    visual_SetClip,
    visual_SetContent,
    visual_AddVisual,
    visual_RemoveVisual,
    visual_RemoveAllVisuals,
    visual_SetCompositeMode,
    /* IDCompositionVisual2 methods */
    visual_SetOpacityMode,
    visual_SetBackFaceVisibility,
};

HRESULT create_visual(struct composition_device *device, int version, REFIID iid, void **new_visual)
{
    struct composition_visual *visual;
    HRESULT hr;

    if (!new_visual)
        return E_INVALIDARG;

    visual = calloc(1, sizeof(*visual));
    if (!visual)
        return E_OUTOFMEMORY;

    visual->IDCompositionVisual2_iface.lpVtbl = &visual2_vtbl;
    visual->device = device;
    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    list_init(&visual->children);
    visual->version = version;
    visual->ref = 1;
    visual->transform._11 = 1.0f;
    visual->transform._22 = 1.0f;
    hr = IUnknown_QueryInterface(&visual->IDCompositionVisual2_iface, iid, new_visual);
    IUnknown_Release(&visual->IDCompositionVisual2_iface);
    return hr;
}
