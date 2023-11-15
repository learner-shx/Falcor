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

const uint32_t kMaxPayloadSizeBytes = 16;
const uint32_t kMaxAttributeSizeBytes = 8;
const uint32_t kMaxRecursionDepth = 1;

const char kOutput[] = "output";
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


TestGauss::TestGauss(ref<Device> pDevice, const Properties& props) : RenderPass(pDevice) {}

Properties TestGauss::getProperties() const
{
    return {};
}

RenderPassReflection TestGauss::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    // reflector.addOutput("dst");
    // reflector.addInput("src");
    reflector.addOutput(kOutput, "Output image").bindFlags(ResourceBindFlags::UnorderedAccess).format(ResourceFormat::RGBA32Float);
    return reflector;
}

void TestGauss::setScene(RenderContext* pRenderContext, const ref<Scene>& pScene)
{
    mpScene = pScene;

    mRT.pProgram = nullptr;
    mRT.pVars = nullptr;

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
    desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);
    desc.setMaxPayloadSize(kMaxPayloadSizeBytes);
    desc.setMaxAttributeSize(kMaxAttributeSizeBytes);

    ref<RtBindingTable> sbt;


    // In this mode we test having two different ray types traced against
    // both triangles and custom primitives using intersection shaders.

    sbt = RtBindingTable::create(2, 2, geometryCount);

    // Create hit group shaders.
    auto defaultMtl0 = desc.addHitGroup("closestHitMtl0", "anyHit", "");
    auto defaultMtl1 = desc.addHitGroup("closestHitMtl1", "anyHit", "");

    auto greenMtl = desc.addHitGroup("closestHitGreen", "", "");
    auto redMtl = desc.addHitGroup("closestHitRed", "", "");

    auto sphereDefaultMtl0 = desc.addHitGroup("closestHitSphereMtl0", "", "intersectSphere");
    auto sphereDefaultMtl1 = desc.addHitGroup("closestHitSphereMtl1", "", "intersectSphere");

    auto spherePurple = desc.addHitGroup("closestHitSpherePurple", "", "intersectSphere");
    auto sphereYellow = desc.addHitGroup("closestHitSphereYellow", "", "intersectSphere");

    // Assign default hit groups to all geometries.
    sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), defaultMtl0);
    sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::TriangleMesh), defaultMtl1);

    sbt->setHitGroup(0, mpScene->getGeometryIDs(Scene::GeometryType::Custom), sphereDefaultMtl0);
    sbt->setHitGroup(1, mpScene->getGeometryIDs(Scene::GeometryType::Custom), sphereDefaultMtl1);

    // Override specific hit groups for some geometries.
    for (uint geometryID = 0; geometryID < geometryCount; geometryID++)
    {
        auto type = mpScene->getGeometryType(GlobalGeometryID{geometryID});

        if (type == Scene::GeometryType::TriangleMesh)
        {
            if (geometryID == 1)
            {
                sbt->setHitGroup(0, geometryID, greenMtl);
                sbt->setHitGroup(1, geometryID, redMtl);
            }
            else if (geometryID == 3)
            {
                sbt->setHitGroup(0, geometryID, redMtl);
                sbt->setHitGroup(1, geometryID, greenMtl);
            }
        }
        else if (type == Scene::GeometryType::Custom)
        {
            uint32_t index = mpScene->getCustomPrimitiveIndex(GlobalGeometryID{geometryID});
            uint32_t userID = mpScene->getCustomPrimitive(index).userID;

            // Use non-default material for custom primitives with even userID.
            if (userID % 2 == 0)
            {
                sbt->setHitGroup(0, geometryID, spherePurple);
                sbt->setHitGroup(1, geometryID, sphereYellow);
            }
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
