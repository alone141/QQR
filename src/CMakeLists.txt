add_library(qqr_core
    Calibration.cpp
    CameraSimulator.cpp
    Checksum.cpp
    FountainDecoder.cpp
    FountainEncoder.cpp
    FrameLayout.cpp
    FrameReader.cpp
    FrameRenderer.cpp
    Galois.cpp
    Homography.cpp
    Image.cpp
    Manifest.cpp
    Packet.cpp
    Receiver.cpp
    ReedSolomon.cpp
    ScreenPlayer.cpp
    Simulation.cpp
    StillEncoder.cpp
    SymbolLayout.cpp
    Transmitter.cpp
    VideoSource.cpp
)
target_include_directories(qqr_core PUBLIC ${PROJECT_SOURCE_DIR}/include)

if(WIN32)
    target_compile_definitions(qqr_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(qqr_core PRIVATE dwmapi)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(qqr_core PUBLIC Threads::Threads)
endif()
