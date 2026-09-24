# CLI tool: headless capture and decode (no Qt required)
add_executable(escope_cli
    escope_cli.cpp
)

target_link_libraries(escope_cli
    PRIVATE
        escope_core
        escope_sim
)

target_include_directories(escope_cli
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core
        ${CMAKE_SOURCE_DIR}/sim
)
