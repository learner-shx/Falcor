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
#include "GaussMaterial.h"
#include "Core/API/Device.h"
#include "Utils/Logger.h"
#include "Utils/Scripting/ScriptBindings.h"
#include "GlobalState.h"
#include "Scene/Material/MaterialSystem.h"
#include "Scene/Material/DiffuseSpecularUtils.h"
#include "Utils/Math/Common.h"
#include "GaussMaterialParamLayout.slang"

namespace Falcor
{
    namespace
    {
        static_assert((sizeof(MaterialHeader) + sizeof(GaussMaterialData)) <= sizeof(MaterialDataBlob), "GaussMaterialData is too large");

        const char kShaderFile[] = "Rendering/Materials/GaussMaterial.slang";
    }

    GaussMaterial::GaussMaterial(ref<Device> pDevice, const std::string& name)
        : Material(pDevice, name, MaterialType::Gauss)
    {
    }

    bool GaussMaterial::renderUI(Gui::Widgets& widget)
    {
        bool changed = false;

        return changed;
    }

    Material::UpdateFlags GaussMaterial::update(MaterialSystem* pOwner)
    {
        FALCOR_ASSERT(pOwner);

        auto flags = Material::UpdateFlags::None;
        if (mUpdates != Material::UpdateFlags::None)
        {
            // uint32_t bufferID = pOwner->addBuffer(mpBRDFData);
            // uint32_t samplerID = pOwner->addTextureSampler(mpLUTSampler);

            // if (mData.bufferID != bufferID || mData.samplerID != samplerID)
            // {
            //     mUpdates |= Material::UpdateFlags::DataChanged;
            // }
            // mData.bufferID = bufferID;
            // mData.samplerID = samplerID;

            // updateTextureHandle(pOwner, mpAlbedoLUT, mData.texAlbedoLUT);

            flags |= mUpdates;
            mUpdates = Material::UpdateFlags::None;
        }

        return flags;
    }

    bool GaussMaterial::isEqual(const ref<Material>& pOther) const
    {
        auto other = dynamic_ref_cast<GaussMaterial>(pOther);
        if (!other) return false;

        if (!isBaseEqual(*other)) return false;
        if (mPath != other->mPath) return false;

        return true;
    }

    ProgramDesc::ShaderModuleList GaussMaterial::getShaderModules() const
    {
        return { ProgramDesc::ShaderModule::fromFile(kShaderFile) };
    }

    TypeConformanceList GaussMaterial::getTypeConformances() const
    {
        return { {{"GaussMaterial", "IMaterial"}, (uint32_t)MaterialType::Gauss} };
    }

    const MaterialParamLayout& GaussMaterial::getParamLayout() const
    {
        return GaussMaterialParamLayout::layout();
    }

    SerializedMaterialParams GaussMaterial::serializeParams() const
    {
        return GaussMaterialParamLayout::serialize(this);
    }

    void GaussMaterial::deserializeParams(const SerializedMaterialParams& params)
    {
        GaussMaterialParamLayout::deserialize(this, params);
    }

    void GaussMaterial::setCovDiag(float3 covDiag)
    {
        if (any(mData.covDiag != covDiag))
        {
            mData.covDiag = covDiag;
            markUpdates(UpdateFlags::DataChanged);
        }
    }

    void GaussMaterial::setCovOffDiag(float3 covOffDiag)
    {
        if (any(mData.covOffDiag != covOffDiag))
        {
            mData.covOffDiag = covOffDiag;
            markUpdates(UpdateFlags::DataChanged);
        }
    }

    void GaussMaterial::setBaseColor(float3 baseColor)
    {
        if (any(mData.baseColor != baseColor))
        {
            mData.baseColor = baseColor;
            markUpdates(UpdateFlags::DataChanged);
        }
    }

    void GaussMaterial::setAlpha(float alpha)
    {
        if (mData.alpha != alpha)
        {
            mData.alpha = alpha;
            markUpdates(UpdateFlags::DataChanged);
        }
    }

    float3x3 getRotationMatrix(float theta, float phi)
    {
        float3x3 R;
        R[0][0] = cos(theta) * cos(phi);
        R[0][1] = -sin(phi);
        R[0][2] = sin(theta) * cos(phi);
        R[1][0] = cos(theta) * sin(phi);
        R[1][1] = cos(phi);
        R[1][2] = sin(theta) * sin(phi);
        R[2][0] = -sin(theta);
        R[2][1] = 0;
        R[2][2] = cos(theta);
        return R;
    }

    float3x3 diag(float3 sigma)
    {
        float3x3 diag;
        diag[0][0] = sigma.x;
        diag[0][1] = 0;
        diag[0][2] = 0;
        diag[1][0] = 0;
        diag[1][1] = sigma.y;
        diag[1][2] = 0;
        diag[2][0] = 0;
        diag[2][1] = 0;
        diag[2][2] = sigma.z;
        return diag;

    }

    void GaussMaterial::setCovariance(float3 sigma, float3 angles)
    {
        float4x4 R44 = mul(mul(matrixFromRotation(angles.x, float3(1, 0, 0)), matrixFromRotation(angles.y, float3(0, 1, 0))), matrixFromRotation(angles.z, float3(0, 0, 1)));
        // float4x4 R44 = mul(matrixFromRotation(angles.x, float3(1, 0, 0)), matrixFromRotation(angles.y, float3(0, 1, 0)));
        // float4x4 R44 = rotate(float4x4::identity(), angles.x, float3(1, 0, 0));
        float3x3 R = float3x3{R44[0][0], R44[0][1], R44[0][2],
                              R44[1][0], R44[1][1], R44[1][2],
                              R44[2][0], R44[2][1], R44[2][2]};
        float3x3 cov = mul(R, mul(diag(sigma), transpose(R)));
        mData.covDiag = float3(cov[0][0], cov[1][1], cov[2][2]);
        mData.covOffDiag = float3(cov[0][1], cov[0][2], cov[1][2]);
    }

    FALCOR_SCRIPT_BINDING(GaussMaterial)
    {
        using namespace pybind11::literals;

        FALCOR_SCRIPT_BINDING_DEPENDENCY(Material)

        pybind11::class_<GaussMaterial, Material, ref<GaussMaterial>> material(m, "GaussMaterial");
        auto create = [] (const std::string& name)
        {
            return GaussMaterial::create(accessActivePythonSceneBuilder().getDevice(), name);
        };
        material.def(pybind11::init(create), "name"_a); // PYTHONDEPRECATED

        material.def_property("covDiag", &GaussMaterial::getCovDiag, &GaussMaterial::setCovDiag);
        material.def_property("covOffDiag", &GaussMaterial::getCovOffDiag, &GaussMaterial::setCovOffDiag);
        material.def_property("baseColor", &GaussMaterial::getBaseColor, &GaussMaterial::setBaseColor);
        material.def_property("alpha", &GaussMaterial::getAlpha, &GaussMaterial::setAlpha);
    }
}
