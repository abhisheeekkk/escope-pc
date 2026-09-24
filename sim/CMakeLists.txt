add_library(escope_sim STATIC
    SimulatedSource.cpp
)

target_link_libraries(escope_sim
    PUBLIC escope_core
)

target_include_directories(escope_sim
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
