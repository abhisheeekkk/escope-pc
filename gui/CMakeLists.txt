qt_add_executable(embeddedscope
    mainwindow/main.cpp
    mainwindow/MainWindow.cpp
    mainwindow/MainWindow.h
    waveform/WaveformWidget.cpp
    waveform/WaveformWidget.h
    waveform/WaveformRenderer.cpp
    waveform/WaveformRenderer.h
    timeline/TimelineWidget.cpp
    timeline/TimelineWidget.h
    panels/MeasurementPanel.cpp
    panels/MeasurementPanel.h
    panels/ChannelPanel.cpp
    panels/ChannelPanel.h
    panels/ProtocolPanel.cpp
    panels/ProtocolPanel.h
)

target_link_libraries(embeddedscope
    PRIVATE
        escope_core
        escope_sim
        Qt6::Core
        Qt6::Widgets
        Qt6::OpenGL
        Qt6::OpenGLWidgets
        OpenGL::GL
)

target_include_directories(embeddedscope
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/core
        ${CMAKE_SOURCE_DIR}/sim
)

set_target_properties(embeddedscope PROPERTIES
    WIN32_EXECUTABLE TRUE   # No console window on Windows
    MACOSX_BUNDLE   TRUE
)
