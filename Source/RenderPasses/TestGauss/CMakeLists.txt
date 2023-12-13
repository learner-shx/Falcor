add_plugin(TestGauss)

target_sources(TestGauss PRIVATE
    Params.slang
    PTUtils.slang
    StaticParams.slang
    TestGauss.cpp
    TestGauss.h
    TestGaussPathTracer.rt.slang
    WarpedAreaReparam.slang
)

target_copy_shaders(TestGauss RenderPasses/TestGauss)

target_source_group(TestGauss "RenderPasses")
