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

TestGauss = render_graph_TestGauss()
try: m.addGraph(TestGauss)
except NameError: None
