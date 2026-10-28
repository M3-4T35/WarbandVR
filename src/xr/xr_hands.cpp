#include "xr/xr_hands.h"
#include "xr/xr_controls.h"
#include "util/log.h"

#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <cmath>
#include <cstring>

#define LOG_XR(...) ::vrlog::Write("[xr]", __VA_ARGS__)

using namespace DirectX;

namespace xr {
namespace {

constexpr char kShader[] = R"(
cbuffer Constants : register(b0) {
    float4x4 mvp;
    float4x4 rotation;
    float4 color;
};
struct VSIn { float3 pos : POSITION; float3 nrm : NORMAL; };
struct VSOut { float4 pos : SV_Position; float3 nrm : NORMAL; };
VSOut vs(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos, 1), mvp);
    o.nrm = mul(float4(i.nrm, 0), rotation).xyz;
    return o;
}
float4 ps(VSOut i) : SV_Target {
    float light = 0.45 + 0.55 * saturate(dot(normalize(i.nrm), normalize(float3(0.3, 1.0, 0.5))));
    return float4(color.rgb * light * color.a, color.a); // alpha prémultiplié
}
// Panneau plat : rectangle unitaire tiré du numéro de sommet (bande de 2 triangles), color.a = opaque
Texture2D panelTexture : register(t0);
SamplerState panelSampler : register(s0);
struct PanelOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
PanelOut panel_vs(uint id : SV_VertexID) {
    float2 c = float2(id & 1, id >> 1);
    PanelOut o;
    o.pos = mul(float4(c.x - 0.5, 0.5 - c.y, 0, 1), mvp);
    o.uv = c;
    return o;
}
float4 panel_ps(PanelOut i) : SV_Target {
    float4 c = panelTexture.Sample(panelSampler, i.uv);
    return color.a > 0.5 ? float4(c.rgb, 1) : c;
}
)";

struct Vertex {
    float pos[3];
    float nrm[3];
};

struct Constants {
    XMFLOAT4X4 mvp;
    XMFLOAT4X4 rotation;
    float color[4];
};

// Couleurs et dimensions (mètres). La pose de visée est au bout de la manette, qui pointe
// vers -z : le boîtier est donc derrière (+z).
constexpr float kBodyColor[4] = {0.16f, 0.17f, 0.19f, 1.0f};
constexpr float kRayColor[4] = {0.35f, 0.80f, 1.00f, 1.0f};
constexpr float kBodySize[3] = {0.035f, 0.030f, 0.110f};
constexpr float kBodyOffset = 0.055f;
constexpr float kRayWidth = 0.003f;

constexpr float kNear = 0.03f;
constexpr float kFar = 100.0f;

XMMATRIX PoseMatrix(const XrPosef& p)
{
    return XMMatrixRotationQuaternion(XMVectorSet(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w)) *
           XMMatrixTranslation(p.position.x, p.position.y, p.position.z);
}

} // namespace

bool HandRenderer::Init(XrSession session, ID3D11Device* device, ID3D11DeviceContext* context, int64_t format,
                        uint32_t size)
{
    device_ = device;
    context_ = context;
    format_ = format;
    size_ = size;
    if (!CreateShaders())
        return false;

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = ci.height = size;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    for (Eye& e : eyes_) {
        if (const XrResult r = xrCreateSwapchain(session, &ci, &e.swapchain); XR_FAILED(r)) {
            LOG_XR("ERREUR : swapchain des manettes (XrResult %d, format %lld), manettes non affichees",
                   static_cast<int>(r), static_cast<long long>(format));
            return false;
        }
        uint32_t count = 0;
        xrEnumerateSwapchainImages(e.swapchain, 0, &count, nullptr);
        e.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(e.swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data()));
        e.rtvs.assign(count, nullptr);
        for (uint32_t i = 0; i < count; ++i) {
            D3D11_TEXTURE2D_DESC td{};
            e.images[i].texture->GetDesc(&td);
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = static_cast<DXGI_FORMAT>(format);
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            const HRESULT hr = device->CreateRenderTargetView(e.images[i].texture, &rd, &e.rtvs[i]);
            if (FAILED(hr)) {
                LOG_XR("ERREUR : vue de rendu des manettes (image %u, format texture %d, bind 0x%X) : 0x%08lX", i,
                       td.Format, td.BindFlags, static_cast<unsigned long>(hr));
                return false;
            }
        }
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = dd.Height = size;
        dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device->CreateTexture2D(&dd, nullptr, &e.depth)) ||
            FAILED(device->CreateDepthStencilView(e.depth, nullptr, &e.dsv))) {
            LOG_XR("ERREUR : tampon de profondeur des manettes");
            return false;
        }
    }
    ready_ = true;
    LOG_XR("manettes 3D pretes (%ux%u par oeil)", size, size);
    return true;
}

