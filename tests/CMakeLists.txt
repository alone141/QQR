include(GoogleTest)
add_executable(vfdt_tests
    CalibrationTest.cpp
    CameraSimulatorTest.cpp
    ChecksumTest.cpp
    FountainDecoderTest.cpp
    FountainEncoderTest.cpp
    FrameLayoutTest.cpp
    FrameReaderTest.cpp
    FrameRendererTest.cpp
    GaloisTest.cpp
    HomographyTest.cpp
    ImageTest.cpp
    ManifestTest.cpp
    PacketTest.cpp
    ReedSolomonTest.cpp
    SimulationTest.cpp
    StillEncoderTest.cpp
    SymbolLayoutTest.cpp
    TransmitterTest.cpp
    VideoSourceTest.cpp
)
target_link_libraries(vfdt_tests PRIVATE vfdt_core GTest::gtest_main)
gtest_discover_tests(vfdt_tests DISCOVERY_TIMEOUT 60)

# The decode command itself, which the unit tests do not reach.
add_test(NAME DecodeCommand.RecoversSimulatedRecording
    COMMAND ${CMAKE_COMMAND} -DVFDT=$<TARGET_FILE:vfdt> -DINPUT=${PROJECT_SOURCE_DIR}/testcase/small.txt
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/decode_command -P ${CMAKE_CURRENT_SOURCE_DIR}/DecodeCommandTest.cmake)
