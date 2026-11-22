// ReDemake post-process (color grading) for the DirectX 11 backend.
// This file is included by gfx_direct3d11.cpp, inside "namespace Fast". Do not compile it on its own.

static const char* kPostHlsl = R"(
cbuffer PostCB : register(b3) {
    float uContrast;
    float uSaturation;
    float uWarmth;
    float uPad;
};
Texture2D<float4> uTex : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    // One big triangle that covers the whole screen, no vertex buffer needed.
    float2 p = float2((id == 1) ? 3.0 : -1.0, (id == 2) ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    // 1:1 pixel copy (same size), so the orientation stays exactly as the game made it.
    float3 c = uTex.Load(int3((int2)pos.xy, 0)).rgb;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(luma, luma, luma), c, uSaturation);
    c = (c - 0.5) * uContrast + 0.5;
    float l = saturate(dot(c, float3(0.2126, 0.7152, 0.0722)));
    c.r += uWarmth * (0.08 * l - 0.03 * (1.0 - l));
    c.g += uWarmth * (0.02 * l);
    c.b += uWarmth * (-0.08 * l + 0.05 * (1.0 - l));
    return float4(saturate(c), 1.0);
}

// ---- Bloom ----
cbuffer BloomCB : register(b4) {
    float4 uA; // xy = 1 / render target size, zw = step (source texel size, or the blur direction)
    float4 uB; // x = threshold (bright pass) or intensity (composite)
};
SamplerState uSmp : register(s0);

float4 PSBright(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy * uA.xy;
    float2 o = uA.zw;
    float3 c = uTex.SampleLevel(uSmp, uv + o * float2(-1.0, -1.0), 0).rgb;
    c += uTex.SampleLevel(uSmp, uv + o * float2(1.0, -1.0), 0).rgb;
    c += uTex.SampleLevel(uSmp, uv + o * float2(-1.0, 1.0), 0).rgb;
    c += uTex.SampleLevel(uSmp, uv + o * float2(1.0, 1.0), 0).rgb;
    c *= 0.25;
    float peak = max(c.r, max(c.g, c.b));
    float k = saturate((peak - uB.x) / max(1.0 - uB.x, 0.01));
    return float4(c * k, 1.0);
}

float4 PSBlur(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy * uA.xy;
    float2 s = uA.zw;
    float3 c = uTex.SampleLevel(uSmp, uv, 0).rgb * 0.2270270270;
    c += (uTex.SampleLevel(uSmp, uv + s * 1.3846153846, 0).rgb + uTex.SampleLevel(uSmp, uv - s * 1.3846153846, 0).rgb) * 0.3162162162;
    c += (uTex.SampleLevel(uSmp, uv + s * 3.2307692308, 0).rgb + uTex.SampleLevel(uSmp, uv - s * 3.2307692308, 0).rgb) * 0.0702702703;
    return float4(c, 1.0);
}

float4 PSComposite(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy * uA.xy;
    return float4(uTex.SampleLevel(uSmp, uv, 0).rgb * uB.x, 0.0);
}

// ---- Depth debug (Phase 2 test): shows the game's depth buffer as grey levels ----
Texture2D<float> uDepth : register(t1);

float4 PSDepth(float4 pos : SV_Position) : SV_Target {
    float d = uDepth.Load(int3((int2)pos.xy, 0));
    // The depth is 0 (near) to 1 (far) but almost everything sits very close to 1,
    // so a log scale of (1 - d) spreads it out: dark = near, bright = far.
    float v = saturate(-log2(max(1.0 - d, 0.000001)) / 20.0);
    return float4(v, v, v, 1.0);
}
)";

static int PostClampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

