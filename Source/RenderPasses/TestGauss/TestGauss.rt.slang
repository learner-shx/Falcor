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

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Utils.Geometry.IntersectionHelpers;
import Rendering.Lights.LightHelpers;

import DiffRendering.SharedTypes;
import DiffRendering.DiffSceneIO;
import DiffRendering.DiffSceneQuery;
import DiffRendering.DiffDebug;

import Scene.Material.GaussMaterialData;
cbuffer CB
{
    uint gFrameCount;    // Frame count since scene was loaded.
    uint gPRNGDimension; // First available PRNG dimension.
}

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW; // Optional

// Outputs
RWTexture2D<float4> gOutputColor;

// Static configuration based on defines set from the host.
#define is_valid(name) (is_valid_##name != 0)
static const uint kMaxBounces = MAX_BOUNCES;
static const bool kComputeDirect = COMPUTE_DIRECT;
static const bool kUseImportanceSampling = USE_IMPORTANCE_SAMPLING;
static const bool kUseAnalyticLights = USE_ANALYTIC_LIGHTS;
static const bool kUseEmissiveLights = USE_EMISSIVE_LIGHTS;
static const bool kUseEnvLight = USE_ENV_LIGHT;
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;
static const float3 kDefaultBackgroundColor = float3(0, 0, 0);
static const float kRayTMax = FLT_MAX;

/**
 * Payload for shadow ray.
 */
struct ShadowRayData
{
    bool visible;
};

/**
 * Payload for scatter ray (up to 72B).
 */
struct ScatterRayData
{
    float3 radiance;  ///< Accumulated outgoing radiance from path.
    bool terminated;  ///< Set to true when path is terminated.
    float3 thp;       ///< Current path throughput. This is updated at each path vertex.
    uint pathLength;  ///< Path length in number of path segments (0 at origin, 1 at first secondary hit, etc.). Max 2^31.
    float3 origin;    ///< Next path segment origin.
    float3 direction; ///< Next path segment direction.

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).

    /**
     * Initializes ray payload with default parameters.
     */
    __init(SampleGenerator sg)
    {
        this.terminated = false;
        this.pathLength = 0;
        this.radiance = float3(0, 0, 0);
        this.thp = float3(1, 1, 1);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
        this.sg = sg;
    }
};

/**
 * Setup ShadingData based on loaded vertex/material attributes for a hit point.
 * @param[in] hit Hit information.
 * @param[in] rayOrigin Ray origin.
 * @param[in] rayDir Normalized ray direction.
 * @param[in] lod Method for computing texture level-of-detail.
 * @return ShadingData struct.
 */
