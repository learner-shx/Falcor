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
#include "TestGauss.h"
#include <random>

const char kShaderFilename[] = "RenderPasses/TestGauss/TestGauss.rt.slang";

// Ray tracing settings that affect the traversal stack size.
// These should be set as small as possible.
const uint32_t kMaxPayloadSizeBytes = 72u;
const uint32_t kMaxAttributeSizeBytes = 8u;
const uint32_t kMaxRecursionDepth = 2u;

const char kInputViewDir[] = "ViewW";

const ChannelList kInputChannels = {
    // clang-format off
    { "vbuffer",        "gVBuffer",     "Visibility buffer in packed format" },
    { kInputViewDir,    "gViewW",       "World-space view direction (xyz float format)", true /* optional */ },
    // clang-format on
};

const ChannelList kOutputChannels = {
    // clang-format off
    { "color",          "gOutputColor", "Output color (sum of direct and indirect)", false, ResourceFormat::RGBA32Float },
    // clang-format on
};

const char kMaxBounces[] = "maxBounces";
const char kComputeDirect[] = "computeDirect";
const char kUseImportanceSampling[] = "useImportanceSampling";

std::mt19937 rng;

extern "C" FALCOR_API_EXPORT void registerPlugin(Falcor::PluginRegistry& registry)
{
    registry.registerClass<RenderPass, TestGauss>();
}

void TestGauss::registerScriptBindings(pybind11::module& m)
{
    pybind11::class_<TestGauss, RenderPass, ref<TestGauss>> pass(m, "TestGauss");
    pass.def("addRandomGauss", &TestGauss::addRandomGauss);
}


TestGauss::TestGauss(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice)
{
    parseProperties(props);

    // Create sample generator.
    mpSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
    FALCOR_ASSERT(mpSampleGenerator);
}

void TestGauss::parseProperties(const Properties& props)
{
    for (const auto& [key, value] : props)
    {
        if (key == kMaxBounces)
            mMaxBounces = value;
        else if (key == kComputeDirect)
            mComputeDirect = value;
        else if (key == kUseImportanceSampling)
            mUseImportanceSampling = value;
        else
            logWarning("Unknown field `" + key + "` in TestGauss pass");
    }
}
Properties TestGauss::getProperties() const
{
    Properties props;
    props[kMaxBounces] = mMaxBounces;
    props[kComputeDirect] = mComputeDirect;
    props[kUseImportanceSampling] = mUseImportanceSampling;
    return props;
}

RenderPassReflection TestGauss::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;

    // reflector.addOutput("dst");
    // reflector.addInput("src");
    // reflector.addOutput(kOutput, "Output image").bindFlags(ResourceBindFlags::UnorderedAccess).format(ResourceFormat::RGBA32Float);
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);

    return reflector;
}

void TestGauss::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    // Set new scene.
    mpScene = pScene;

    // Clear data for previous scene.
    // After changing scene, the ray tracing program needs to be recreated.
    mRT.pProgram = nullptr;
    mRT.pBindingTable = nullptr;
    mRT.pVars = nullptr;
    mFrameCount = 0;

    if (mpScene)
    {
        sceneChanged();
    }
}