bool GfxRenderingAPIDX11::InitPostProcess() {
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
    ComPtr<ID3DBlob> vsBlob, psBlob, err;

    HRESULT hr = mD3dCompile(kPostHlsl, strlen(kPostHlsl), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", flags, 0,
                             vsBlob.GetAddressOf(), err.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SPDLOG_ERROR("ReDemake post-process (DX11): vertex shader failed to compile: {}",
                     err ? (const char*)err->GetBufferPointer() : "unknown");
        return false;
    }
    hr = mD3dCompile(kPostHlsl, strlen(kPostHlsl), nullptr, nullptr, nullptr, "PSMain", "ps_4_0", flags, 0,
                     psBlob.GetAddressOf(), err.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SPDLOG_ERROR("ReDemake post-process (DX11): pixel shader failed to compile: {}",
                     err ? (const char*)err->GetBufferPointer() : "unknown");
        return false;
    }
    if (FAILED(mDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                           mPostVs.GetAddressOf())) ||
        FAILED(mDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                          mPostPs.GetAddressOf()))) {
        SPDLOG_ERROR("ReDemake post-process (DX11): failed to create shader objects");
        return false;
    }

    D3D11_BUFFER_DESC cbDesc;
    ZeroMemory(&cbDesc, sizeof(cbDesc));
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.ByteWidth = 16;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(mDevice->CreateBuffer(&cbDesc, nullptr, mPostCb.GetAddressOf()))) {
        SPDLOG_ERROR("ReDemake post-process (DX11): failed to create constant buffer");
        return false;
    }

    D3D11_RASTERIZER_DESC rsDesc;
    ZeroMemory(&rsDesc, sizeof(rsDesc));
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = false;
    rsDesc.ScissorEnable = false;

    D3D11_BLEND_DESC bsDesc;
    ZeroMemory(&bsDesc, sizeof(bsDesc));
    bsDesc.RenderTarget[0].BlendEnable = FALSE;
    bsDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_DEPTH_STENCIL_DESC dsDesc;
    ZeroMemory(&dsDesc, sizeof(dsDesc));
    dsDesc.DepthEnable = FALSE;
    dsDesc.StencilEnable = FALSE;

    if (FAILED(mDevice->CreateRasterizerState(&rsDesc, mPostRaster.GetAddressOf())) ||
        FAILED(mDevice->CreateBlendState(&bsDesc, mPostBlend.GetAddressOf())) ||
        FAILED(mDevice->CreateDepthStencilState(&dsDesc, mPostDepth.GetAddressOf()))) {
        SPDLOG_ERROR("ReDemake post-process (DX11): failed to create state objects");
        return false;
    }
    return true;
}

bool GfxRenderingAPIDX11::InitBloom() {
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
    const char* entries[3] = { "PSBright", "PSBlur", "PSComposite" };
    ComPtr<ID3D11PixelShader>* targets[3] = { &mBloomPsBright, &mBloomPsBlur, &mBloomPsComp };
    for (int i = 0; i < 3; i++) {
        ComPtr<ID3DBlob> blob, err;
        HRESULT hr = mD3dCompile(kPostHlsl, strlen(kPostHlsl), nullptr, nullptr, nullptr, entries[i], "ps_4_0", flags,
                                 0, blob.GetAddressOf(), err.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SPDLOG_ERROR("ReDemake bloom (DX11): {} failed to compile: {}", entries[i],
                         err ? (const char*)err->GetBufferPointer() : "unknown");
            return false;
        }
        if (FAILED(mDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                              targets[i]->GetAddressOf()))) {
            SPDLOG_ERROR("ReDemake bloom (DX11): failed to create {}", entries[i]);
            return false;
        }
    }

    D3D11_BUFFER_DESC cbDesc;
    ZeroMemory(&cbDesc, sizeof(cbDesc));
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.ByteWidth = 32;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_SAMPLER_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    // Additive blending: result = destination + source (the alpha channel stays as it was).
    D3D11_BLEND_DESC bd;
    ZeroMemory(&bd, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (FAILED(mDevice->CreateBuffer(&cbDesc, nullptr, mBloomCb.GetAddressOf())) ||
        FAILED(mDevice->CreateSamplerState(&sd, mBloomSampler.GetAddressOf())) ||
        FAILED(mDevice->CreateBlendState(&bd, mBloomAddBlend.GetAddressOf()))) {
        SPDLOG_ERROR("ReDemake bloom (DX11): failed to create buffer, sampler or blend state");
        return false;
    }
    return true;
}