ShadingData loadShadingData(const HitInfo hit, const float3 rayOrigin, const float3 rayDir, const ITextureSampler lod)
{
    VertexData v = {};
    uint materialID = {};

#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_TRIANGLE_MESH)
    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        v = gScene.getVertexData(triangleHit);
        materialID = gScene.getMaterialID(triangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_DISPLACED_TRIANGLE_MESH)
    if (hit.getType() == HitType::DisplacedTriangle)
    {
        const DisplacedTriangleHit displacedTriangleHit = hit.getDisplacedTriangleHit();
        v = gScene.getVertexData(displacedTriangleHit, -rayDir);
        materialID = gScene.getMaterialID(displacedTriangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_CURVE)
    if (hit.getType() == HitType::Curve)
    {
        const CurveHit curveHit = hit.getCurveHit();
        v = gScene.getVertexDataFromCurve(curveHit);
        materialID = gScene.getMaterialID(curveHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_SDF_GRID)
    if (hit.getType() == HitType::SDFGrid)
    {
        const SDFGridHit sdfGridHit = hit.getSDFGridHit();
        v = gScene.getVertexDataFromSDFGrid(sdfGridHit, rayOrigin, rayDir);
        materialID = gScene.getMaterialID(sdfGridHit.instanceID);
    }
#endif

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir, lod);

    return sd;
}

/**
 * Returns the primary ray's direction.
 */
float3 getPrimaryRayDir(uint2 launchIndex, uint2 launchDim, const Camera camera)
{
    if (is_valid(gViewW))
    {
        // If we have the view vector bound as a buffer, just fetch it. No need to compute anything.
        return -gViewW[launchIndex].xyz;
    }
    else
    {
        // Compute the view vector. This must exactly match what the G-buffer pass is doing (jitter etc.).
        // Note that we do not take depth-of-field into account as it would require exactly matching the
        // sample generator between the passes, which is error prone. The host side will issue a warning instead.
        return camera.computeRayPinhole(launchIndex, launchDim).dir;
    }
}

/**
 * Traces a shadow ray towards a light source.
 * @param[in] origin Ray origin for the shadow ray.
 * @param[in] dir Direction from shading point towards the light source (normalized).
 * @param[in] distance Distance to the light source.
 * @return True if light is visible, false otherwise.
 */
bool traceShadowRay(float3 origin, float3 dir, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = distance;

    ShadowRayData rayData;
    rayData.visible = false; // Set to true by miss shader if ray is not terminated before
    TraceRay(
        gScene.rtAccel,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xff /* instanceInclusionMask */,
        1 /* hitIdx */,
        rayTypeCount,
        1 /* missIdx */,
        ray,
        rayData
    );

    return rayData.visible;
}

/**
 * Traces a scatter ray based on ray parameters stored in the ray payload.
 * @param[in] rayData Describes the ray parameters. The struct is modified based on the result.
 */
void traceScatterRay(inout ScatterRayData rayData)
{
    RayDesc ray;
    ray.Origin = rayData.origin;
    ray.Direction = rayData.direction;
    ray.TMin = 0.f;
    ray.TMax = kRayTMax;

    uint rayFlags = 0; // TODO: Set cull mode from the app
    TraceRay(gScene.rtAccel, rayFlags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);
}

/**
 * Evaluates the direct illumination from analytic lights.
 * This function samples Falcor's light list uniformly with one shadow ray.
 * @param[in] sd Shading data.
 * @param[in] mi Material instance.
 * @param[in,out] sg SampleGenerator object.
 * @return Outgoing radiance in view direction.
 */
float3 evalDirectAnalytic(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg)
{
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0)
        return float3(0.f);

    // Pick one of the analytic light sources randomly with equal probability.
    const uint lightIndex = min(uint(sampleNext1D(sg) * lightCount), lightCount - 1);
    float invPdf = lightCount; // Light selection pdf = 1.0 / lightCount.

    // Sample local light source.
    AnalyticLightSample ls;

    if (!sampleLight(sd.posW, gScene.getLight(lightIndex), sg, ls))
        return float3(0.f);

    // Reject sample if not in the hemisphere of a BSDF lobe.
    const uint lobeTypes = mi.getLobeTypes(sd);
    const bool hasReflection = lobeTypes & uint(LobeType::Reflection);
    const bool hasTransmission = lobeTypes & uint(LobeType::Transmission);
    float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
    if ((NdotL <= kMinCosTheta && !hasTransmission) || (NdotL >= -kMinCosTheta && !hasReflection))
        return float3(0.f);

    // Get origin with offset applied in direction of the geometry normal to avoid self-intersection.
    const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);

    // Test visibility by tracing a shadow ray.
    bool V = traceShadowRay(origin, ls.dir, ls.distance);
    if (!V)
        return float3(0.f);

    // Evaluate contribution.
    // return mi.eval(sd, ls.dir, sg);
    return mi.eval(sd, ls.dir, sg) * ls.Li * invPdf;
}

/**
 * Generate a new scatter ray or terminate.
 * @param[in] sd Shading data.
 * @param[in] mi Material instance.
 * @param[in] isCurveHit True if on curve hit.
 * @param[in] rayOrigin Ray origin for the new ray.
 * @param[in,out] rayData Ray payload.
 * @return True if the path continues.
 */
