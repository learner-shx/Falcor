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
#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Rendering.Lights.EmissiveLightSampler;
import Rendering.Lights.EmissiveLightSamplerHelpers;
import Rendering.Lights.LightHelpers;
import Scene.Raytracing;
import Scene.Intersection;
import Scene.RaytracingInline;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Debug.PixelDebug;
import Utils.Debug.PixelDebugTypes;
import DiffRendering.SharedTypes;
import DiffRendering.DiffSceneIO;
import DiffRendering.DiffSceneQuery;

import Scene.Material.GaussMaterialData;

import DiffRendering.DiffDebug;

__exported import StaticParams;
__exported import Params;
import WarpedAreaReparam;

struct PathData : IDifferentiable
{
    float3 radiance;
    float3 thp;
    uint length;
    bool terminated;

    // For MIS.
    no_diff float3 normal;
    no_diff float pdf;

    [Differentiable]
    __init()
    {
        this.radiance = float3(0.f);
        this.thp = float3(1.f);
        this.length = 0;
        this.terminated = false;
        this.normal = float3(0.f);
        this.pdf = 0.f;
    }
};

struct LightSample
{
    float3 Li;      ///< Incident radiance at the shading point (unshadowed). This is already divided by the pdf.
    float pdf;      ///< Pdf with respect to solid angle at the shading point.
    float3 origin;  ///< Ray origin for visibility evaluation (offseted to avoid self-intersection).
    float distance; ///< Ray distance for visibility evaluation (shortened to avoid self-intersection).
    float3 dir;     ///< Ray direction for visibility evaluation (normalized).

    // For differentiable evaluation.
    float3 lightPos;
    float3 lightNormal;

    Ray getVisibilityRay() { return Ray(origin, dir, 0.f, distance); }
};

float evalMIS(float n0, float p0, float n1, float p1)
{
    // Power two heuristic
    float q0 = (n0 * p0) * (n0 * p0);
    float q1 = (n1 * p1) * (n1 * p1);
    return q0 / (q0 + q1);
}

/**
 * Returns the shading normal flipped for backfacing hits on double-sided materials.
 * This reproduces the legacy behavior before the conventions were updated.
 */
[Differentiable]
[PreferRecompute]
float3 getFlippedShadingNormal(const ShadingData sd)
{
    return !sd.frontFacing && sd.mtl.isDoubleSided() ? -sd.frame.N : sd.frame.N;
}

[Differentiable]
[PreferRecompute]
float3 evalDirectAnalytic( SceneQueryAD sceneQuery, const ShadingData sd, const IMaterialInstance mi, DiffMaterialData diffData, const float3 rayOrigin, inout SampleGenerator sg)
{
    // Use all analytic light sources.
    const uint lightCount = gScene.getLightCount();
    float invPdf = 1.0f / lightCount; // Light selection pdf = 1.0f / lightCount.

    float3 contribution = float3(0.0f);

    // contribution += mi.evalAD(diffData, sd, rayOrigin, sg);
    [MaxIters(100)]
    for (uint lightIndex = 0; lightIndex < lightCount; lightIndex++)
    {
        // Sample local light source.
        AnalyticLightSample ls;
        bool valid = sampleLight(rayOrigin, gScene.getLight(lightIndex), sg, ls);

        // Reject sample if lower hemisphere.
        if (valid && dot(ls.dir, getFlippedShadingNormal(sd)) > kMinCosTheta)
        {
            // Test visibility by tracing a shadow ray.
            Ray shadowRay = Ray(ls.posW, ls.dir, 0.f, ls.distance);;
            bool visible = sceneQuery.traceVisibilityRay(shadowRay);

            contribution += mi.evalAD(diffData, sd, ls.dir, sg) * ls.Li * invPdf;
        }
    }
    return contribution;
}

[Differentiable]
[PreferRecompute]
ShadingData loadShadingData(
    SceneQueryAD sceneQuery,
    HitInfo hit,
    IntersectionAD isect,
    const float3 rayOrigin,
    const float3 rayDir,
    const ITextureSampler lod
)
{
    VertexData v = {};
    uint materialID = {};
    ShadingData sd;
    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        v = gScene.getVertexData(triangleHit);
        materialID = gScene.getMaterialID(isect.instanceID);

        // Overwrite some fields to enable auto-diff.
        sd = no_diff gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);
        sd.V = -rayDir;
        bool valid;
        sd.frame = ShadingFrame::createSafe(isect.normalW, v.tangentW, valid);

        // Set offset value for the material gradient propagation.
        sd.materialGradOffset = materialID * DiffMaterialData::kMaterialParamCount;
    }

    else if (hit.getType() == HitType::Gauss)
    {
        const GaussHit gaussHit = hit.getGaussHit();
        // v = gScene.getVertexData(gaussHit);
        float3 posW = gScene.getCustomPrimitiveAABB(isect.instanceID).center();
        CustomPrimitiveDesc desc = gScene.getCustomPrimitiveDesc(isect.instanceID);

        materialID = desc.materialID;

        // Overwrite some fields to enable auto-diff.
        sd = no_diff gScene.materials.prepareShadingData(posW, materialID, -rayDir, lod);
        sd.V = -rayDir;

        // Set offset value for the material gradient propagation.
        sd.materialGradOffset = materialID * DiffMaterialData::kMaterialParamCount;
        bool valid;
        sd.frame = ShadingFrame::createSafe(isect.normalW, float4(0.0f), valid);
    }


    return sd;
}