void TestGauss::sceneChanged()
{
    FALCOR_ASSERT(mpScene);
    const uint32_t geometryCount = mpScene->getGeometryCount();

    //
    // Example creating a ray tracing program using the new interfaces.
    //

    ProgramDesc desc;
    desc.addShaderModules(mpScene->getShaderModules());
    desc.addShaderLibrary(kShaderFilename);
    desc.addMaxPayloadSize(kMaxPayloadSizeBytes);
    desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
    desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);
    desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
    // desc.setMaxAttributeSize(kMaxAttributeSizeBytes);

    mRT.pBindingTable = RtBindingTable::create(2, 2, geometryCount);
    auto& sbt = mRT.pBindingTable;

    sbt->setRayGen(desc.addRayGen("rayGen"));
    sbt->setMiss(0, desc.addMiss("scatterMiss"));
    sbt->setMiss(1, desc.addMiss("shadowMiss"));

    if (mpScene->hasGeometryType(Scene::GeometryType::TriangleMesh))
    {
        sbt->setHitGroup(
            0,
            mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh);
            desc.addHitGroup("scatterTriangleMeshClosestHit", "scatterTriangleMeshAnyHit")
        );
        sbt->setHitGroup(
            1,
            mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh);
            desc.addHitGroup("", "shadowTriangleMeshAnyHit");
        )
    }

    if (mpScene->hasGeometryType(Scene::GeometryType::DisplacedTriangleMesh))
    {
        sbt->setHitGroup(
            0,
            mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh),
            desc.addHitGroup("scatterDisplacedTriangleMeshClosestHit", "", "displacedTriangleMeshIntersection")
        );
        sbt->setHitGroup(
            1,
            mpScene->getGeometryIDs(Scene::GeometryType::DisplacedTriangleMesh),
            desc.addHitGroup("", "", "displacedTriangleMeshIntersection")
        );
    }

    if (mpScene->hasGeometryType(Scene::GeometryType::Curve))
    {
        sbt->setHitGroup(
            0, mpScene->getGeometryIDs(Scene::GeometryType::Curve), desc.addHitGroup("scatterCurveClosestHit", "", "curveIntersection")
        );
        sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::Curve), desc.addHitGroup("", "", "curveIntersection"));
    }

    if (mpScene->hasGeometryType(Scene::GeometryType::SDFGrid))
    {
        sbt->setHitGroup(
            0,
            mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid),
            desc.addHitGroup("scatterSdfGridClosestHit", "", "sdfGridIntersection")
        );
        sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::SDFGrid), desc.addHitGroup("", "", "sdfGridIntersection"));
    }


    // In this mode we test having two different ray types traced against
    // both triangles and custom primitives using intersection shaders.

    auto spherePurple = desc.addHitGroup("closestHitSpherePurple", "", "intersectSphere");
    auto sphereYellow = desc.addHitGroup("closestHitSphereYellow", "", "intersectSphere");
    auto sphereAttrib = desc.addHitGroup("closestHitSphereAttrib", "", "intersectSphere");

    if (mpScene->hasGeometryTypes(Scene::GeometryType::Custom))
    {
        sbt->setHitGroup(
            0,
            mpScene->getGeometryIDs(Scene::GeometryType::Custom),
            desc.addHitGroup("closestHitSphereAttrib", "", "intersectSphere")
        );
    }
    // Override specific hit groups for some geometries.
    for (uint geometryID = 0; geometryID < geometryCount; geometryID++)
    {
        auto type = mpScene->getGeometryType(GlobalGeometryID{geometryID});

        if (type == Scene::GeometryType::TriangleMesh)
        {
            if (geometryID == 1)
            {
                sbt->setHitGroup(0, geometryID, greenMtl);
                sbt->setHitGroup(1, geometryID, greenMtl);
            }
            else if (geometryID == 3)
            {
                sbt->setHitGroup(0, geometryID, redMtl);
                sbt->setHitGroup(1, geometryID, redMtl);
            }
        }
        else if (type == Scene::GeometryType::Custom)
        {
            uint32_t index = mpScene->getCustomPrimitiveIndex(GlobalGeometryID{geometryID});
            uint32_t userID = mpScene->getCustomPrimitive(index).userID;

            // Use non-default material for custom primitives with even userID.
            // if (userID % 2 == 0)
            // {
            //     sbt->setHitGroup(0, geometryID, spherePurple);
            //     sbt->setHitGroup(1, geometryID, sphereYellow);
            // }
            sbt->setHitGroup(0, geometryID, sphereAttrib);
            sbt->setHitGroup(1, geometryID, sphereAttrib);
        }
    }

    // Add global type conformances.
    desc.addTypeConformances(mpScene->getTypeConformances());



    // Create raygen and miss shaders.
    sbt->setRayGen(desc.addRayGen("rayGen"));
    sbt->setMiss(0, desc.addMiss("miss0"));
    sbt->setMiss(1, desc.addMiss("miss1"));

    DefineList defines = mpScene->getSceneDefines();
    // defines.add("MODE", std::to_string(mMode));

    // Create program and vars.
    mRT.pProgram = Program::create(mpDevice, desc, defines);
    mRT.pVars = RtProgramVars::create(mpDevice, mRT.pProgram, sbt);
}
void TestGauss::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // renderData holds the requested resources
    // auto& pTexture = renderData.getTexture("src");
    const uint2 frameDim = renderData.getDefaultTextureDims();

    auto pOutput = renderData.getTexture(kOutput);
    pRenderContext->clearUAV(pOutput->getUAV().get(), float4(0, 0, 0, 1));

    if (!mpScene)
        return;

    // Check for scene changes that require shader recompilation.
    if (is_set(mpScene->getUpdates(), Scene::UpdateFlags::RecompileNeeded) ||
        is_set(mpScene->getUpdates(), Scene::UpdateFlags::GeometryChanged))
    {
        sceneChanged();
    }

    auto var = mRT.pVars->getRootVar()["gTestProgram"];
    var["frameDim"] = frameDim;
    var["output"] = pOutput;

    mpScene->raytrace(pRenderContext, mRT.pProgram.get(), mRT.pVars, uint3(frameDim, 1));

}

void TestGauss::renderUI(Gui::Widgets& widget)
{
    if (!mpScene)
    {
        widget.text("No scene loaded!");
        return;
    }


    auto primCount = mpScene->getCustomPrimitiveCount();
    widget.text("Custom primitives: " + std::to_string(primCount));

    mSelectedIdx = std::min(mSelectedIdx, primCount - 1);
    widget.text("\nSelected primitive:");
    widget.var("##idx", mSelectedIdx, 0u, primCount - 1);

    if (mSelectedIdx != mPrevSelectedIdx)
    {
        mPrevSelectedIdx = mSelectedIdx;
        mSelectedAABB = mpScene->getCustomPrimitiveAABB(mSelectedIdx);
    }

    if (widget.button("Add"))
    {
        addRandomGauss();
    }

    // if (primCount > 0)
    // {
    //     if (widget.button("Remove", true))
    //     {
    //         removeCustomPrimitive(mSelectedIdx);
    //     }

    //     if (widget.button("Random move"))
    //     {
    //         moveCustomPrimitive();
    //     }

    //     bool modified = false;
    //     modified |= widget.var("Min", mSelectedAABB.minPoint);
    //     modified |= widget.var("Max", mSelectedAABB.maxPoint);
    //     if (widget.button("Update"))
    //     {
    //         mpScene->updateCustomPrimitive(mSelectedIdx, mSelectedAABB);
    //     }
    // }

}

void TestGauss::addRandomGauss()
{
    if (!mpScene)
    {
        logWarning("No scene! Ignoring call to addCustomPrimitive()");
        return;
    }

    std::uniform_real_distribution<float> u(0.f, 1.f);

    float3 c = {4.f * u(rng) - 2.f, u(rng), 4.f * u(rng) - 2.f};
    float r = 0.5f * u(rng) + 0.5f;

    mpScene->addCustomPrimitive(mUserID++, AABB(c - r, c + r));

}
