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
__exported import Rendering.Materials.De18MaterialInstance;
import Scene.Material.De18MaterialParamLayout;
import Scene.Material.BasicMaterialData;
import Scene.Material.ShadingUtils;

struct De18Material : MaterialBase, IMaterial
{
    BasicMaterialData data;

    typedef De18MaterialInstance MaterialInstance;

    De18MaterialInstance setupMaterialInstance(const MaterialSystem ms, const ShadingData sd, const ITextureSampler lod, const uint hints)
    {
        SamplerState s = ms.getTextureSampler(header.getDefaultTextureSamplerID());

        // Abuse these colors and assume baseColor = baseColor, roughness = transmission, specular = specular, tangent = emissive
        const float3 baseColor = ms.sampleTexture(data.texBaseColor, s, sd.uv, data.baseColor, lod).rgb;
        const float3 specular = ms.sampleTexture(data.texSpecular, s, sd.uv, data.specular, lod).rgb;
        const float2 roughness = ms.sampleTexture(data.texTransmission, s, sd.uv, float4(data.transmission, 1.0f), lod).rg;



        // Compute final shading frame.
        ShadingFrame sf = sd.frame;
        if (isNormalMappingEnabled(hints))
        {
            float3 encodedNormal = ms.sampleTexture(data.texNormalMap, s, sd.uv, float4(0.f), lod).rgb;
            // encodedNormal = pow(encodedNormal, 2.2);
            float3 encodedTangent = ms.sampleTexture(data.texEmissive, s, sd.uv, float4(data.emissive, 1.0f), lod).rgb;
            encodedTangent = pow(encodedTangent, 2.2);
            sf = computeShadingFrameFromNormalMap(sd, NormalMapType::RGB, encodedNormal);
        }

        flipShadingNormal(sd, sf);
        if (isAdjustShadingNormalEnabled(hints))
        {
            adjustShadingNormal(sd, sf);
        }

        De18MaterialInstance mi = { sf, { AnisotropicGGX(roughness), baseColor, specular}};

        return mi;
    }

    [Differentiable]
    De18MaterialInstance setupDiffMaterialInstance(out DiffMaterialData diffData, const MaterialSystem ms, const ShadingData sd, const ITextureSampler lod, const uint hints)
    {
        SamplerState s = ms.getTextureSampler(header.getDefaultTextureSamplerID());

        uint hashIndex = hashFunction(sd.threadID, gSceneGradients.getHashSize(GradientType::Material));
        GradientIOWrapper gradIO = GradientIOWrapper(GradientType::Material, sd.materialGradOffset, hashIndex);

        // Abuse these colors and assume eta = baseColor, k = transColor
        // TODO: Handle texture gradients.
        const float4 _baseColor = no_diff ms.sampleTexture(data.texBaseColor, s, sd.uv, data.baseColor, lod);
        float3 baseColor = gradIO.getFloat(_baseColor.rgb, De18MaterialParamLayout.baseColor);
        const float4 _roughness = no_diff ms.sampleTexture(data.texTransmission, s, sd.uv, float4(data.transmission, 1.0f), lod);
        float2 roughness = gradIO.getFloat(_roughness.rg, De18MaterialParamLayout.roughness);
        const float4 _specular = no_diff ms.sampleTexture(data.texSpecular, s, sd.uv, data.specular, lod);
        float3 specular = gradIO.getFloat(_specular.rgb, De18MaterialParamLayout.specular);

        // Compute final shading frame.
        ShadingFrame sf = sd.frame;
        if (isNormalMappingEnabled(hints))
        {
            float3 encodedNormal = ms.sampleTexture(data.texNormalMap, s, sd.uv, float4(0.f), lod).rgb;
            // TODO: Allow differentiation here.
            // We don't support it because differentiating normal maps is harder due to additional discontinuities.
            float3 encodedTangent = ms.sampleTexture(data.texEmissive, s, sd.uv, float4(data.emissive, 1.0f), lod).rgb;
            sf = no_diff computeShadingFrameFromNormalMap(sd, NormalMapType::RGB, encodedNormal);
        }
        flipShadingNormal(sd, sf);
        if (isAdjustShadingNormalEnabled(hints))
        {
            no_diff adjustShadingNormal(sd, sf);
        }

        De18MaterialInstance mi = { sf, { AnisotropicGGX(roughness), baseColor, specular } };
        diffData.sf = sf;

        uint offset = 0;
        // 0--3: eta (baseColor)
        diffData.write(baseColor, offset);
        // 3--6: k (transColor)
        diffData.write(roughness, offset);
        // 6--8: roughness
        diffData.write(specular, offset);

        return mi;
    }
};