bool generateScatterRay(const ShadingData sd, const IMaterialInstance mi, bool isCurveHit, float3 rayOrigin, inout ScatterRayData rayData)
{
    // Sample material.
    BSDFSample bsdfSample;
    if (mi.sample(sd, rayData.sg, bsdfSample, kUseImportanceSampling))
    {
        rayData.origin = rayOrigin;
        if (!isCurveHit && bsdfSample.isLobe(LobeType::Transmission))
        {
            rayData.origin = sd.computeRayOrigin(false);
        }
        rayData.direction = bsdfSample.wo;
        rayData.thp *= bsdfSample.weight;
        return any(rayData.thp > 0.f);
    }

    return false;
}

/**
 * Process a hit.
 * Loads the shading data, samples analytic lights and samples a new scatter ray.
 * Terminates the path if maximum number of bounces is reached.
 * @param[in] hit Hit info.
 * @param[in,out] rayData Ray payload.
 *
 */
void handleHit(const HitInfo hit, inout ScatterRayData rayData)
{

    const bool isCurveHit = hit.getType() == HitType::Curve;
    let lod = ExplicitLodTextureSampler(0.f);

    // Load shading data.
    ShadingData sd = loadShadingData(hit, rayData.origin, rayData.direction, lod);

    // Create material instance.
    let mi = gScene.materials.getMaterialInstance(sd, lod);

    // Add emitted light.
    if (kUseEmissiveLights && (kComputeDirect || rayData.pathLength > 0))
    {

        rayData.radiance += rayData.thp * mi.getProperties(sd).emission;
    }

    // Check whether to terminate based on max depth.
    if (rayData.pathLength >= kMaxBounces)
    {
        rayData.terminated = true;
        return;
    }

    // Compute ray origin for new rays spawned from the hit.
    float3 rayOrigin;
    if (isCurveHit)
    {
        // For curves, we set the new origin at the sphere center.
        rayOrigin = sd.posW - sd.curveRadius * sd.frame.N;
    }
    else
    {
        rayOrigin = sd.computeRayOrigin();
    }

    // Add contribution of direct light from analytic lights.
    if (kUseAnalyticLights)
    // if (true)
    {
        // rayData.radiance += mi.getProperties(sd).emission;
        float3 Lr = evalDirectAnalytic(sd, mi, rayData.sg);
        rayData.radiance += rayData.thp * Lr;

    }

    // Generate scatter ray for the next path segment.
    // The raygen shader will continue the path based on the returned payload.
    if (!generateScatterRay(sd, mi, isCurveHit, rayOrigin, rayData))
    {
        rayData.terminated = true;
        return;
    }



    rayData.pathLength++;
}

/**
 * This is the main entry point for the minimal path tracer.
 *
 * One path per pixel is generated, which is traced into the scene.
 * The path tracer is written as a for-loop over path segments.
 *
 * Built-in light sources (point, directional) are sampled explicitly at each
 * path vertex. The contributions from area lights (env map and mesh lights)
 * are explicitly added by the scatter ray hit/miss shaders.
 *
 * @param[in] pixel Pixel to trace a path for.
 * @param[in] frameDim Dimension of the frame in pixels.
 * @return Returns the estimated color (radiance).
 */
