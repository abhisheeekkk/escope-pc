add_executable(escope_tests
    core/test_SampleBuffer.cpp
    core/test_DigitalBuffer.cpp
    core/test_TriggerEngine.cpp
    core/test_Measurements.cpp
    core/test_UartDecoder.cpp
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

include(GoogleTest)
gtest_discover_tests(escope_tests)
