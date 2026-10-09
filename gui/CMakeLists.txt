qt_add_executable(embeddedscope
    mainwindow/main.cpp
    theme/Theme.h
    mainwindow/MainWindow.cpp
    mainwindow/MainWindow.h
    waveform/WaveformWidget.cpp
    waveform/WaveformWidget.h
    waveform/WaveformRenderer.cpp
    waveform/WaveformRenderer.h
    waveform/AnnotationPainter.cpp
    waveform/AnnotationPainter.h
    panels/ChannelPanel.cpp
    panels/ChannelPanel.h
    panels/ProtocolPanel.cpp
    panels/ProtocolPanel.h
    resources/fonts.qrc
)

target_link_libraries(embeddedscope
    PRIVATE
        escope_core
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
)

set_target_properties(embeddedscope PROPERTIES
    WIN32_EXECUTABLE TRUE   # No console window on Windows
    MACOSX_BUNDLE   TRUE
)

# Developer tool: renders the protocol annotation lane for a simulated I2C bus to PNGs
qt_add_executable(annotation_preview
    waveform/preview/annotation_preview.cpp
    waveform/AnnotationPainter.cpp
)
target_link_libraries(annotation_preview PRIVATE escope_core Qt6::Core Qt6::Gui)
target_include_directories(annotation_preview PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/core
)
