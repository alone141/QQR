include(GoogleTest)
add_executable(vfdt_tests
    CameraSimulatorTest.cpp
    ChecksumTest.cpp
    FountainDecoderTest.cpp
    FountainEncoderTest.cpp
    FrameLayoutTest.cpp
    FrameReaderTest.cpp
    FrameRendererTest.cpp
    HomographyTest.cpp
    ManifestTest.cpp
    PacketTest.cpp
    ReedSolomonTest.cpp
    SimulationTest.cpp
    SymbolLayoutTest.cpp
    TransmitterTest.cpp
)
target_link_libraries(vfdt_tests PRIVATE vfdt_core GTest::gtest_main)
gtest_discover_tests(vfdt_tests DISCOVERY_TIMEOUT 60)
