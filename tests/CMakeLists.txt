add_executable(escope_tests
    core/test_SampleBuffer.cpp
    core/test_DigitalBuffer.cpp
    core/test_TriggerEngine.cpp
    core/test_Measurements.cpp
    core/test_UartDecoder.cpp
    core/test_I2CDecoder.cpp
    core/test_CANDecoder.cpp
    core/test_SPIDecoder.cpp
    core/test_BurstFrame.cpp
    core/test_AnnotationLayout.cpp
    core/test_SessionSerializer.cpp
    core/test_DecoderFixtures.cpp
    core/test_ProtocolTrigger.cpp
)

target_link_libraries(escope_tests
    PRIVATE
        escope_core
        escope_sim
        GTest::gtest_main
)

target_include_directories(escope_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core
        ${CMAKE_SOURCE_DIR}/sim
)

target_compile_definitions(escope_tests PRIVATE ESCOPE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(GoogleTest)
gtest_discover_tests(escope_tests)