bool HandRenderer::CreateShaders()
{
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                       UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (!compile) {
        LOG_XR("ERREUR : d3dcompiler_47.dll indisponible, manettes non affichees");
        return false;
    }
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* panelVsBlob = nullptr;
    ID3DBlob* panelPsBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile(kShader, sizeof(kShader) - 1, "hands", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsBlob, &errors);
    if (SUCCEEDED(hr))
        hr = compile(kShader, sizeof(kShader) - 1, "hands", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psBlob, &errors);
    if (SUCCEEDED(hr))
        hr = compile(kShader, sizeof(kShader) - 1, "hands", nullptr, nullptr, "panel_vs", "vs_5_0", 0, 0,
                     &panelVsBlob, &errors);
    if (SUCCEEDED(hr))
        hr = compile(kShader, sizeof(kShader) - 1, "hands", nullptr, nullptr, "panel_ps", "ps_5_0", 0, 0,
                     &panelPsBlob, &errors);
    if (FAILED(hr)) {
        for (ID3DBlob* b : {psBlob, panelVsBlob})
            if (b)
                b->Release();
        LOG_XR("ERREUR : compilation des shaders des manettes : %s",
               errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        if (errors)
            errors->Release();
        if (vsBlob)
            vsBlob->Release();
        return false;
    }
    device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_);
    device_->CreateVertexShader(panelVsBlob->GetBufferPointer(), panelVsBlob->GetBufferSize(), nullptr, &panelVs_);
    device_->CreatePixelShader(panelPsBlob->GetBufferPointer(), panelPsBlob->GetBufferSize(), nullptr, &panelPs_);
    panelVsBlob->Release();
    panelPsBlob->Release();
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sampler, &sampler_);
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device_->CreateInputLayout(elements, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout_);
    vsBlob->Release();
    psBlob->Release();

    // Cube unitaire centré, 4 sommets par face pour des normales franches
    Vertex v[24];
    uint16_t idx[36];
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        // Deux axes tangents à la face
        const float* nf = n[f];
        float a[3] = {nf[1] != 0 ? 1.0f : 0.0f, nf[1] != 0 ? 0.0f : 1.0f, 0};
        float b[3] = {nf[1] * a[2] - nf[2] * a[1], nf[2] * a[0] - nf[0] * a[2], nf[0] * a[1] - nf[1] * a[0]};
        const float s[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int k = 0; k < 4; ++k) {
            Vertex& vx = v[f * 4 + k];
            for (int c = 0; c < 3; ++c) {
                vx.pos[c] = 0.5f * (nf[c] + s[k][0] * a[c] + s[k][1] * b[c]);
                vx.nrm[c] = nf[c];
            }
        }
        const uint16_t base = static_cast<uint16_t>(f * 4);
        const uint16_t q[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; ++k)
            idx[f * 6 + k] = base + q[k];
    }
    D3D11_BUFFER_DESC bd{sizeof(v), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
    D3D11_SUBRESOURCE_DATA sd{v};
    device_->CreateBuffer(&bd, &sd, &vertices_);
    bd = {sizeof(idx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
    sd = {idx};
    device_->CreateBuffer(&bd, &sd, &indices_);
    bd = {sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    device_->CreateBuffer(&bd, nullptr, &constants_);

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS;
    device_->CreateDepthStencilState(&ds, &depthState_);
    // Pas d'élimination des faces : l'orientation des triangles n'a pas d'importance
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    device_->CreateRasterizerState(&rs, &raster_);
    return vs_ && ps_ && panelVs_ && panelPs_ && sampler_ && layout_ && vertices_ && indices_ && constants_ && depthState_ && raster_;
}

void HandRenderer::DrawBox(const float world[16], const float viewProj[16], const float rotation[16],
                           const float color[4])
{
    Constants c{};
    const XMMATRIX mvp = XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(world)) *
                         XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(viewProj));
    XMStoreFloat4x4(&c.mvp, XMMatrixTranspose(mvp));
    XMStoreFloat4x4(&c.rotation,
                    XMMatrixTranspose(XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(rotation))));
    memcpy(c.color, color, sizeof(c.color));
    context_->UpdateSubresource(constants_, 0, nullptr, &c, 0, 0);
    context_->DrawIndexed(36, 0, 0);
}

