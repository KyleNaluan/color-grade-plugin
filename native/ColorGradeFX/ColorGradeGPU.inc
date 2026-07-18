/*
 * ColorGradeGPU.inc - DirectX 12 GPU path for the Color Grade effect.
 *
 * Included from ColorGrade.cpp only when HAS_HLSL is defined. Implements
 * GPUDeviceSetup/Setdown (load the DXC-compiled ColorGradeKernel.cso) and
 * SmartRenderGPU (upload the resolved LUT as a raw SRV, dispatch the compute shader).
 * DirectX is the primary GPU path; CUDA/OpenCL are intentionally not wired here (they
 * are optional polish, and DirectX is vendor-neutral). AE falls back to the CPU
 * SmartRender path automatically when the GPU path is unavailable.
 *
 * Requires DirectX_Assets/ColorGradeKernel.{cso,rs} to ship next to the .aex (the
 * build's post-step copies them) and PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING in both
 * Global Flags and the PiPL - otherwise AE silently uses the CPU path.
 */

#include "DirectXUtils.h"
#include <d3d12.h>
#include <wrl.h>
#include <vector>

inline PF_Err DXErr(bool ok) { return ok ? PF_Err_NONE : PF_Err_INTERNAL_STRUCT_DAMAGED; }
#define DX_ERR(FUNC) ERR(DXErr(FUNC))

struct DirectXGPUData {
    DXContextPtr    mContext;
    ShaderObjectPtr mColorGradeShader;
};

// Param constant buffer - order MUST match the kernel's scalar argument list.
typedef struct {
    int   mSrcPitch;
    int   mDstPitch;
    int   m16f;
    int   mWidth;
    int   mHeight;
    int   mLutSize;
    float mStrength;
} ColorGradeGPUParams;

static size_t DivideRoundUp(size_t v, size_t d) { return v ? (v + d - 1) / d : 0; }

static PF_Err GPUDeviceSetup(PF_InData* in_data, PF_OutData* out_data, PF_GPUDeviceSetupExtra* extra) {
    PF_Err err = PF_Err_NONE;

    PF_GPUDeviceInfo device_info;
    AEFX_CLR_STRUCT(device_info);

    AEFX_SuiteScoper<PF_HandleSuite1> handle_suite(in_data, kPFHandleSuite, kPFHandleSuiteVersion1, out_data);
    AEFX_SuiteScoper<PF_GPUDeviceSuite1> gpu_suite(in_data, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1, out_data);
    gpu_suite->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &device_info);

    if (extra->input->what_gpu == PF_GPU_Framework_DIRECTX) {
        PF_Handle gpu_dataH = handle_suite->host_new_handle(sizeof(DirectXGPUData));
        if (!gpu_dataH) return PF_Err_OUT_OF_MEMORY;
        DirectXGPUData* dx = reinterpret_cast<DirectXGPUData*>(*gpu_dataH);
        new (dx) DirectXGPUData();  // construct the shared_ptrs in place

        dx->mContext = std::make_shared<DXContext>();
        dx->mColorGradeShader = std::make_shared<ShaderObject>();

        DX_ERR(dx->mContext->Initialize(
            (ID3D12Device*)device_info.devicePV,
            (ID3D12CommandQueue*)device_info.command_queuePV));

        std::wstring csoPath, sigPath;
        DX_ERR(GetShaderPath(L"ColorGradeKernel", csoPath, sigPath));
        DX_ERR(dx->mContext->LoadShader(csoPath.c_str(), sigPath.c_str(), dx->mColorGradeShader));

        extra->output->gpu_data = gpu_dataH;
        out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    }
    // Other frameworks (CUDA/OpenCL) are unsupported here -> AE uses the CPU path.
    return err;
}