float3 tracePath(const uint2 pixel, const uint2 frameDim)
{
    float3 outColor = float3(0.f);

    const float3 primaryRayOrigin = gScene.camera.getPosition();
    const float3 primaryRayDir = getPrimaryRayDir(pixel, frameDim, gScene.camera);

    const HitInfo hit = HitInfo(gVBuffer[pixel]);
    // Create sample generator.
    SampleGenerator sg = SampleGenerator(pixel, gFrameCount);
    ScatterRayData rayData = ScatterRayData(sg);

    // if (hit.isValid())
    // {

    // Pixel represents a valid primary hit. Compute its contribution.
    uint rayFlags = RAY_FLAG_NONE;
    RayDesc ray = gScene.camera.computeRayThinlens(pixel, frameDim, sampleNext2D(sg)).toRayDesc();
    // RayDesc ray;
    // ray.Origin = primaryRayOrigin;
    // ray.Direction = primaryRayDir;
    // ray.TMin = 0.f;
    // ray.TMax = kRayTMax;


    // const bool isCurveHit = hit.getType() == HitType::Curve;
    // let lod = ExplicitLodTextureSampler(0.f);

    // // Load shading data.
    // ShadingData sd = loadShadingData(hit, primaryRayOrigin, primaryRayDir, lod);

    // // Create material instance at shading point.
    // let mi = gScene.materials.getMaterialInstance(sd, lod);



    // // Advance the generator to the first available dimension.
    // // TODO: This is potentially expensive. We may want to store/restore the state from memory if it becomes a problem.
    for (uint i = 0; i < gPRNGDimension; i++)
        sampleNext1D(sg);

    // // Compute ray origin for new rays spawned from the G-buffer.
    // float3 rayOrigin;
    // if (isCurveHit)
    // {
    //     // For curves, we set the new origin at the sphere center.
    //     rayOrigin = sd.posW - sd.curveRadius * sd.frame.N;
    // }
    // else
    // {
    //     rayOrigin = sd.computeRayOrigin();
    // }
    TraceRay(gScene.rtAccel, rayFlags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);
    // if (kComputeDirect)
    // {
    //     // Always output directly emitted light, independent of whether emissive materials are treated as light sources or not.
    //     outColor += mi.getProperties(sd).emission;

    //     // Add contribution of direct light from analytic lights.
    //     // Light probe and mesh lights are handled by the scatter ray hit/miss shaders.
    //     outColor += kUseAnalyticLights ? evalDirectAnalytic(sd, mi, sg) : float3(0.f);
    // }
    outColor += rayData.radiance;
    // Prepare ray payload.
    // rayData = ScatterRayData(sg);

    // // Generate scatter ray.
    // if (!generateScatterRay(sd, mi, isCurveHit, rayOrigin, rayData))
    //     rayData.terminated = true;

    // // Follow path into the scene and compute its total contribution.
    // for (uint depth = 0; depth <= kMaxBounces && !rayData.terminated; depth++)
    // {
    //     // Trace scatter ray. If it hits geometry, the closest hit shader samples
    //     // direct illumination and generates the next scatter ray.
    //     traceScatterRay(rayData);
    // }

    // RayDesc ray = gScene.camera.computeRayPinhole(pixel, frameDim).toRayDesc();
    // TraceRay(gScene.rtAccel, RAY_FLAG_NONE, 0xff, 0, rayTypeCount, 0, ray, rayData);
    // outColor += rayData.radiance;
    // Store contribution from scatter ray.
    // outColor += rayData.radiance;
    // }
    // else
    // {
    //     // Background pixel.
    //     outColor = kUseEnvBackground ? gScene.envMap.eval(primaryRayDir) : kDefaultBackgroundColor;
    // }

    return outColor;
}

//
// Shader entry points for miss shaders.
//

[shader("miss")]
void scatterMiss(inout ScatterRayData rayData)
{
    // Ray missed the scene. Mark the ray as terminated.
    rayData.terminated = true;

    // Add contribution from distant light (env map) in this direction.
    if (kUseEnvLight && (kComputeDirect || rayData.pathLength > 0))
    {
        float3 Le = gScene.envMap.eval(WorldRayDirection());
        rayData.radiance += rayData.thp * Le;
    }
}

[shader("miss")]
void shadowMiss(inout ShadowRayData rayData)
{
    // The miss shader is executed if the ray misses all geometry. Mark as visible.
    rayData.visible = true;
}

//
// Shader entry points for TriangleMesh hit groups.
//

[shader("anyhit")]
void scatterTriangleMeshAnyHit(inout ScatterRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
        IgnoreHit();
    // rayData.radiance += float3(0.0f, 0.0f, 0.1f) * rayData.thp;
}

[shader("closesthit")]
void scatterTriangleMeshClosestHit(inout ScatterRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    handleHit(HitInfo(triangleHit), rayData);
    rayData.radiance += float3(0.0f, 0.0f, 0.1f) * rayData.thp;
}

[shader("anyhit")]
void shadowTriangleMeshAnyHit(inout ShadowRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
        IgnoreHit();
}

//
// Shader entry points for DisplacedTriangleMesh hit groups.
//

