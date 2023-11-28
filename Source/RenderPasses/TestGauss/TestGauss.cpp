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
#include "RenderGraph/RenderPassHelpers.h"
#include "RenderGraph/RenderPassStandardFlags.h"

#include <random>


const char kShaderFilename[] = "RenderPasses/TestGauss/TestGauss.rt.slang";

// Ray tracing settings that affect the traversal stack size.
// These should be set as small as possible.
const uint32_t kMaxPayloadSizeBytes = 72u;
const uint32_t kMaxAttributeSizeBytes = 8u;
const uint32_t kMaxRecursionDepth = 2u;

const char kInputViewDir[] = "viewW";

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
    mpSampleGenerator = SampleGenerator::create(mpDevice, SAMPLE_GENERATOR_UNIFORM);
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

void TestGauss::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // renderData holds the requested resources
    // auto& pTexture = renderData.getTexture("src");
    auto& dict = renderData.getDictionary();

    if (mOptionsChanged)
    {
        auto flags = dict.getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        dict[Falcor::kRenderPassRefreshFlags] = flags | Falcor::RenderPassRefreshFlags::RenderOptionsChanged;
        mOptionsChanged = false;
    }
    // If we have no scene, just clear the outputs and return.
    if (!mpScene)
    {
        for(auto it : kOutputChannels)
        {
            Texture* pDst = renderData.getTexture(it.name).get();
            if (pDst)
                pRenderContext->clearTexture(pDst);
        }
        return;
    }

    // Check for scene changes that require shader recompilation.
    if (is_set(mpScene->getUpdates(), Scene::UpdateFlags::RecompileNeeded) ||
        is_set(mpScene->getUpdates(), Scene::UpdateFlags::GeometryChanged))
    {
        sceneChanged();
    }

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    // Configure depth-of-field.
    const bool useDOF = mpScene->getCamera()->getApertureRadius() > 0.f;
    if (useDOF && renderData[kInputViewDir] == nullptr)
    {
        logWarning("Depth-of-field requires the '{}' input. Expect incorrect shading.", kInputViewDir);
    }


    // Specialize program
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    mRT.pProgram->addDefine("MAX_BOUNCES", std::to_string(mMaxBounces));
    mRT.pProgram->addDefine("COMPUTE_DIRECT", mComputeDirect ? "1" : "0");
    mRT.pProgram->addDefine("USE_IMPORTANCE_SAMPLING", mUseImportanceSampling ? "1" : "0");
    mRT.pProgram->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    mRT.pProgram->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    mRT.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mRT.pProgram->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // TODO: This should be moved to a more general mechanism using Slang.
    mRT.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    mRT.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    // Prepare program vars. This may trigger shader compilation.
    // The program should have all necvessary defines set at this point.
    // if (mRT.pVars == nullptr)
    // {
    //     prepareVars();
    // }
    prepareVars();
    FALCOR_ASSERT(mRT.pVars);
    // Set constants.
    auto var = mRT.pVars->getRootVar();
    var["CB"]["gFrameCount"] = mFrameCount;
    var["CB"]["gPRNGDimension"] = dict.keyExists(kRenderPassPRNGDimension) ? dict[kRenderPassPRNGDimension] : 0u;

    // Bind I/O buffers. These needs to be done per-frame as the buffers may change anytime.
    auto bind = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            var[desc.texname] = renderData.getTexture(desc.name);
        }
    };

    for (auto channel : kInputChannels)
        bind(channel);
    for (auto channel : kOutputChannels)
        bind(channel);

    const uint2 targetDim = renderData.getDefaultTextureDims();
    FALCOR_ASSERT(targetDim.x > 0 && targetDim.y > 0);

    mpScene->raytrace(pRenderContext, mRT.pProgram.get(), mRT.pVars, uint3(targetDim, 1));

    mFrameCount++;
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
    desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
    desc.setMaxAttributeSize(mpScene->getRaytracingMaxAttributeSize());
    desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);
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
            mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh),
            desc.addHitGroup("scatterTriangleMeshClosestHit", "scatterTriangleMeshAnyHit")
        );
        sbt->setHitGroup(
            1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), desc.addHitGroup("", "shadowTriangleMeshAnyHit")
        );
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

    if (mpScene->hasGeometryType(Scene::GeometryType::Custom))
    {
        sbt->setHitGroup(
            0,
            mpScene->getGeometryIDs(Scene::GeometryType::Custom),
            desc.addHitGroup("closestHitSphereAttrib", "", "intersectSphere")
        );
    }

    DefineList defines = mpScene->getSceneDefines();
    // defines.add("MODE", std::to_string(mMode));

    // Create program
    mRT.pProgram = Program::create(mpDevice, desc, defines);
    // prepareVars();

}


void TestGauss::prepareVars()
{
    FALCOR_ASSERT(mpScene);
    FALCOR_ASSERT(mRT.pProgram);

    mRT.pProgram->addDefines(mpSampleGenerator->getDefines());
    mRT.pProgram->setTypeConformances(mpScene->getTypeConformances());

    mRT.pVars = RtProgramVars::create(mpDevice, mRT.pProgram, mRT.pBindingTable);

    auto var = mRT.pVars->getRootVar();
    mpSampleGenerator->bindShaderData(var);
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
        mpScene->updateCustomPrimitive(mSelectedIdx, mSelectedAABB);
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
    sceneChanged();

}

