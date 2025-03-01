/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
__exported import Rendering.Materials.IMaterial;
__exported import Rendering.Materials.GaussMaterialInstance;
import Scene.Material.GaussMaterialParamLayout;
import Scene.Material.GaussMaterialData;
import Scene.Material.ShadingUtils;

/** Implementation of measured MERL material.
 */
struct GaussMaterial : MaterialBase, IMaterial
{
    GaussMaterialData data;

    // Specify that the associated `MaterialInstance` type is `GaussMaterialInstance`.
    typedef GaussMaterialInstance MaterialInstance;

    GaussMaterialInstance setupMaterialInstance(const MaterialSystem ms, const ShadingData sd, const ITextureSampler lod, const uint hints)
    {
        // Compute final shading frame.
        ShadingFrame sf = sd.frame;
        flipShadingNormal(sd, sf);
        if (isAdjustShadingNormalEnabled(hints))
        {
            adjustShadingNormal(sd, sf);
        }


        // // Evaluate the albedo for the current incident (view) direction.
        // float3 albedo = {};
        // float cosTheta = dot(sf.N, sd.V);

        // if (cosTheta >= kMinCosTheta)
        // {
        //     // The albedo is precomputed in a lookup table, which we sample using piecewise linear interpolation.
        //     // The precompued albedo values are sampled at cosTheta = (1,2,...N)/N, where N is the LUT size.
        //     // We offset the texture coordinate by -0.5/N to get correct interpolation of the two nearest samples.
        //     float u = cosTheta - 0.5f / MERLMaterialData::kAlbedoLUTSize;

        //     SamplerState s = ms.getTextureSampler(data.samplerID);
        //     let explicitLod = ExplicitLodTextureSampler(0.f);
        //     albedo = ms.sampleTexture(data.texAlbedoLUT, s, float2(u, 0.5f), float4(0.5f), explicitLod).rgb;
        // }

        return GaussMaterialInstance(sf);
    }

    [Differentiable]
    GaussMaterialInstance setupDiffMaterialInstance(out DiffMaterialData diffData, const MaterialSystem ms, const ShadingData sd, const ITextureSampler lod, const uint hints)
    {
        SamplerState s = ms.getTextureSampler(header.getDefaultTextureSamplerID());
        ShadingFrame sf = sd.frame;
        uint hashIndex = hashFunction(sd.threadID, gSceneGradients.getHashSize(GradientType::Material));
        GradientIOWrapper gradIO = GradientIOWrapper(GradientType::Material, sd.materialGradOffset, hashIndex);

        float3 baseColor = gradIO.getFloat(data.baseColor, GaussMaterialParamLayout.baseColor);

        float3 covDiag = gradIO.getFloat(data.covDiag, GaussMaterialParamLayout.covDiag);

        float3 covOffDiag = gradIO.getFloat(data.covOffDiag, GaussMaterialParamLayout.covOffDiag);

        float alpha = gradIO.getFloat(data.alpha, GaussMaterialParamLayout.alpha);

        flipShadingNormal(sd, sf);
        if (isAdjustShadingNormalEnabled(hints))
        {
            adjustShadingNormal(sd, sf);
        }

        uint offset = 0;
        diffData.write(baseColor, offset); // offset 0 1 2
        diffData.write(covDiag, offset); // offset 3 4 5
        diffData.write(covOffDiag, offset); // offset 6 7 8
        diffData.write(alpha, offset);      // offset 9

        return GaussMaterialInstance(sf);
    }
};