[Differentiable]
[PreferRecompute]
IMaterialInstance getDiffMaterialInstance(out DiffMaterialData diffData, const ShadingData sd, const ITextureSampler lod)
{
    let material = gScene.materials.getMaterial(sd.materialID);
    let mi = material.setupDiffMaterialInstance(diffData, gScene.materials, sd, lod);
    return mi;
}

[Differentiable]
[PreferRecompute]
bool generateScatterRay(
    SceneQueryAD sceneQuery,
    ShadingData sd,
    IMaterialInstance mi,
    DiffMaterialData diffData,
    IntersectionAD isect,
    inout RayAD ray,
    inout PathData pathData,
    inout SampleGenerator sg,
    bool shouldReparameterize = true
)
{
    // Sample material.
    BSDFSample bsdfSample = {};
    if (mi.sample(sd, sg, bsdfSample, kUseBSDFSampling))
    {
        ray.direction = bsdfSample.wo;
        ray.origin = computeRayOrigin(isect.posW, ((sd.frontFacing) ? sd.faceN : -sd.faceN));

        if (shouldReparameterize)
        {
            float weight = reparameterizeRay(sceneQuery, ray, sg);
            pathData.thp *= weight;
        }

        float3 weight = mi.evalAD(diffData, sd, ray.direction, sg) / bsdfSample.pdf;
        pathData.thp *= weight;

        // Save normal and pdf for MIS.
        pathData.normal = sd.getOrientedFaceNormal();
        pathData.pdf = bsdfSample.pdf;

        return any(weight > 0.f);
    }

    return false;
}

// Generate a light sample on the emissive geometry.
// Assume the area lights are static (we don't allow differentiation of dynamic area lights for now).
[PreferRecompute]
bool generateEmissiveSample(
    IntersectionAD isect,
    EmissiveLightSampler emissiveSampler,
    bool upperHemisphere,
    inout SampleGenerator sg,
    out LightSample ls
)
{
    ls = {}; // Default initialization to avoid divergence at returns.
    if (!kUseEmissiveLights)
        return false;

    TriangleLightSample tls;
    if (!emissiveSampler.sampleLight(isect.posW, isect.normalW, upperHemisphere, sg, tls))
        return false;

    // Setup returned sample.
    ls.Li = tls.pdf > 0.f ? tls.Le / tls.pdf : float3(0);
    ls.pdf = tls.pdf;
    // Offset shading and light position to avoid self-intersection.
    float3 lightPos = computeRayOrigin(tls.posW, tls.normalW);
    ls.origin = computeRayOrigin(isect.posW, isect.normalW);
    float3 toLight = lightPos - ls.origin;
    ls.distance = length(toLight);
    ls.dir = normalize(toLight);

    ls.lightPos = tls.posW;
    ls.lightNormal = tls.normalW;

    return any(ls.Li > 0.f);
}

[Differentiable]
[PreferRecompute]
void computeNEE(
    SceneQueryAD sceneQuery,
    ShadingData sd,
    IMaterialInstance mi,
    DiffMaterialData diffData,
    IntersectionAD isect,
    LightSample ls,
    inout PathData pathData,
    inout SampleGenerator sg,
    bool shouldReparameterize = true
)
{
    float3 wo = ls.lightPos - isect.posW;
    float dist = length(wo);
    wo /= dist;

    RayAD ray = RayAD(computeRayOrigin(isect.posW, ((sd.frontFacing) ? sd.faceN : -sd.faceN)), wo, sceneQuery.gradInfo.pixel);

    float3 thp = pathData.thp;

    if (shouldReparameterize)
    {
        float weight = reparameterizeRay(sceneQuery, ray, sg);
        thp *= weight;
    }

    // Note: ls.Li and ls.pdf are already w.r.t. the solid angle.
    float areaToSolidAngle = (dist * dist) / dot(ls.lightNormal, -ray.direction);
    float3 bsdfValue = mi.evalAD(diffData, sd, ray.direction, sg);
    float3 weight = ls.Li * bsdfValue * areaToSolidAngle / detach(areaToSolidAngle);
    // float3 weight = bsdfValue;

    if (kUseMIS)
    {
        float scatterPdf = mi.evalPdf(sd, ls.dir, kUseBSDFSampling);
        weight *= no_diff evalMIS(1, ls.pdf, 1, scatterPdf);
    }

    pathData.radiance += thp * weight;
}

