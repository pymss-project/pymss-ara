cmake_minimum_required(VERSION 3.22 FATAL_ERROR)

project(PyMSS_ARA VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# -----------------------------------------------------------------------------
# Third-party: JUCE + ARA SDK (managed as git submodules under third_party/)
# -----------------------------------------------------------------------------
add_subdirectory(third_party/JUCE)

# Tell JUCE where the ARA SDK headers live (ARA_API/ and ARA_Library/).
juce_set_ara_sdk_path("${CMAKE_CURRENT_SOURCE_DIR}/third_party/ARA_SDK")

# -----------------------------------------------------------------------------
# Plugin sources
# -----------------------------------------------------------------------------
set(PYMSS_SOURCES
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/PyMSSDocumentController.cpp
    src/PyMSSPlaybackRenderer.cpp
    src/Settings.cpp
    src/WorkerClient.cpp
    src/SeparationEngine.cpp
)

# -----------------------------------------------------------------------------
# Build the ARA effect plugin (VST3, Windows-first).
# -----------------------------------------------------------------------------
juce_add_plugin(PyMSS_ARA
    VERSION             0.1.0
    COMPANY_NAME        "pymss-project"
    BUNDLE_ID           "com.pymss-project.ara"
    PLUGIN_MANUFACTURER_CODE   Pymc
    PLUGIN_CODE               Pmar
    FORMATS                   VST3
    IS_ARA_EFFECT             TRUE
    PRODUCT_NAME              "PyMSS ARA Plugin"
    COPY_PLUGIN_AFTER_BUILD   TRUE
    NEEDS_TIMERS              TRUE
    VST3_CATEGORIES           Tools Effect Audio
    HEADER_SEARCH_PATHS src
)

# juce_add_plugin has no SOURCES keyword; add plugin sources explicitly.
target_sources(PyMSS_ARA PRIVATE ${PYMSS_SOURCES})

target_link_libraries(PyMSS_ARA
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce_ara_sdk
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Brand-new VST3-only plugin: no VST2 compatibility needed.
target_compile_definitions(PyMSS_ARA PUBLIC JUCE_VST3_CAN_REPLACE_VST2=0)

# Allow synchronous modal dialogs (file choosers in the settings panel).
target_compile_definitions(PyMSS_ARA PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)

# -----------------------------------------------------------------------------
# Resource packaging: copy python/worker.py into the VST3 bundle's Resources.
# VST3 bundle layout (Windows): PyMSS_ARA.vst3/Contents/{x86_64-win,Resources}/
# -----------------------------------------------------------------------------
add_custom_command(TARGET PyMSS_ARA_VST3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:PyMSS_ARA_VST3>/../Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/python"
        "$<TARGET_FILE_DIR:PyMSS_ARA_VST3>/../Resources"
    COMMENT "Copying python/ resources into VST3 bundle"
)

# JUCE's COPY_PLUGIN_AFTER_BUILD installs the bundle to the system VST3 folder,
# but it can run BEFORE the resource copy above, leaving the installed bundle
# with stale resources. Also copy python/ into the installed bundle so the
# system copy always has the current worker.py.
if (WIN32)
    set(_installed_bundle "$ENV{ProgramW6432}/Common Files/VST3/PyMSS ARA Plugin.vst3")
    add_custom_command(TARGET PyMSS_ARA_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${_installed_bundle}/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/python"
            "${_installed_bundle}/Contents/Resources"
        COMMENT "Copying python/ resources into installed VST3 bundle"
    )
endif()