[shader("intersection")]
void displacedTriangleMeshIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    DisplacedTriangleMeshIntersector::Attribs attribs;
    float t;
    if (DisplacedTriangleMeshIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attribs, t))
    {
        ReportHit(t, 0, attribs);
    }
}

[shader("closesthit")]
void scatterDisplacedTriangleMeshClosestHit(inout ScatterRayData rayData, DisplacedTriangleMeshIntersector::Attribs attribs)
{
    DisplacedTriangleHit displacedTriangleHit;
    displacedTriangleHit.instanceID = getGeometryInstanceID();
    displacedTriangleHit.primitiveIndex = PrimitiveIndex();
    displacedTriangleHit.barycentrics = attribs.barycentrics;
    displacedTriangleHit.displacement = attribs.displacement;
    handleHit(HitInfo(displacedTriangleHit), rayData);
}

//
// Shader entry points for Curve hit groups.
//

[shader("intersection")]
void curveIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    CurveIntersector::Attribs attribs;
    float t;
    if (CurveIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attribs, t))
    {
        ReportHit(t, 0, attribs);
    }
}

[shader("closesthit")]
void scatterCurveClosestHit(inout ScatterRayData rayData, CurveIntersector::Attribs attribs)
{
    CurveHit curveHit;
    curveHit.instanceID = getGeometryInstanceID();
    curveHit.primitiveIndex = PrimitiveIndex();
    curveHit.barycentrics = attribs.barycentrics;
    handleHit(HitInfo(curveHit), rayData);
}

//
// Shader entry points for SDFGrid hit groups.
//

[shader("intersection")]
void sdfGridIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    SDFGridHitData sdfGridHitData;
    float t;
    if (SDFGridIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), sdfGridHitData, t))
    {
        ReportHit(t, 0, sdfGridHitData);
    }
}

[shader("closesthit")]
void scatterSdfGridClosestHit(inout ScatterRayData rayData, SDFGridHitData sdfGridHitData)
{
    SDFGridHit sdfGridHit;
    sdfGridHit.instanceID = getGeometryInstanceID();
    sdfGridHit.hitData = sdfGridHitData;
    handleHit(HitInfo(sdfGridHit), rayData);

}

//
// Shader entry point for ray generation shader.
//

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    float3 color = tracePath(pixel, frameDim);

    gOutputColor[pixel] = float4(color, 1.f);
}


struct MyIntersectionAttribs
{
    float t;
    float d;
    // float3 mean;
};

float3 transformPoint4x3(const float3 p, const float4x4 matrix)
{
    float3 transformed = {
        matrix[0][0] * p.x + matrix[0][1] * p.y + matrix[0][2] * p.z + matrix[0][3],
        matrix[1][0] * p.x + matrix[1][1] * p.y + matrix[1][2] * p.z + matrix[1][3],
        matrix[2][0] * p.x + matrix[2][1] * p.y + matrix[2][2] * p.z + matrix[2][3]
    };
    return transformed;
}

float3 computeCov2D(const float3 mean, float focal_x, float focal_y, float tan_fovx, float tan_fovy, const float3 diag, float3 Offdiag, float4x4 viewmatrix)
{
    // The following models the steps outlined by equations 29
    // and 31 in "EWA Splatting" (Zwicker et al., 2002).
    // Additionally considers aspect / scaling of viewport.
    // Transposes used to account for row-/column-major conventions.
    // viewmatrix = transpose(viewmatrix);
    float3 t = mul(viewmatrix, float4(mean, 1.0f)).xyz;

    const float limx = 1.3f * tan_fovx;
    const float limy = 1.3f * tan_fovy;
    const float txtz = t.x / t.z;
    const float tytz = t.y / t.z;
    t.x = min(limx, max(-limx, txtz)) * t.z;
    t.y = min(limy, max(-limy, tytz)) * t.z;

    float3x3 J = float3x3(
		focal_x / t.z, 0.0f, -(focal_x * t.x) / (t.z * t.z),
		0.0f, focal_y / t.z, -(focal_y * t.y) / (t.z * t.z),
		0, 0, 0);

    float3x3 W = float3x3(
        viewmatrix[0][0], viewmatrix[0][1], viewmatrix[0][2],
        viewmatrix[1][0], viewmatrix[1][1], viewmatrix[1][2],
        viewmatrix[2][0], viewmatrix[2][1], viewmatrix[2][2] );

    float3x3 T = W * J;

    float3x3 Vrk = float3x3(
		diag[0], Offdiag[0], Offdiag[1],
		Offdiag[0], diag[1], Offdiag[2],
		Offdiag[1], Offdiag[2], diag[2]);

    float3x3 cov = transpose(T) * transpose(Vrk) * T;

    // Apply low-pass filter: every Gaussian should be at least
    // one pixel wide/high. Discard 3rd row and column.
    cov[0][0] += 0.3f;
    cov[1][1] += 0.3f;
    return { float(cov[0][0]), float(cov[0][1]), float(cov[1][1]) };
}