// void packHeader( inout PackedHitInfo packed, const HitType type, const GeometryInstanceID instanceID, const uint primitiveIndex, uint kHeaderBits, uint kTypeOffset, uint kInstanceIDOffset)
// {
//     if (kHeaderBits <= 32)
//     {
//         packed[0] = (uint(type) << kTypeOffset) | (instanceID.index << kInstanceIDOffset) | primitiveIndex;
//     }
//     else
//     {
//         packed[0] = (uint(type) << kTypeOffset) | instanceID.index;
//         packed[1] = primitiveIndex;
//     }
// }

// void unpackHeader( const PackedHitInfo packed, out GeometryInstanceID instanceID, out uint primitiveIndex, uint kHeaderBits, uint kTypeOffset, uint kInstanceIDOffset, uint kInstanceIDBits, uint kPrimitiveIndexBits)
// {
//     if (kHeaderBits <= 32)
//     {
//         instanceID.index = { (packed[0] >> kInstanceIDOffset) & ((1 << kInstanceIDBits) - 1) };
//         primitiveIndex = packed[0] & ((1 << kPrimitiveIndexBits) - 1);
//     }
//     else
//     {
//         instanceID.index = { packed[0] & ((1 << kInstanceIDBits) - 1) };
//         primitiveIndex = packed[1];
//     }
// }


[Differentiable]
[PreferRecompute]
void handleHit(
    SceneQueryAD sceneQuery,
    IntersectionAD isect,
    EmissiveLightSampler emissiveSampler,
    inout PathData pathData,
    inout RayAD ray,
    inout SampleGenerator sg
)
{
    let lod = ExplicitLodTextureSampler(0.f);
    HitInfo hit;
    TriangleHit triHit = { isect.instanceID, isect.triangleID, isect.barycentrics.y, isect.barycentrics.z };
    GaussHit gaussHit = { isect.instanceID, isect.triangleID, isect.barycentrics.y, isect.barycentrics.z };

    if (gScene.getGeometryInstanceType(isect.instanceID) == GeometryType::Custom)
        hit = HitInfo(gaussHit);
    else
        hit = HitInfo(triHit);

    // Load shading data.
    ShadingData sd;


    sd = loadShadingData(sceneQuery, hit, isect, ray.origin, ray.direction, lod);

    // Create differentiable material instance.
    DiffMaterialData diffData = DiffMaterialData();
    let mi = getDiffMaterialInstance(diffData, sd, lod);

    print<uint>("instanceID", isect.instanceID.index);
    print<uint>("materialID", sd.materialID);
    print<uint>("CustomPrimitiveCount", gScene.customPrimitiveInstanceCount);
    print<uint>("CustomPrimitiveOffset", gScene.customPrimitiveInstanceOffset);

    print<uint>("gDiffDebug.id.x", gDiffDebug.id.x);
    GeometryInstanceID testID;
    uint testPId;

    if (gScene.getGeometryInstanceType(isect.instanceID) == GeometryType::Custom)
    {
        // CustomPrimitiveDesc desc = gScene.getCustomPrimitiveDesc(isect.instanceID);
        // float alpha;
        // uint offset = 0;
        // diffData.read(alpha, offset);
        // float epower = exp(-10 * gaussHit.barycentrics.x);
        // alpha *= epower;
        // pathData.radiance += mi.evalAD(diffData, sd, float3(0.0), sg) * pathData.thp * epower;
        // pathData.thp *= (1 - alpha);

        CustomPrimitiveDesc desc = gScene.getCustomPrimitiveDesc(isect.instanceID);
        float alpha;
        float3 baseColor;
        uint offset = 0;
        // diffData.read(alpha, offset);
        diffData.read(baseColor, offset);
        // float epower = exp(-10 * gaussHit.barycentrics.x);
        // alpha *= epower;
        pathData.radiance += mi.evalAD(diffData, sd, float3(0.0), sg);

        // pathData.radiance += float3(0.1f) * baseColor;
        // pathData.thp *= (1 - alpha);
    }
    else
    {
        // pathData.radiance += evalDirectAnalytic(sceneQuery, sd, mi, diffData, ray.origin, sg);
    }

    // float3 hitPos = isect.posW;

    // ray.origin = computeRayOrigin(hitPos, ray.direction);
    // ray.direction = ray.direction;

    // pathData.radiance += float3(1 / 10.0f, 1 / 10.0f, 1 / 10.0f);
    // pathData.radiance += gScene.customPrimitiveInstanceOffset / 10.0;
    // pathData.radiance += float3(0.0f, 1.0f, 0.0f);;

    // if (gScene.getGeometryInstanceType(isect.instanceID) == GeometryType::Custom)
    // {
    //     ray.origin = computeRayOrigin(hitPos, ray.direction);
    // }

    // else
    // {
    //     if (!generateScatterRay(sceneQuery, sd, mi, diffData, isect, ray, pathData, sg))
    //     {
    //         pathData.terminated = true;
    //         return;
    //     }
    // }

    // print<float4>("ray.direction", float4(ray.direction, 1.0f));
    print<float>("pathData.thp.x", pathData.thp.x);

    pathData.length++;
    print<uint>("pathData.length", pathData.length);
    print<float>("hitT", isect.hitT);
}
