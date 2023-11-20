add_plugin(TestGauss)

target_sources(TestGauss PRIVATE
    TestGauss.cpp
    TestGauss.h
    TestGauss.rt.slang
)

target_copy_shaders(TestGauss RenderPasses/TestGauss)

target_source_group(TestGauss "RenderPasses")