bool intersectGauss(float3 origin, float3 dir, float3 mu, float3x3 cov, out float t, out float d)
{
    float3x3 covInv = inverse(cov);

    float3 muDelta = mu - origin;

    t = mul(mul(muDelta, covInv), dir) / mul(mul(dir, covInv), dir);

    float3 hitpos = origin + t * dir;

    float mdistance = mul(mul(hitpos - mu, covInv), hitpos - mu);

    d = mdistance;

    if (mdistance < 0.2f)
    {
        return true;
    }
    return false;
}

[shader("intersection")]
void intersectSphere()
{
    const GeometryInstanceID instanceID = getGeometryInstanceID();
    CustomPrimitiveDesc desc = gScene.getCustomPrimitiveDesc(instanceID);
    GaussMaterialData data = reinterpret<GaussMaterialData, MaterialPayload>(gScene.materials.getMaterialDataBlob(desc.materialID).payload);

    AABB aabb = gScene.getCustomPrimitiveAABB(instanceID);
    float3 diag = data.covDiag;
    float3 offDiag = data.covOffDiag;
    float3x3 covariance = float3x3(diag.x, offDiag.x, offDiag.y,
                                   offDiag.x, diag.y, offDiag.z,
                                   offDiag.y, offDiag.z, diag.z);
    float t;
    float d;
    bool isect = intersectGauss(WorldRayOrigin(), WorldRayDirection(), aabb.center(), covariance, t, d);


    // Inscribed sphere radius
    float3 e = aabb.extent();
    float radius = min(e.x, min(e.y, e.z)) * 0.5f;

    // isect = intersectRaySphere(WorldRayOrigin(), WorldRayDirection(), aabb.center(), radius, t);
    if (isect && t < RayTCurrent())
    {
        MyIntersectionAttribs attribs;
        attribs.t = t;
        attribs.d = d;
        ReportHit(t.x, 0, attribs);
    }
}

[shader("closesthit")]
void closestHitSphereAttrib(inout ScatterRayData rayData, MyIntersectionAttribs attribs)
{
    // const GeometryInstanceID instanceID = getGeometryInstanceID();
    // CustomPrimitiveDesc desc = gScene.getCustomPrimitiveDesc(instanceID);
    // GaussMaterialData data = reinterpret<GaussMaterialData, MaterialPayload>(gScene.materials.getMaterialDataBlob(desc.materialID).payload);

    // rayData.radiance += data.baseColor * (0.2 - attribs.d) * 2;
    // rayData.radiance += float3(0.1f, 0.0f, 0.0f);
    // rayData.radiance += float3(0.1f);
    // rayData.radiance += desc.color;
}

