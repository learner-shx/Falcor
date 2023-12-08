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
#include "Scene/Material/GaussMaterial.h"

#include <random>


const char kShaderFilename[] = "RenderPasses/TestGauss/TestGaussPathTracer.rt.slang";

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

const char kComputeDirect[] = "computeDirect";
const char kUseImportanceSampling[] = "useImportanceSampling";

const std::string kSamplesPerPixel = "samplesPerPixel";
const std::string kMaxBounces = "maxBounces";

const std::string kDiffMode = "diffMode";
const std::string kDiffVarName = "diffVarName";

const std::string kSampleGenerator = "sampleGenerator";
const std::string kFixedSeed = "fixedSeed";
const std::string kUseBSDFSampling = "useBSDFSampling";
const std::string kUseNEE = "useNEE";
const std::string kUseMIS = "useMIS";

const std::string kUseWAR = "useWAR";
const std::string kAuxSampleCount = "auxSampleCount";
const std::string kLog10vMFConcentration = "Log10vMFConcentration";
const std::string kLog10vMFConcentrationScreen = "Log10vMFConcentrationScreen";
const std::string kBoundaryTermBeta = "boundaryTermBeta";
const std::string kUseAntitheticSampling = "useAntitheticSampling";

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
        if (key == kComputeDirect)
            mComputeDirect = value;
        else if (key == kUseImportanceSampling)
            mUseImportanceSampling = value;

        else if (key == kSamplesPerPixel)
            mStaticParams.samplesPerPixel = value;
        else if (key == kMaxBounces)
            mStaticParams.maxBounces = value;
        // Differentiable rendering parameters
        else if (key == kDiffMode)
            mStaticParams.diffMode = value;
        else if (key == kDiffVarName)
            mStaticParams.diffVarName = value.operator std::string();

        // Sampling parameters
        else if (key == kSampleGenerator)
            mStaticParams.sampleGenerator = value;
        else if (key == kFixedSeed)
        {
            mParams.fixedSeed = value;
            mParams.useFixedSeed = true;
        }
        else if (key == kUseBSDFSampling)
            mStaticParams.useBSDFSampling = value;
        else if (key == kUseNEE)
            mStaticParams.useNEE = value;
        else if (key == kUseMIS)
            mStaticParams.useMIS = value;

        // WAR parameters
        else if (key == kUseWAR)
            mStaticParams.useWAR = value;
        else if (key == kAuxSampleCount)
            mStaticParams.auxSampleCount = value;
        else if (key == kLog10vMFConcentration)
            mStaticParams.log10vMFConcentration = value;
        else if (key == kLog10vMFConcentrationScreen)
            mStaticParams.log10vMFConcentrationScreen = value;
        else if (key == kBoundaryTermBeta)
            mStaticParams.boundaryTermBeta = value;
        else if (key == kUseAntitheticSampling)
            mStaticParams.useAntitheticSampling = value;
        else
            logWarning("Unknown field `" + key + "` in TestGauss pass");
    }
}
Properties TestGauss::getProperties() const
{
    Properties props;
    props[kComputeDirect] = mComputeDirect;
    props[kUseImportanceSampling] = mUseImportanceSampling;

    // Rendering parameters
    props[kSamplesPerPixel] = mStaticParams.samplesPerPixel;
    props[kMaxBounces] = mStaticParams.maxBounces;

    // Differentiable rendering parameters
    props[kDiffMode] = mStaticParams.diffMode;
    props[kDiffVarName] = mStaticParams.diffVarName;

    // Sampling parameters
    props[kSampleGenerator] = mStaticParams.sampleGenerator;
    if (mParams.useFixedSeed)
        props[kFixedSeed] = mParams.fixedSeed;
    props[kUseBSDFSampling] = mStaticParams.useBSDFSampling;
    props[kUseNEE] = mStaticParams.useNEE;
    props[kUseMIS] = mStaticParams.useMIS;

    // WAR parameters
    props[kUseWAR] = mStaticParams.useWAR;
    props[kAuxSampleCount] = mStaticParams.auxSampleCount;
    props[kLog10vMFConcentration] = mStaticParams.log10vMFConcentration;
    props[kLog10vMFConcentrationScreen] = mStaticParams.log10vMFConcentrationScreen;
    props[kBoundaryTermBeta] = mStaticParams.boundaryTermBeta;
    props[kUseAntitheticSampling] = mStaticParams.useAntitheticSampling;
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
            desc.addHitGroup("closestHitSphereAttrib", "anyHitSphereAttrib", "intersectSphere")
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

DefineList TestGauss::StaticParams::getDefines(const TestGauss& owner) const
{
    DefineList defines;

    defines.add("SAMPLES_PER_PIXEL", std::to_string(samplesPerPixel));
    defines.add("MAX_BOUNCES", std::to_string(maxBounces));

    defines.add("DIFF_MODE", std::to_string((uint32_t)diffMode));
    defines.add(diffVarName);

    defines.add("USE_BSDF_SAMPLING", useBSDFSampling ? "1" : "0");
    defines.add("USE_NEE", useNEE ? "1" : "0");
    defines.add("USE_MIS", useMIS ? "1" : "0");

    // WAR parameters configuration.
    defines.add("USE_WAR", useWAR ? "1" : "0");
    defines.add("AUX_SAMPLE_COUNT", std::to_string(auxSampleCount));
    defines.add("LOG10_VMF_CONCENTRATION", std::to_string(log10vMFConcentration));
    defines.add("LOG10_VMF_CONCENTRATION_SCREEN", std::to_string(log10vMFConcentrationScreen));
    defines.add("BOUNDARY_TERM_BETA", std::to_string(boundaryTermBeta));
    defines.add("USE_ANTITHETIC_SAMPLING", useAntitheticSampling ? "1" : "0");
    defines.add("HARMONIC_GAMMA", std::to_string(harmonicGamma));

    // Sampling utilities configuration.
    FALCOR_ASSERT(owner.mpSampleGenerator);
    defines.add(owner.mpSampleGenerator->getDefines());

    if (owner.mpEmissiveSampler)
        defines.add(owner.mpEmissiveSampler->getDefines());

    // Scene-specific configuration.
    const auto& scene = owner.mpScene;
    if (scene)
        defines.add(scene->getSceneDefines());
    defines.add("USE_ENV_LIGHT", scene && scene->useEnvLight() ? "1" : "0");
    defines.add("USE_ANALYTIC_LIGHTS", scene && scene->useAnalyticLights() ? "1" : "0");
    defines.add("USE_EMISSIVE_LIGHTS", scene && scene->useEmissiveLights() ? "1" : "0");

    return defines;
}

void TestGauss::addRandomGauss()
{
    if (!mpScene)
    {
        logWarning("No scene! Ignoring call to addCustomPrimitive()");
        return;
    }

    std::uniform_real_distribution<float> u(0.f, 1.f);

    ref<GaussMaterial> pRandomGaussMat = GaussMaterial::create(mpDevice, "");
    pRandomGaussMat.get()->setBaseColor(float3(u(rng), u(rng), u(rng)));
    pRandomGaussMat.get()->setCovariance(float3(u(rng), u(rng), u(rng)), float3(u(rng) * M_2_PI, u(rng) * M_2_PI, u(rng) * M_2_PI));
    pRandomGaussMat.get()->setAlpha(u(rng));
    float3 c = {4.f * u(rng) - 2.f, u(rng), 4.f * u(rng) - 2.f};
    float r = 0.5f * u(rng) + 0.5f;
    mpScene->addCustomPrimitiveWithMaterial(mUserID++, AABB(c - r, c + r), pRandomGaussMat);
    mpScene->getMaterialSystem().update(true);
    sceneChanged();

}

