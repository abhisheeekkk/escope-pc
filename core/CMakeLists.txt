# core — pure C++20, NO Qt dependency
# This library must remain Qt-free so it can be:
#   - unit-tested headlessly in CI
#   - used from CLI tools
#   - potentially exposed via a future REST/IPC interface

add_library(escope_core STATIC
    acquisition/SampleBuffer.cpp
    acquisition/DigitalBuffer.cpp
    acquisition/TriggerEngine.cpp
    analysis/Measurements.cpp
    analysis/FFT.cpp
    decoders/base/DecoderRegistry.cpp
    decoders/base/AnnotationLayout.cpp
    decoders/uart/UartDecoder.cpp
    decoders/spi/SPIDecoder.cpp
    decoders/i2c/I2CDecoder.cpp
    decoders/can/CANDecoder.cpp
    session/CaptureSession.cpp
    session/SessionSerializer.cpp
    session/EdgeCodec.cpp
    hal/IDataSource.cpp
    hal/StmDataSource.cpp
)

target_include_directories(escope_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}      # so #include "acquisition/SampleBuffer.h" works
        ${CMAKE_SOURCE_DIR}/core         # alias for the same
)

target_link_libraries(escope_core
    PUBLIC
        Threads::Threads
)

target_compile_features(escope_core PUBLIC cxx_std_20)
