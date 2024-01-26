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
#include "OpenSVBRDFMaterial.h"
#include "OpenSVBRDFMaterialParamLayout.slang"
#include "Utils/Scripting/ScriptBindings.h"
#include "GlobalState.h"

namespace Falcor
{
    namespace
    {
        const char kShaderFile[] = "Rendering/Materials/OpenSVBRDFMaterial.slang";
    }

    OpenSVBRDFMaterial::OpenSVBRDFMaterial(ref<Device> pDevice, const std::string& name)
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

    ProgramDesc::ShaderModuleList OpenSVBRDFMaterial::getShaderModules() const
    {
        return { ProgramDesc::ShaderModule::fromFile(kShaderFile) };
    }

    TypeConformanceList OpenSVBRDFMaterial::getTypeConformances() const
    {
        return { {{"OpenSVBRDFMaterial", "IMaterial"}, (uint32_t)MaterialType::PBRTConductor} };
    }

    void OpenSVBRDFMaterial::renderSpecularUI(Gui::Widgets& widget)
    {
        float2 roughness = getRoughness();
        if (widget.var("X Roughness", roughness.x, 0.f, 1.f, 0.01f)) setRoughness(roughness);
        if (widget.var("Y Roughness", roughness.y, 0.f, 1.f, 0.01f)) setRoughness(roughness);
    }

    void OpenSVBRDFMaterial::setRoughness(float2 roughness)
    {
        if (mData.transmission[0] != (float16_t)roughness.x || mData.transmission[1] != (float16_t)roughness.y)
        {
            mData.transmission[0] = (float16_t)roughness.x;
            mData.transmission[1] = (float16_t)roughness.y;
            markUpdates(UpdateFlags::DataChanged);
        }
    }

    void OpenSVBRDFMaterial::setTangent(float3 tangent)
    {
        if (mData.emissive[0] != (float16_t)tangent.x || mData.emissive[1] != (float16_t)tangent.y || mData.emissive[2] != (float16_t)tangent.z)
        {
            mData.emissive = tangent;
            markUpdates(UpdateFlags::DataChanged);
        }
    }

    const MaterialParamLayout& OpenSVBRDFMaterial::getParamLayout() const
    {
        return OpenSVBRDFMaterialParamLayout::layout();
    }

    SerializedMaterialParams OpenSVBRDFMaterial::serializeParams() const
    {
        return OpenSVBRDFMaterialParamLayout::serialize(this);
    }

    void OpenSVBRDFMaterial::deserializeParams(const SerializedMaterialParams& params)
    {
        OpenSVBRDFMaterialParamLayout::deserialize(this, params);
    }

    FALCOR_SCRIPT_BINDING(OpenSVBRDFMaterial)
    {
        using namespace pybind11::literals;

        FALCOR_SCRIPT_BINDING_DEPENDENCY(BasicMaterial)

        pybind11::class_<OpenSVBRDFMaterial, BasicMaterial, ref<OpenSVBRDFMaterial>> material(m, "OpenSVBRDFMaterial");
        auto create = [] (const std::string& name)
        {
            return OpenSVBRDFMaterial::create(accessActivePythonSceneBuilder().getDevice(), name);
        };
        material.def(pybind11::init(create), "name"_a = ""); // PYTHONDEPRECATED

        material.def_property("roughness", &OpenSVBRDFMaterial::getRoughness, &OpenSVBRDFMaterial::setRoughness);
    }
}
