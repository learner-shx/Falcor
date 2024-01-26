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
#include "De18Material.h"
#include "De18MaterialParamLayout.slang"
#include "Utils/Scripting/ScriptBindings.h"
#include "GlobalState.h"

namespace Falcor
{
    namespace
    {
        const char kShaderFile[] = "Rendering/Materials/De18Material.slang";
    }

    De18Material::De18Material(ref<Device> pDevice, const std::string& name)
        : BasicMaterial(pDevice, name, MaterialType::PBRTConductor)
    {
        // Setup additional texture slots.
        mTextureSlotInfo[(uint32_t)TextureSlot::BaseColor] = { "baseColor", TextureChannelFlags::RGBA, false };
        mTextureSlotInfo[(uint32_t)TextureSlot::Transmission] = { "roughness", TextureChannelFlags::Red | TextureChannelFlags::Green, false };
        // mTextureSlotInfo[(uint32_t)TextureSlot::Roughness] = { "Roughness", TextureChannelFlags::Red | TextureChannelFlags::Green, false };
        mTextureSlotInfo[(uint32_t)TextureSlot::Specular] = { "specular", TextureChannelFlags::RGB, false };
        mTextureSlotInfo[(uint32_t)TextureSlot::Normal] = { "normal", TextureChannelFlags::RGB, false };
        mTextureSlotInfo[(uint32_t)TextureSlot::Emissive] = { "tangent", TextureChannelFlags::RGB, false };
    }

    ProgramDesc::ShaderModuleList De18Material::getShaderModules() const
    {
        return { ProgramDesc::ShaderModule::fromFile(kShaderFile) };
    }

    TypeConformanceList De18Material::getTypeConformances() const
    {
        return { {{"De18Material", "IMaterial"}, (uint32_t)MaterialType::PBRTConductor} };
    }

    void De18Material::renderSpecularUI(Gui::Widgets& widget)
    {
        float2 roughness = getRoughness();
        if (widget.var("X Roughness", roughness.x, 0.f, 1.f, 0.01f)) setRoughness(roughness);
        if (widget.var("Y Roughness", roughness.y, 0.f, 1.f, 0.01f)) setRoughness(roughness);
    }

    void De18Material::setRoughness(float2 roughness)
    {
        if (mData.transmission[0] != (float16_t)roughness.x || mData.transmission[1] != (float16_t)roughness.y)
        {
            mData.transmission[0] = (float16_t)roughness.x;
            mData.transmission[1] = (float16_t)roughness.y;
            markUpdates(UpdateFlags::DataChanged);
        }
    }

    void De18Material::setTangent(float3 tangent)
    {
        if (mData.emissive[0] != (float16_t)tangent.x || mData.emissive[1] != (float16_t)tangent.y || mData.emissive[2] != (float16_t)tangent.z)
        {
            mData.emissive = tangent;
            markUpdates(UpdateFlags::DataChanged);
        }
    }

    const MaterialParamLayout& De18Material::getParamLayout() const
    {
        return De18MaterialParamLayout::layout();
    }

    SerializedMaterialParams De18Material::serializeParams() const
    {
        return De18MaterialParamLayout::serialize(this);
    }

    void De18Material::deserializeParams(const SerializedMaterialParams& params)
    {
        De18MaterialParamLayout::deserialize(this, params);
    }

    FALCOR_SCRIPT_BINDING(De18Material)
    {
        using namespace pybind11::literals;

        FALCOR_SCRIPT_BINDING_DEPENDENCY(BasicMaterial)

        pybind11::class_<De18Material, BasicMaterial, ref<De18Material>> material(m, "De18Material");
        auto create = [] (const std::string& name)
        {
            return De18Material::create(accessActivePythonSceneBuilder().getDevice(), name);
        };
        material.def(pybind11::init(create), "name"_a = ""); // PYTHONDEPRECATED

        material.def_property("roughness", &De18Material::getRoughness, &De18Material::setRoughness);
    }
}