bool HandRenderer::Render(const XrView eyes[2], const XrControls& pointer, XrSpace space,
                          XrCompositionLayerProjection& layer, XrCompositionLayerProjectionView views[2],
                          const Panel* panel)
{
    if (panel && !panel->srv)
        panel = nullptr;
    if (!ready_ || (!panel && !pointer.GetHand(0).valid && !pointer.GetHand(1).valid))
        return false;

    // Boîtes à dessiner : boîtier de chaque main, rayon de la main active
    struct Box {
        XMFLOAT4X4 world;
        XMFLOAT4X4 rotation;
        const float* color;
    };
    std::vector<Box> boxes;
    for (int h = 0; h < 2; ++h) {
        const XrControls::Hand& hand = pointer.GetHand(h);
        if (!hand.valid)
            continue;
        const XMMATRIX pose = PoseMatrix(hand.pose);
        const XMMATRIX rot = XMMatrixRotationQuaternion(XMVectorSet(
            hand.pose.orientation.x, hand.pose.orientation.y, hand.pose.orientation.z, hand.pose.orientation.w));
        Box body{};
        XMStoreFloat4x4(&body.world, XMMatrixScaling(kBodySize[0], kBodySize[1], kBodySize[2]) *
                                         XMMatrixTranslation(0, 0, kBodyOffset) * pose);
        XMStoreFloat4x4(&body.rotation, rot);
        body.color = kBodyColor;
        boxes.push_back(body);
        if (h == pointer.ActiveHand() && pointer.RayLength() > 0) {
            const float len = pointer.RayLength();
            Box ray{};
            XMStoreFloat4x4(&ray.world, XMMatrixScaling(kRayWidth, kRayWidth, len) *
                                            XMMatrixTranslation(0, 0, -len / 2) * pose);
            XMStoreFloat4x4(&ray.rotation, rot);
            ray.color = kRayColor;
            boxes.push_back(ray);
        }
    }

    const UINT stride = sizeof(Vertex), offset = 0;
    for (int e = 0; e < 2; ++e) {
        Eye& eye = eyes_[e];
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        const XrResult acq = xrAcquireSwapchainImage(eye.swapchain, &ai, &index);
        if (XR_FAILED(acq)) {
            if (!loggedError_)
                LOG_XR("ERREUR : acquisition d'image des manettes : %d", static_cast<int>(acq));
            loggedError_ = true;
            return false;
        }
        if (XR_FAILED(xrWaitSwapchainImage(eye.swapchain, &wi))) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(eye.swapchain, &ri);
            return false;
        }

        const float clear[4] = {0, 0, 0, 0};
        context_->ClearRenderTargetView(eye.rtvs[index], clear);
        context_->ClearDepthStencilView(eye.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        context_->OMSetRenderTargets(1, &eye.rtvs[index], eye.dsv);
        context_->OMSetDepthStencilState(depthState_, 0);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(size_), static_cast<float>(size_), 0, 1};
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(raster_);
        context_->IASetInputLayout(layout_);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetVertexBuffers(0, 1, &vertices_, &stride, &offset);
        context_->IASetIndexBuffer(indices_, DXGI_FORMAT_R16_UINT, 0);
        context_->VSSetShader(vs_, nullptr, 0);
        context_->VSSetConstantBuffers(0, 1, &constants_);
        context_->PSSetShader(ps_, nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, &constants_); // la couleur est lue par le shader de pixels

        // Vue = inverse de la pose de l'œil ; projection asymétrique de son champ de vue
        const XrView& v = eyes[e];
        const XMMATRIX view = XMMatrixInverse(nullptr, PoseMatrix(v.pose));
        const XMMATRIX proj = XMMatrixPerspectiveOffCenterRH(
            kNear * std::tan(v.fov.angleLeft), kNear * std::tan(v.fov.angleRight), kNear * std::tan(v.fov.angleDown),
            kNear * std::tan(v.fov.angleUp), kNear, kFar);
        XMFLOAT4X4 viewProj;
        XMStoreFloat4x4(&viewProj, view * proj);
        if (panel) {
            // Panneau d'abord : les manettes passent devant au test de profondeur
            Constants c{};
            const XMMATRIX world = XMMatrixScaling(panel->width, panel->height, 1) *
                                   XMMatrixTranslation(panel->center.x, panel->center.y, panel->center.z);
            XMStoreFloat4x4(&c.mvp, XMMatrixTranspose(world * view * proj));
            c.color[3] = panel->opaque ? 1.0f : 0.0f;
            context_->UpdateSubresource(constants_, 0, nullptr, &c, 0, 0);
            context_->IASetInputLayout(nullptr);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context_->VSSetShader(panelVs_, nullptr, 0);
            context_->PSSetShader(panelPs_, nullptr, 0);
            context_->PSSetShaderResources(0, 1, &panel->srv);
            context_->PSSetSamplers(0, 1, &sampler_);
            context_->Draw(4, 0);
            ID3D11ShaderResourceView* none = nullptr;
            context_->PSSetShaderResources(0, 1, &none);
            context_->IASetInputLayout(layout_);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->VSSetShader(vs_, nullptr, 0);
            context_->PSSetShader(ps_, nullptr, 0);
        }
        for (const Box& b : boxes)
            DrawBox(&b.world._11, &viewProj._11, &b.rotation._11, b.color);

        context_->OMSetRenderTargets(0, nullptr, nullptr);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(eye.swapchain, &ri);

        views[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[e].pose = v.pose;
        views[e].fov = v.fov;
        views[e].subImage = {eye.swapchain, {{0, 0}, {static_cast<int32_t>(size_), static_cast<int32_t>(size_)}}, 0};
    }
    if (!loggedRender_) {
        const XrPosef& p = pointer.GetHand(pointer.ActiveHand()).pose;
        LOG_XR("premier rendu des manettes : %zu boites, main active a (%.2f %.2f %.2f), oeil gauche a "
               "(%.2f %.2f %.2f)",
               boxes.size(), p.position.x, p.position.y, p.position.z, eyes[0].pose.position.x,
               eyes[0].pose.position.y, eyes[0].pose.position.z);
        loggedRender_ = true;
    }
    layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = space;
    layer.viewCount = 2;
    layer.views = views;
    return true;
}

void HandRenderer::Shutdown()
{
    for (Eye& e : eyes_) {
        for (auto* r : e.rtvs)
            if (r)
                r->Release();
        if (e.dsv)
            e.dsv->Release();
        if (e.depth)
            e.depth->Release();
        if (e.swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(e.swapchain);
        e = Eye{};
    }
    for (IUnknown** p : {reinterpret_cast<IUnknown**>(&vs_), reinterpret_cast<IUnknown**>(&ps_),
                         reinterpret_cast<IUnknown**>(&panelVs_), reinterpret_cast<IUnknown**>(&panelPs_),
                         reinterpret_cast<IUnknown**>(&sampler_),
                         reinterpret_cast<IUnknown**>(&layout_), reinterpret_cast<IUnknown**>(&vertices_),
                         reinterpret_cast<IUnknown**>(&indices_), reinterpret_cast<IUnknown**>(&constants_),
                         reinterpret_cast<IUnknown**>(&depthState_), reinterpret_cast<IUnknown**>(&raster_)}) {
        if (*p)
            (*p)->Release();
        *p = nullptr;
    }
    ready_ = false;
}

} // namespace xr
