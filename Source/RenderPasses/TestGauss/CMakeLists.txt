add_plugin(TestGauss)

target_sources(TestGauss PRIVATE
    TestGauss.cpp
    TestGauss.h
)

target_source_group(TestGauss "RenderPasses")
