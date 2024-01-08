from falcor import *

def render_graph_TestGauss():
    g = RenderGraph("TestGauss")
    AccumulatePass = createPass("AccumulatePass", {'enabled': True, 'precisionMode': 'Single'})
    g.addPass(AccumulatePass, "AccumulatePass")
    ToneMapper = createPass("ToneMapper", {'autoExposure': False, 'exposureCompensation': 0.0})
    g.addPass(ToneMapper, "ToneMapper")
    TestGauss = createPass("TestGauss", {'maxBounces': 3})
    g.addPass(TestGauss, "TestGauss")
    VBufferRT = createPass("VBufferRT", {'samplePattern': 'Stratified', 'sampleCount': 16})
    g.addPass(VBufferRT, "VBufferRT")
    g.addEdge("AccumulatePass.output", "ToneMapper.src")
    g.addEdge("VBufferRT.vbuffer", "TestGauss.vbuffer")
    g.addEdge("VBufferRT.viewW", "TestGauss.viewW")
    g.addEdge("TestGauss.color", "AccumulatePass.input")
    g.markOutput("ToneMapper.dst")
    return g

def render_graph_TestGaussPT():
    g = RenderGraph("TestGauss")
    TestGauss = createPass("TestGauss",
                                   {
                                       "maxBounces": 0,
                                       "samplesPerPixel": 1,
                                       "diffMode": "Primal",
                                       "diffVarName": "CBOX_BUNNY_MATERIAL",
                                    #    "diffVarName": "CBOX_BUNNY_TRANSLATION",
                                   })
    g.addPass(TestGauss, "TestGauss")

    AccumulatePassPrimal = createPass("AccumulatePass", {"enabled": True, "precisionMode": "Single"})
    g.addPass(AccumulatePassPrimal, "AccumulatePassPrimal")

    AccumulatePassDiff = createPass("AccumulatePass", {"enabled": True, 'precisionMode': "Single"})
    g.addPass(AccumulatePassDiff, "AccumulatePassDiff")
    ColorMapPassDiff = createPass("ColorMapPass", {"minValue": -4.0, "maxValue": 4.0, "autoRange": False})
    g.addPass(ColorMapPassDiff, "ColorMapPassDiff")

    g.addEdge("TestGauss.color", "AccumulatePassPrimal.input")
    g.addEdge("TestGauss.dColor", "AccumulatePassDiff.input")
    g.addEdge("AccumulatePassDiff.output", "ColorMapPassDiff.input")
    g.markOutput("AccumulatePassDiff.output")
    g.markOutput("AccumulatePassPrimal.output")
    g.markOutput("ColorMapPassDiff.output")
    return g
TestGauss = render_graph_TestGaussPT()
try: m.addGraph(TestGauss)

except NameError: None

flags = SceneBuilderFlags.DontMergeMaterials | SceneBuilderFlags.RTDontMergeDynamic | SceneBuilderFlags.DontOptimizeMaterials
# m.loadScene("test_scenes/testCustom.pyscene", buildFlags=flags)
m.loadScene("inv_rendering_scenes/testCustom_ref.pyscene", buildFlags=flags)
#
