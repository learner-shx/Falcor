from pathlib import WindowsPath, PosixPath
from falcor import *

def render_graph_DefaultRenderGraph():
    g = RenderGraph('DefaultRenderGraph')
    g.create_pass('TestGauss', 'TestGauss', {})
    g.mark_output('TestGauss.output')
    return g

DefaultRenderGraph = render_graph_DefaultRenderGraph()
try: m.addGraph(DefaultRenderGraph)

except NameError: None
flags = SceneBuilderFlags.DontMergeMaterials | SceneBuilderFlags.RTDontMergeDynamic | SceneBuilderFlags.DontOptimizeMaterials
m.loadScene("test_scenes/testCustom.pyscene", buildFlags=flags)

