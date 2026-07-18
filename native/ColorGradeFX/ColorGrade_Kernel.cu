/*
 * ColorGrade_Kernel.cu - GPU trilinear 3D-LUT apply kernel (the ported sampleLut).
 *
 * Authored once with the SDK's cross-platform GF_KERNEL_FUNCTION macros; the DirectX
 * build preprocesses it (via ColorGrade_Kernel.chlsl -> ParseHLSL -> DXC) into a .cso
 * root-signatured compute shader. The CUDA host launcher is provided but only compiled
 * when nvcc is used (CUDA is optional polish; DirectX is the primary GPU path).
 *
 * Numerically mirrors lut/CubeLut.h::sampleLut so the GPU output tracks the CPU path:
 *   - clamp each channel to [0,1] for the lookup, blend against the ORIGINAL channel;
 *   - lower node = min(floor(v), size-2); node index ((b*size+g)*size+r)*3 + c.
 * Note GPU buffers are BGRA (x=B, y=G, z=R), so R/G/B map to px.z/px.y/px.x.
 */
#ifndef COLORGRADE_KERNEL
#define COLORGRADE_KERNEL

#include "PrGPU/KernelSupport/KernelCore.h"    // includes KernelWrapper.h
#include "PrGPU/KernelSupport/KernelMemory.h"

#if GF_DEVICE_TARGET_DEVICE

GF_DEVICE_FUNCTION float CG_Clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

GF_KERNEL_FUNCTION(ColorGradeKernel,
    ((GF_PTR_READ_ONLY(float4))(inSrc))
    ((GF_PTR_READ_ONLY(float))(inLut))
    ((GF_PTR(float4))(outDst)),
    ((int)(inSrcPitch))
    ((int)(inDstPitch))
    ((int)(in16f))
    ((unsigned int)(inWidth))
    ((unsigned int)(inHeight))
    ((int)(inLutSize))
    ((float)(inStrength)),
    ((uint2)(inXY)(KERNEL_XY)))
{
    if (inXY.x < inWidth && inXY.y < inHeight)
    {
        float4 pixel = ReadFloat4(inSrc, inXY.y * inSrcPitch + inXY.x, !!in16f);

        // GPU pixels are BGRA: pixel.z = R, pixel.y = G, pixel.x = B.
        float origR = pixel.z;
        float origG = pixel.y;
        float origB = pixel.x;

        int n = inLutSize - 1;
        float vr = CG_Clamp01(origR) * (float)n;
        float vg = CG_Clamp01(origG) * (float)n;
        float vb = CG_Clamp01(origB) * (float)n;

        int ri = (int)floor(vr); if (ri > n - 1) ri = n - 1;
        int gi = (int)floor(vg); if (gi > n - 1) gi = n - 1;
        int bi = (int)floor(vb); if (bi > n - 1) bi = n - 1;
        float rf = vr - (float)ri;
        float gf = vg - (float)gi;
        float bf = vb - (float)bi;

        float graded[3];
        for (int c = 0; c < 3; c++)
        {
            // node (R,G,B) channel c -> flat index ((b*size+g)*size+r)*3 + c
            int i000 = ((bi       * inLutSize + gi    ) * inLutSize + ri    ) * 3 + c;
            int i100 = ((bi       * inLutSize + gi    ) * inLutSize + ri + 1) * 3 + c;
            int i010 = ((bi       * inLutSize + gi + 1) * inLutSize + ri    ) * 3 + c;
            int i110 = ((bi       * inLutSize + gi + 1) * inLutSize + ri + 1) * 3 + c;
            int i001 = (((bi + 1) * inLutSize + gi    ) * inLutSize + ri    ) * 3 + c;
            int i101 = (((bi + 1) * inLutSize + gi    ) * inLutSize + ri + 1) * 3 + c;
            int i011 = (((bi + 1) * inLutSize + gi + 1) * inLutSize + ri    ) * 3 + c;
            int i111 = (((bi + 1) * inLutSize + gi + 1) * inLutSize + ri + 1) * 3 + c;

            float c00 = ReadFloat(inLut, i000, false) * (1.0f - rf) + ReadFloat(inLut, i100, false) * rf;
            float c10 = ReadFloat(inLut, i010, false) * (1.0f - rf) + ReadFloat(inLut, i110, false) * rf;
            float c01 = ReadFloat(inLut, i001, false) * (1.0f - rf) + ReadFloat(inLut, i101, false) * rf;
            float c11 = ReadFloat(inLut, i011, false) * (1.0f - rf) + ReadFloat(inLut, i111, false) * rf;
            float c0 = c00 * (1.0f - gf) + c10 * gf;
            float c1 = c01 * (1.0f - gf) + c11 * gf;
            graded[c] = c0 * (1.0f - bf) + c1 * bf;
        }

        pixel.z = origR + (graded[0] - origR) * inStrength;
        pixel.y = origG + (graded[1] - origG) * inStrength;
        pixel.x = origB + (graded[2] - origB) * inStrength;
        // alpha (pixel.w) unchanged

        WriteFloat4(pixel, outDst, inXY.y * inDstPitch + inXY.x, !!in16f);
    }
}
#endif  // GF_DEVICE_TARGET_DEVICE

#if __NVCC__
// CUDA host launcher (only built when nvcc compiles this file; DirectX ignores it).
void ColorGrade_CUDA(
    float const* src, float const* lut, float* dst,
    unsigned int srcPitch, unsigned int dstPitch, int is16f,
    unsigned int width, unsigned int height, int lutSize, float strength)
{
    dim3 blockDim(16, 16, 1);
    dim3 gridDim((width + blockDim.x - 1) / blockDim.x, (height + blockDim.y - 1) / blockDim.y, 1);
    ColorGradeKernel<<<gridDim, blockDim, 0>>>(
        (float4 const*)src, (float const*)lut, (float4*)dst,
        srcPitch, dstPitch, is16f, width, height, lutSize, strength);
    cudaDeviceSynchronize();
}
#endif  // __NVCC__

#endif  // COLORGRADE_KERNEL
