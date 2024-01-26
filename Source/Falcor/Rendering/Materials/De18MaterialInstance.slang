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
#include "Utils/Math/MathConstants.slangh"

__exported import Rendering.Materials.IMaterialInstance;
__exported import Rendering.Materials.AnisotropicGGX;
__exported import DiffRendering.SceneGradients;
__exported import DiffRendering.GradientIOWrapper;
import Rendering.Materials.Fresnel;
import Utils.Math.MathHelpers;

struct De18BSDF : IBSDF, IDifferentiable
{
    AnisotropicGGX D;
    float3 baseColor;
    float3 specular;

    [Differentiable]
    __init(const DiffMaterialData diffData)
    {
        uint offset = 0;
        diffData.read(baseColor, offset);
        float2 roughness = diffData.read<2>(offset);
        diffData.read(specular, offset);

        D = AnisotropicGGX(roughness);
    }

    /** Commputes Fresnel reflectance for a dielectric material.
     */
    [Differentiable]
    float3 Fresnel(float3 wo, float3 wi, float3 F0)
    {
        float cos = dot(normalize(wo + wi), wo);
        float sphg = pow(2.0, ((-5.55473 * cos) - 6.98316) * cos);
        return specular + (1.0 - specular) * sphg;
    }

    [Differentiable]
    float DTerm(float3 woLocal, float3 wiLocal, float alpha)
    {
        float3 whLocal = normalize(woLocal + wiLocal);
        float c2 = whLocal.z * whLocal.z;
        float a2 = alpha * alpha;
        float den = c2 * a2 + (1.0f - c2);
        return a2 / (M_PI * den * den);
    }

    [Differentiable]
    float _G1(float cos, float k)
    {
        return cos / (cos * (1.0 - k) + k);
    }

    [Differentiable]
    float GTerm(float3 woLocal, float3 wiLocal, float alpha)
    {
        float n_dot_v = woLocal.z;
        float n_dot_l = wiLocal.z;

        float k = alpha * 0.5 + 1e-6;
        return _G1(n_dot_v, k) * _G1(n_dot_l, k);

    }

    bool sampleReference<S : ISampleGenerator>( const ShadingData sd, const float3 wi, out float3 wo, out float pdf, out float3 weight, out uint lobeType, inout S sg )
    {
        weight = {};
        lobeType = {};

        // const bool isTransmissive = (getLobeTypes(sd) & (uint)LobeType::Transmission) != 0;

        wo = sample_cosine_hemisphere_concentric(sampleNext2D(sg), pdf); // pdf = cos(theta) / pi


        lobeType = (uint)LobeType::DiffuseReflection;

        return true;
    }

    [Differentiable]
    float3 eval<S : ISampleGenerator>(const float3 wiLocal, const float3 woLocal, inout S sg)
    {
        if (D.isSingular() || min(wiLocal.z, woLocal.z) < 0.0f) return float3(0.0f);

        float3 h = normalize(wiLocal + woLocal);
        float wiDotH = abs(dot(wiLocal, h));
        // float3 F = evalFresnelConductor(eta, k, wiDotH);
        // float3 F = evalFresnelGeneralizedSchlick(specular, 0.01f, 20, wiDotH);
        // float3 specular = pow(specular, 1 / 2.2);
        float alpha = D.alpha.x * D.alpha.x;
        // return D.G2(woLocal, wiLocal) * D.evalNDF(h) * F / (4.0f * abs(wiLocal.z));
        // float3 D = 1.0 / (1e-6 + M_PI * D.alpha.x * D.alpha.y h);
        // return 0.05 * (baseColor / M_PI + Fresnel(woLocal, wiLocal, specular) * D.evalNDF(h) * D.G2(woLocal, wiLocal) / (4.0f * abs(wiLocal.z)));
        return 0.05f * ( baseColor / M_PI * (1 - specular) + Fresnel(woLocal, wiLocal, specular) * DTerm(woLocal, wiLocal, alpha) * GTerm(woLocal, wiLocal, alpha) / (4.0f * wiLocal.z * woLocal.z));

    }