// Runs after the color grading draw, so the vertex shader, topology, rasterizer and depth state of the grading pass
// are still set. ApplyPostProcess restores the game's own state afterwards.
void GfxRenderingAPIDX11::DrawBloom(uint32_t width, uint32_t height, float threshold, float intensity) {
    if (!mBloomInitTried) {
        mBloomInitTried = true;
        mBloomReady = InitBloom();
        SPDLOG_INFO("ReDemake bloom (DX11): shader init {}", mBloomReady ? "OK" : "FAILED");
    }
    if (!mBloomReady) {
        return;
    }

    const uint32_t qw = (width / 4) > 1 ? (width / 4) : 1;
    const uint32_t qh = (height / 4) > 1 ? (height / 4) : 1;
    if (mBloomWidth != qw || mBloomHeight != qh || mBloomSrv[0].Get() == nullptr) {
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = qw;
        td.Height = qh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < 2; i++) {
            if (FAILED(mDevice->CreateTexture2D(&td, nullptr, mBloomTex[i].ReleaseAndGetAddressOf())) ||
                FAILED(mDevice->CreateShaderResourceView(mBloomTex[i].Get(), nullptr,
                                                         mBloomSrv[i].ReleaseAndGetAddressOf())) ||
                FAILED(mDevice->CreateRenderTargetView(mBloomTex[i].Get(), nullptr,
                                                       mBloomRtv[i].ReleaseAndGetAddressOf()))) {
                SPDLOG_ERROR("ReDemake bloom (DX11): failed to create textures {}x{}", qw, qh);
                mBloomSrv[0].Reset();
                return;
            }
        }
        mBloomWidth = qw;
        mBloomHeight = qh;
        SPDLOG_INFO("ReDemake bloom (DX11): working textures {}x{}", qw, qh);
    }

    ID3D11Buffer* cbs[1] = { mBloomCb.Get() };
    mContext->PSSetConstantBuffers(4, 1, cbs);
    ID3D11SamplerState* samplers[1] = { mBloomSampler.Get() };
    mContext->PSSetSamplers(0, 1, samplers);

    // One full-screen draw: reads "src", writes "dst" (size dstW x dstH). "step" and "param" go to the shader.
    auto pass = [&](ID3D11PixelShader* ps, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, uint32_t dstW,
                    uint32_t dstH, float stepX, float stepY, float param) {
        ID3D11ShaderResourceView* none[1] = { nullptr };
        mContext->PSSetShaderResources(0, 1, none);
        ID3D11RenderTargetView* rtvs[1] = { dst };
        mContext->OMSetRenderTargets(1, rtvs, nullptr);
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width = (float)dstW;
        vp.Height = (float)dstH;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        mContext->RSSetViewports(1, &vp);
        D3D11_MAPPED_SUBRESOURCE ms;
        ZeroMemory(&ms, sizeof(ms));
        if (FAILED(mContext->Map(mBloomCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            return;
        }
        float data[8] = { 1.0f / (float)dstW, 1.0f / (float)dstH, stepX, stepY, param, 0.0f, 0.0f, 0.0f };
        memcpy(ms.pData, data, sizeof(data));
        mContext->Unmap(mBloomCb.Get(), 0);
        mContext->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* srvs[1] = { src };
        mContext->PSSetShaderResources(0, 1, srvs);
        mContext->Draw(3, 0);
    };

    // 1) Keep only the bright parts, shrunk to a quarter of the size.
    pass(mBloomPsBright.Get(), mPostSrv.Get(), mBloomRtv[0].Get(), qw, qh, 1.0f / (float)width, 1.0f / (float)height,
         threshold);
    // 2) Blur: horizontal then vertical, twice.
    for (int i = 0; i < 2; i++) {
        pass(mBloomPsBlur.Get(), mBloomSrv[0].Get(), mBloomRtv[1].Get(), qw, qh, 1.0f / (float)qw, 0.0f, 0.0f);
        pass(mBloomPsBlur.Get(), mBloomSrv[1].Get(), mBloomRtv[0].Get(), qw, qh, 0.0f, 1.0f / (float)qh, 0.0f);
    }
    // 3) Add the glow on top of the graded image.
    const float noBlendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    mContext->OMSetBlendState(mBloomAddBlend.Get(), noBlendFactor, 0xFFFFFFFF);
    pass(mBloomPsComp.Get(), mBloomSrv[0].Get(), mPostRtv.Get(), width, height, 0.0f, 0.0f, intensity);

    static bool sLoggedBloom = false;
    if (!sLoggedBloom) {
        sLoggedBloom = true;
        SPDLOG_INFO("ReDemake bloom (DX11): drawing (threshold {}, intensity {})", threshold, intensity);
    }
}

static Microsoft::WRL::ComPtr<ID3D11PixelShader> sDepthDbgPs;
static bool sDepthDbgTried = false;

void* GfxRenderingAPIDX11::ApplyPostProcess(int fb_id) {
    void* original = GetFramebufferTextureId(fb_id);

    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();
    static bool sLoggedReached = false;
    if (!sLoggedReached) {
        sLoggedReached = true;
        SPDLOG_INFO("ReDemake post-process: reached on the DirectX 11 backend (RemakePostFx={})",
                    cvars->GetInteger("gEnhancements.RemakePostFx", 0));
    }
    if (cvars->GetInteger("gEnhancements.RemakePostFx", 0) == 0) {
        return original;
    }
    if (fb_id <= 0 || fb_id >= (int)mFrameBuffers.size()) {
        return original;
    }
    const FramebufferDX11& srcFb = mFrameBuffers[fb_id];
    const TextureData& srcTex = mTextures[srcFb.texture_id];
    const uint32_t width = srcTex.width;
    const uint32_t height = srcTex.height;
    // With MSAA the game image has no shader view, so the effect is skipped (same limit as OpenGL).
    if (width == 0 || height == 0 || srcFb.msaa_level > 1 || srcTex.resource_view.Get() == nullptr) {
        return original;
    }

    if (!mPostInitTried) {
        mPostInitTried = true;
        mPostReady = InitPostProcess();
        SPDLOG_INFO("ReDemake post-process (DX11): shader init {}", mPostReady ? "OK" : "FAILED");
    }
    if (!mPostReady) {
        return original;
    }

    // (Re)create the output texture when the game image size changes.
    if (mPostWidth != width || mPostHeight != height || mPostSrv.Get() == nullptr) {
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(mDevice->CreateTexture2D(&td, nullptr, mPostTex.ReleaseAndGetAddressOf())) ||
            FAILED(mDevice->CreateShaderResourceView(mPostTex.Get(), nullptr, mPostSrv.ReleaseAndGetAddressOf())) ||
            FAILED(mDevice->CreateRenderTargetView(mPostTex.Get(), nullptr, mPostRtv.ReleaseAndGetAddressOf()))) {
            SPDLOG_ERROR("ReDemake post-process (DX11): failed to create output texture {}x{}", width, height);
            mPostSrv.Reset();
            return original;
        }
        mPostWidth = width;
        mPostHeight = height;
        SPDLOG_INFO("ReDemake post-process (DX11): output texture {}x{}", width, height);
    }

    const float contrast = PostClampInt(cvars->GetInteger("gEnhancements.RemakePostFxContrast", 110), 50, 150) / 100.0f;
    const float saturation =
        PostClampInt(cvars->GetInteger("gEnhancements.RemakePostFxSaturation", 120), 0, 200) / 100.0f;
    const float warmth = PostClampInt(cvars->GetInteger("gEnhancements.RemakePostFxWarmth", 15), -50, 50) / 100.0f;

    D3D11_MAPPED_SUBRESOURCE ms;
    ZeroMemory(&ms, sizeof(ms));
    if (FAILED(mContext->Map(mPostCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        return original;
    }
    float cbData[4] = { contrast, saturation, warmth, 0.0f };
    memcpy(ms.pData, cbData, sizeof(cbData));
    mContext->Unmap(mPostCb.Get(), 0);

    // Save the state we are about to change.
    ComPtr<ID3D11RenderTargetView> prevRtv;
    ComPtr<ID3D11DepthStencilView> prevDsv;
    mContext->OMGetRenderTargets(1, prevRtv.GetAddressOf(), prevDsv.GetAddressOf());
    UINT numViewports = 1;
    D3D11_VIEWPORT prevViewport;
    mContext->RSGetViewports(&numViewports, &prevViewport);
    UINT numScissors = 1;
    D3D11_RECT prevScissor;
    mContext->RSGetScissorRects(&numScissors, &prevScissor);
    ComPtr<ID3D11RasterizerState> prevRaster;
    mContext->RSGetState(prevRaster.GetAddressOf());
    ComPtr<ID3D11DepthStencilState> prevDepth;
    UINT prevStencilRef = 0;
    mContext->OMGetDepthStencilState(prevDepth.GetAddressOf(), &prevStencilRef);
    ComPtr<ID3D11BlendState> prevBlend;
    float prevBlendFactor[4] = {};
    UINT prevSampleMask = 0xFFFFFFFF;
    mContext->OMGetBlendState(prevBlend.GetAddressOf(), prevBlendFactor, &prevSampleMask);
    ComPtr<ID3D11InputLayout> prevLayout;
    mContext->IAGetInputLayout(prevLayout.GetAddressOf());
    D3D11_PRIMITIVE_TOPOLOGY prevTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    mContext->IAGetPrimitiveTopology(&prevTopology);
    ComPtr<ID3D11Buffer> prevVertexBuffer;
    UINT prevStride = 0;
    UINT prevOffset = 0;
    mContext->IAGetVertexBuffers(0, 1, prevVertexBuffer.GetAddressOf(), &prevStride, &prevOffset);
    ComPtr<ID3D11VertexShader> prevVs;
    mContext->VSGetShader(prevVs.GetAddressOf(), nullptr, nullptr);
    ComPtr<ID3D11PixelShader> prevPs;
    mContext->PSGetShader(prevPs.GetAddressOf(), nullptr, nullptr);
    ComPtr<ID3D11ShaderResourceView> prevSrv0;
    mContext->PSGetShaderResources(0, 1, prevSrv0.GetAddressOf());

    // Draw the game image through the shader into our output texture.
    mContext->OMSetRenderTargets(1, mPostRtv.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    mContext->RSSetViewports(1, &vp);
    mContext->RSSetState(mPostRaster.Get());
    mContext->OMSetDepthStencilState(mPostDepth.Get(), 0);
    const float noBlendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    mContext->OMSetBlendState(mPostBlend.Get(), noBlendFactor, 0xFFFFFFFF);
    mContext->IASetInputLayout(nullptr);
    mContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mContext->VSSetShader(mPostVs.Get(), nullptr, 0);
    mContext->PSSetShader(mPostPs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[1] = { mPostCb.Get() };
    mContext->PSSetConstantBuffers(3, 1, cbs);
    ID3D11ShaderResourceView* srvs[1] = { srcTex.resource_view.Get() };
    mContext->PSSetShaderResources(0, 1, srvs);
    mContext->Draw(3, 0);

    const bool depthDebug = cvars->GetInteger("gEnhancements.RemakeDepthDebug", 0) != 0;
    if (depthDebug) {
        if (!sDepthDbgTried) {
            sDepthDbgTried = true;
            ComPtr<ID3DBlob> dbgBlob, dbgErr;
            HRESULT dbgHr = mD3dCompile(kPostHlsl, strlen(kPostHlsl), nullptr, nullptr, nullptr, "PSDepth", "ps_4_0",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, dbgBlob.GetAddressOf(),
                                        dbgErr.ReleaseAndGetAddressOf());
            if (FAILED(dbgHr) ||
                FAILED(mDevice->CreatePixelShader(dbgBlob->GetBufferPointer(), dbgBlob->GetBufferSize(), nullptr,
                                                  sDepthDbgPs.GetAddressOf()))) {
                SPDLOG_ERROR("ReDemake depth debug (DX11): shader failed: {}",
                             dbgErr ? (const char*)dbgErr->GetBufferPointer() : "unknown");
            } else {
                SPDLOG_INFO("ReDemake depth debug (DX11): shader init OK");
            }
        }
        if (sDepthDbgPs.Get() != nullptr && srcFb.depth_stencil_srv.Get() != nullptr) {
            mContext->PSSetShader(sDepthDbgPs.Get(), nullptr, 0);
            ID3D11ShaderResourceView* depthSrv[1] = { srcFb.depth_stencil_srv.Get() };
            mContext->PSSetShaderResources(1, 1, depthSrv);
            mContext->Draw(3, 0);
            ID3D11ShaderResourceView* noDepthSrv[1] = { nullptr };
            mContext->PSSetShaderResources(1, 1, noDepthSrv);
            static bool sLoggedDepth = false;
            if (!sLoggedDepth) {
                sLoggedDepth = true;
                SPDLOG_INFO("ReDemake depth debug (DX11): drawing depth buffer of framebuffer {}", fb_id);
            }
        } else {
            static bool sLoggedNoDepth = false;
            if (!sLoggedNoDepth) {
                sLoggedNoDepth = true;
                SPDLOG_WARN("ReDemake depth debug (DX11): no depth view available on framebuffer {}", fb_id);
            }
        }
    }

    if (!depthDebug && cvars->GetInteger("gEnhancements.RemakeBloom", 0) != 0) {
        const float bloomIntensity =
            PostClampInt(cvars->GetInteger("gEnhancements.RemakeBloomIntensity", 35), 0, 100) / 100.0f * 1.5f;
        const float bloomThreshold =
            PostClampInt(cvars->GetInteger("gEnhancements.RemakeBloomThreshold", 70), 30, 95) / 100.0f;
        DrawBloom(width, height, bloomThreshold, bloomIntensity);
    }

    // Put everything back the way DrawTriangles expects it.
    ID3D11ShaderResourceView* noSrv[1] = { nullptr };
    mContext->PSSetShaderResources(0, 1, noSrv);
    mContext->OMSetRenderTargets(1, prevRtv.GetAddressOf(), prevDsv.Get());
    mContext->RSSetViewports(1, &prevViewport);
    mContext->RSSetScissorRects(1, &prevScissor);
    mContext->RSSetState(prevRaster.Get());
    mContext->OMSetDepthStencilState(prevDepth.Get(), prevStencilRef);
    mContext->OMSetBlendState(prevBlend.Get(), prevBlendFactor, prevSampleMask);
    mContext->IASetInputLayout(prevLayout.Get());
    mContext->IASetPrimitiveTopology(prevTopology);
    mContext->IASetVertexBuffers(0, 1, prevVertexBuffer.GetAddressOf(), &prevStride, &prevOffset);
    mContext->VSSetShader(prevVs.Get(), nullptr, 0);
    mContext->PSSetShader(prevPs.Get(), nullptr, 0);
    mContext->PSSetShaderResources(0, 1, prevSrv0.GetAddressOf());
    // Forget the cached texture view so the next game draw binds its own again.
    mLastResourceViews[0].Reset();

    static bool sLoggedDraw = false;
    if (!sLoggedDraw) {
        sLoggedDraw = true;
        SPDLOG_INFO("ReDemake post-process (DX11): drawing effect on framebuffer {} ({}x{})", fb_id, width, height);
    }
    return (void*)mPostSrv.Get();
}