[shader("anyhit")]
void anyHitSphereAttrib(inout ScatterRayData rayData, MyIntersectionAttribs attribs)
{
    // // rayData.thp = RayTCurrent();
    const GeometryInstanceID instanceID = getGeometryInstanceID();
    CustomPrimitiveDesc desc = gScene.getCustomPrimitiveDesc(instanceID);
    GaussMaterialData data = reinterpret<GaussMaterialData, MaterialPayload>(gScene.materials.getMaterialDataBlob(desc.materialID).payload);
    uint2 frameDim = DispatchRaysDimensions().xy;


    float focalX = 1920 / (2.0f * gScene.camera.getTanFovX());
    float focalY = 1080 / (2.0f * gScene.camera.getTanFovY());
    AABB aabb = gScene.getCustomPrimitiveAABB(instanceID);

    // float3x4 viewMatrix = float3x4(
    //     gScene.camera.getView()[0],
    //     gScene.camera.getView()[1],
    //     gScene.camera.getView()[2]
    // );
    // float4x4 viewMatrix = gScene.camera.getView();
    // float3 rayDir = mul(viewMatrix, float4(WorldRayDirection(), 0.0f)).xyz;
    // float3 gaussDir = mul(viewMatrix, float4(normalize(aabb.center() - WorldRayOrigin()), 0.0f)).xyz;

    // float t2ZnearCam = (gScene.camera.getNearZ()) / rayDir.z;
    // float t2ZnearGauss = (gScene.camera.getNearZ()) / gaussDir.z;

    // float3 rayScreen = (mul(viewMatrix, float4(WorldRayOrigin(), 1.0f)).xyz + rayDir * t2ZnearCam);
    // float3 gaussScreen = (mul(viewMatrix, float4(WorldRayOrigin(), 1.0f)).xyz + gaussDir * t2ZnearGauss);

    // float4x4 viewProj = gScene.camera.getViewProj();
    // float4x4 Proj = gScene.camera.getProj();

    // float2 xyRayScreen = mul(Proj, float4(rayScreen, 1.0f)).xy;
    // float2 xyGaussScreen = mul(Proj, float4(gaussScreen, 1.0f)).xy;

    // float2 d = xyGaussScreen - xyRayScreen;

    // d *= frameDim;

    float3 gaussPos = aabb.center();
    float3 rayPos = WorldRayOrigin() + 10.0f * WorldRayDirection();

    float4 gaussPosNdc = mul(gScene.camera.getViewProj(), float4(gaussPos, 1.0f));
    float4 rayPosNdc = mul(gScene.camera.getViewProj(), float4(rayPos, 1.0f));

    float2 d = (gaussPosNdc.xy / gaussPosNdc.w) - (rayPosNdc.xy / rayPosNdc.w);

    d *= float2(1920, 1080);

    float4x4 viewMatrix = gScene.camera.getView();



    float3 cov = computeCov2D(  aabb.center(), focalX, focalY,
                                gScene.camera.getTanFovX(), gScene.camera.getTanFovY(),
                                data.covDiag, data.covOffDiag, viewMatrix );

    float det = (cov.x * cov.z - cov.y * cov.y);
    if (det == 0.0f)
    {
        IgnoreHit();
        return;
    }

    float det_inv = 1.f / det;
    float3 conic = { cov.z * det_inv, -cov.y * det_inv, cov.x * det_inv };

    // float4 dWorld = float4(attribs.d.x, attribs.d.y, attribs.d.z, 0.0);

    float4 con_o = float4(conic.x, conic.y, conic.z, data.alpha );
    float power = -0.5f * (con_o.x * d.x * d.x + con_o.z * d.y * d.y) - con_o.y * d.x * d.y;

    if (power > 0.0f)
    {
        IgnoreHit();
        return;
    }

    // Eq. (2) from 3D Gaussian splatting paper.
    // Obtain alpha by multiplying with Gaussian opacity
    // and its exponential falloff from mean.
    // Avoid numerical instabilities (see paper appendix).

    float alpha = min(0.99f, con_o.w * exp(power));
    alpha = data.alpha * exp(-10 * attribs.d);
    rayData.radiance += data.baseColor * rayData.thp * alpha;
    // rayData.radiance += float3(gaussPosNdc.xy, 0.0f);
    rayData.thp *= (1 - alpha);
    // rayData.radiance += float3(0.0f, 0.1f, 0.0f);
    // // rayData.radiance += float3(0.1f);

    if (any(rayData.thp < float3(0.00001f)))
        AcceptHitAndEndSearch();
    IgnoreHit();
}