    bool sample<S : ISampleGenerator>(float3 wiLocal, out float3 wo, out float pdf, out float3 weight, out uint lobeType, inout S sg)
    {
        wo = {};
        pdf = {};
        weight = {};
        lobeType = {};

        if (wiLocal.z < 0.0f) return false;

        if (D.isSingular())
        {
            wo = float3(-wiLocal.x, -wiLocal.y, wiLocal.z);
            float3 h = normalize(wiLocal + wo);
            float wiDotH = abs(dot(wiLocal, h));
            weight = evalFresnelGeneralizedSchlick(specular, 0.01f, 20, wiDotH);
            lobeType = (uint)LobeType::DeltaReflection;
            pdf = 1.0f;
        }
        else
        {
            float3 h = D.sample(wiLocal, sg);

            float3 woLocal = reflect(-wiLocal, h);
            if (woLocal.z < 0.0f) return false;

            float wiDotH = abs(dot(wiLocal, h));
            // float3 F = evalFresnelConductor(eta, k, wiDotH);
            float3 F = evalFresnelGeneralizedSchlick(specular, 0.01f, 20, wiDotH);

            wo = woLocal;
            weight = F * D.G2(woLocal, wiLocal) / D.G1(wiLocal);
            lobeType = (uint)LobeType::SpecularReflection;
            pdf = D.evalPDF(wiLocal, h) / (4.0f * abs(dot(wiLocal, h)));
        }

        return true;
    }

    float evalPdf(const float3 wiLocal, const float3 woLocal)
    {
        if (D.isSingular() || min(wiLocal.z, woLocal.z) < 0.0f) return 0.0f;
        float3 h = normalize(wiLocal + woLocal);
        return D.evalPDF(wiLocal, h) / (4.0f * abs(dot(wiLocal, h)));
    }

    AlbedoContributions evalAlbedo(const float3 wi, const LobeType lobetype)
    {
        // float3 r = evalFresnelConductor(eta, k, wi.z);
        float3 r = float3(0.0f);
        return AlbedoContributions(r, 1.0f - r, 0.0f, 0.0f);
    }

    RoughnessInformation getRoughnessInformation(const float3 wi)
    {
        RoughnessInformation r;
        r.roughnessBSDFNotation = D.alpha;
        return r;
    }
};

struct De18MaterialInstance : MaterialInstanceBase, IMaterialInstance
{
    ShadingFrame sf;            ///< Shading frame in world space.
    De18BSDF bsdf;

    float3 eval<S : ISampleGenerator>(const ShadingData sd, const float3 wo, inout S sg)
    {
        float3 wiLocal = sf.toLocal(sd.V);
        float3 woLocal = sf.toLocal(wo);

        if (!isValidHemisphereReflection(sd, sf, wiLocal, woLocal, wo)) return float3(0.f);

        return bsdf.eval(wiLocal, woLocal, sg);
    }

    [Differentiable]
    float3 evalAD<S : ISampleGenerator>(const DiffMaterialData diffData, const ShadingData sd, const float3 wo, inout S sg)
    {
        float3 wiLocal = diffData.sf.toLocal(sd.V);
        float3 woLocal = diffData.sf.toLocal(wo);

        if (!isValidHemisphereReflection(sd, sf, wiLocal, woLocal, wo)) return float3(0.f);

        De18BSDF bsdfAD = De18BSDF(diffData);
        return bsdfAD.eval(wiLocal, woLocal, sg);
    }

    bool sample<S : ISampleGenerator>(const ShadingData sd, inout S sg, out BSDFSample result, bool useImportanceSampling = true)
    {
        float3 wiLocal = sf.toLocal(sd.V);
        float3 woLocal = {};

        bool valid = bsdf.sample(wiLocal, woLocal, result.pdf, result.weight, result.lobeType, sg);
        result.wo = sf.fromLocal(woLocal);

        if (!isValidHemisphereReflection(sd, sf, wiLocal, woLocal, result.wo) || result.pdf == 0.f) return false;

        return valid;
    }

    float evalPdf(const ShadingData sd, const float3 wo, bool useImportanceSampling = true)
    {
        float3 wiLocal = sf.toLocal(sd.V);
        float3 woLocal = sf.toLocal(wo);

        if (!isValidHemisphereReflection(sd, sf, wiLocal, woLocal, wo)) return 0.f;

        return bsdf.evalPdf(wiLocal, woLocal);
    }

    BSDFProperties getProperties(const ShadingData sd)
    {
        BSDFProperties p = {};

        p.guideNormal = sf.N;
        p.roughness = bsdf.D.getIsotropicRoughness();
        p.specularReflectionAlbedo = float3(1.0f); // TODO: Compute approximate albedo from eta/k

        return p;
    }

    uint getLobeTypes(const ShadingData sd)
    {
        return (uint)(bsdf.D.isSingular() ? LobeType::DeltaReflection : LobeType::SpecularReflection);
    }

};
