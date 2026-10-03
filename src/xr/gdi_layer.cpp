#include "xr/gdi_layer.h"
#include "util/log.h"

#include <windows.h>

#define LOG_XR(...) ::vrlog::Write("[xr]", __VA_ARGS__)

namespace xr {

void GdiLayer::Init(XrSession session, ID3D11Device* device, ID3D11DeviceContext* context, int64_t format, int width,
                    int height, const char* name)
{
    session_ = session;
    device_ = device;
    context_ = context;
    format_ = format;
    width_ = width;
    height_ = height;
    name_ = name;
}

void GdiLayer::Shutdown()
{
    if (swapchain_ != XR_NULL_HANDLE)
        xrDestroySwapchain(swapchain_);
    swapchain_ = XR_NULL_HANDLE;
    images_.clear();
    if (staging_)
        staging_->Release();
    staging_ = nullptr;
    if (dc_) {
        SelectObject(dc_, oldBitmap_);
        DeleteDC(dc_);
    }
    if (bitmap_)
        DeleteObject(bitmap_);
    dc_ = nullptr;
    bitmap_ = nullptr;
    bits_ = nullptr;
    committed_ = false;
    failed_ = false;
}

bool GdiLayer::Create()
{
    failed_ = true; // en cas d'échec, on n'insiste pas
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = format_;
    ci.sampleCount = 1;
    ci.width = width_;
    ci.height = height_;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session_, &ci, &swapchain_))) {
        LOG_XR("ERREUR : swapchain du calque %s non creee", name_);
        swapchain_ = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr);
    images_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(swapchain_, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data()));

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = td.ArraySize = 1;
    td.Format = static_cast<DXGI_FORMAT>(format_);
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging_))) {
        LOG_XR("ERREUR : texture du calque %s non creee", name_);
        return false;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width_;
    bi.bmiHeader.biHeight = -height_; // de haut en bas
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc_ = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    bitmap_ = dc_ ? CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bitmap_) {
        LOG_XR("ERREUR : surface GDI du calque %s non creee", name_);
        return false;
    }
    oldBitmap_ = SelectObject(dc_, bitmap_);
    bits_ = static_cast<uint32_t*>(bits);
    failed_ = false;
    LOG_XR("calque %s pret (%dx%d)", name_, width_, height_);
    return true;
}

HDC GdiLayer::BeginDraw()
{
    if (failed_ || !session_)
        return nullptr;
    if (!dc_ && !Create())
        return nullptr;
    return dc_;
}

bool GdiLayer::Commit()
{
    if (!dc_ || !staging_)
        return false;
    GdiFlush();
    context_->UpdateSubresource(staging_, 0, nullptr, bits_, width_ * 4, 0);
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &index)))
        return false;
    const bool ok = XR_SUCCEEDED(xrWaitSwapchainImage(swapchain_, &wi));
    if (ok)
        context_->CopyResource(images_[index].texture, staging_);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &ri);
    committed_ = committed_ || ok;
    return ok;
}

void GdiLayer::Fill(XrSpace space, const XrPosef& pose, float width, XrCompositionLayerQuad& layer) const
{
    layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = space;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = {swapchain_, {{0, 0}, {width_, height_}}, 0};
    layer.pose = pose;
    layer.size = {width, width * height_ / width_};
}

} // namespace xr