static PF_Err GPUDeviceSetdown(PF_InData* in_data, PF_OutData* out_data, PF_GPUDeviceSetdownExtra* extra) {
    PF_Err err = PF_Err_NONE;
    if (extra->input->what_gpu == PF_GPU_Framework_DIRECTX) {
        PF_Handle gpu_dataH = (PF_Handle)extra->input->gpu_data;
        DirectXGPUData* dx = reinterpret_cast<DirectXGPUData*>(*gpu_dataH);
        dx->mContext.reset();
        dx->mColorGradeShader.reset();
        dx->~DirectXGPUData();

        AEFX_SuiteScoper<PF_HandleSuite1> handle_suite(in_data, kPFHandleSuite, kPFHandleSuiteVersion1, out_data);
        handle_suite->host_dispose_handle(gpu_dataH);
    }
    return err;
}

// Create an UPLOAD-heap buffer holding the LUT floats (CPU-writable, GPU-readable as a
// raw SRV). Kept alive by the caller until Execute() (which waits on the fence) returns.
static Microsoft::WRL::ComPtr<ID3D12Resource> CreateLutBuffer(
    ID3D12Device* device, const std::vector<float>& data, PF_Err& err) {
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    const UINT bytes = static_cast<UINT>(data.size() * sizeof(float));
    if (bytes == 0) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; return res; }

    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                   D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes, 1, 1, 1,
                                 DXGI_FORMAT_UNKNOWN, { 1, 0 },
                                 D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(res.GetAddressOf())))) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
        return res;
    }
    void* mapped = nullptr;
    if (FAILED(res->Map(0, nullptr, &mapped))) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; res.Reset(); return res; }
    memcpy(mapped, data.data(), bytes);
    res->Unmap(0, nullptr);
    return res;
}

static PF_Err SmartRenderGPU(PF_InData* in_data, PF_OutData* out_data, PF_PixelFormat pixel_format,
                             PF_EffectWorld* input, PF_EffectWorld* output,
                             PF_SmartRenderExtra* extra, CG_RenderData* d) {
    PF_Err err = PF_Err_NONE;

    AEFX_SuiteScoper<PF_GPUDeviceSuite1> gpu_suite(in_data, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1, out_data);

    if (pixel_format != PF_PixelFormat_GPU_BGRA128) return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    const A_long bytes_per_pixel = 16;

    PF_GPUDeviceInfo device_info;
    ERR(gpu_suite->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &device_info));

    void* src_mem = 0;
    void* dst_mem = 0;
    ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref, input, &src_mem));
    ERR(gpu_suite->GetGPUWorldData(in_data->effect_ref, output, &dst_mem));

    ColorGradeGPUParams params;
    params.mWidth = input->width;
    params.mHeight = input->height;
    A_long src_row_bytes = input->rowbytes;
    A_long dst_row_bytes = output->rowbytes;
    params.mSrcPitch = src_row_bytes / bytes_per_pixel;
    params.mDstPitch = dst_row_bytes / bytes_per_pixel;
    params.m16f = 0;  // BGRA128 is 32f
    params.mLutSize = d->lut.size;
    params.mStrength = d->strength;

    if (!err && extra->input->what_gpu == PF_GPU_Framework_DIRECTX) {
        PF_Handle gpu_dataH = (PF_Handle)extra->input->gpu_data;
        DirectXGPUData* dx = reinterpret_cast<DirectXGPUData*>(*gpu_dataH);

        Microsoft::WRL::ComPtr<ID3D12Resource> lutBuf =
            CreateLutBuffer(dx->mContext->mDevice.Get(), d->lut.data, err);

        if (!err) {
            // 4 descriptors: 1 CBV (params) + 1 UAV (dst) + 2 SRV (src, lut).
            DXShaderExecution exec(dx->mContext, dx->mColorGradeShader, 4);
            DX_ERR(exec.SetParamBuffer(&params, sizeof(ColorGradeGPUParams)));
            DX_ERR(exec.SetUnorderedAccessView((ID3D12Resource*)dst_mem, params.mHeight * dst_row_bytes));
            DX_ERR(exec.SetShaderResourceView((ID3D12Resource*)src_mem, params.mHeight * src_row_bytes));
            DX_ERR(exec.SetShaderResourceView(lutBuf.Get(), static_cast<UINT>(d->lut.data.size() * sizeof(float))));
            DX_ERR(exec.Execute((UINT)DivideRoundUp(params.mWidth, 16), (UINT)DivideRoundUp(params.mHeight, 16)));
        }
    }
    return err;
}
