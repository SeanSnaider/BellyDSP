# Build file for the whole project. CMake doesn't compile anything itself: it generates the real
# build instructions (Makefiles here) from this description, a bit like Maven's pom.xml.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release     # configure (once, or after editing this file)
#   cmake --build build -j                        # compile
#   ctest --test-dir build --output-on-failure    # run the tests

cmake_minimum_required(VERSION 3.22)

# Apple Silicon Macs start at macOS 11. Has to be set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(AmpSim VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)            # NeuralAmpModelerCore needs C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# JUCE: audio device I/O, the standalone app wrapper, GUI, and DSP helpers (convolution).
add_subdirectory(third_party/JUCE)

# ---------------------------------------------------------------------------------------------
# NeuralAmpModelerCore, the official NAM inference engine, as a plain static library (no JUCE).
set(NAM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NeuralAmpModelerCore)
file(GLOB NAM_SOURCES CONFIGURE_DEPENDS ${NAM_DIR}/NAM/*.cpp ${NAM_DIR}/NAM/*/*.cpp)

add_library(nam_core STATIC ${NAM_SOURCES})
target_include_directories(nam_core SYSTEM PUBLIC
    ${NAM_DIR}
    ${NAM_DIR}/Dependencies/eigen
    ${NAM_DIR}/Dependencies/nlohmann)
target_compile_definitions(nam_core PUBLIC
    NAM_SAMPLE_FLOAT        # process float buffers directly (JUCE's sample type), not double
    NAM_ENABLE_A2_FAST)     # NAM's hand-optimized path for A2 models (its default)
target_compile_features(nam_core PUBLIC cxx_std_20)
# Always optimize NAM, even in Debug builds, or the network can't keep up with real time.
target_compile_options(nam_core PRIVATE -O3 -w)

# Each NAM architecture (WaveNet, LSTM, ...) registers itself from a static object in its own .cpp
# file, and nothing else references those objects. A normal static-library link drops them, which
# leaves get_dsp() with "No config parser registered for architecture: WaveNet". WHOLE_ARCHIVE
# (-force_load on macOS) keeps every object.
set(NAM_CORE_WHOLE "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")

# ---------------------------------------------------------------------------------------------
# Our DSP blocks. Compiled into each target that uses them, because JUCE modules must not be
# linked into a shared static library and then again into the app.
set(AMPSIM_DSP_SOURCES
    src/dsp/AmpSection.cpp
    src/dsp/Cab.cpp
    src/dsp/CabPack.cpp
    src/dsp/MinimumPhase.cpp
    src/dsp/FftDouble.cpp
    src/dsp/Loudness.cpp
    src/dsp/ReferenceSignals.cpp
    src/dsp/Chain.cpp
    src/dsp/NamAmp.cpp
    src/dsp/CabIR.cpp
    src/dsp/CutFilter.cpp
    src/dsp/Equalizer.cpp
    src/dsp/Compressor.cpp
    src/dsp/ModulatedDelay.cpp
    src/dsp/Chorus.cpp
    src/dsp/Tempo.cpp
    src/dsp/Delay.cpp
    src/dsp/Reverb.cpp
    src/dsp/Bitcrush.cpp
    src/dsp/Phaser.cpp
    src/dsp/Flanger.cpp
    src/dsp/Bloom.cpp)

set(AMPSIM_JUCE_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

# ---------------------------------------------------------------------------------------------
# The app. FORMATS Standalone builds a macOS .app; adding VST3 or AU later is a one-word change.
juce_add_plugin(AmpSim
    COMPANY_NAME "Sean Snaider"
    PRODUCT_NAME "Amp Sim"
    BUNDLE_ID com.seansnaider.ampsim
    PLUGIN_MANUFACTURER_CODE Ssna
    PLUGIN_CODE Amps
    FORMATS Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE          # for the footswitch later
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Amp Sim needs audio input to hear your guitar.")

target_sources(AmpSim PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/BlockParameters.cpp
    src/Presets.cpp
    ${AMPSIM_DSP_SOURCES})
target_include_directories(AmpSim PRIVATE src)
target_compile_definitions(AmpSim PUBLIC ${AMPSIM_JUCE_DEFINITIONS})
target_link_libraries(AmpSim
    PRIVATE
        ${NAM_CORE_WHOLE}
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------------
# Offline renderer: WAV in, the same chain, WAV out, plus CPU timing.
juce_add_console_app(ampsim_render PRODUCT_NAME "ampsim_render")
target_sources(ampsim_render PRIVATE tools/render/Main.cpp ${AMPSIM_DSP_SOURCES})
target_include_directories(ampsim_render PRIVATE src)
target_compile_definitions(ampsim_render PRIVATE ${AMPSIM_JUCE_DEFINITIONS})
target_link_libraries(ampsim_render
    PRIVATE
        ${NAM_CORE_WHOLE}
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------------
# NeuralAmpModelerCore's own render tool, built unchanged from its source. The tests run it as the
# reference ("oracle") that our NAM slot must match.
add_executable(nam_render
    ${NAM_DIR}/tools/render.cpp
    ${NAM_DIR}/Dependencies/AudioDSPTools/dsp/wav.cpp)
target_include_directories(nam_render PRIVATE ${NAM_DIR}/Dependencies/AudioDSPTools/dsp)
target_link_libraries(nam_render PRIVATE ${NAM_CORE_WHOLE})
target_compile_options(nam_render PRIVATE -O3 -w)

# ---------------------------------------------------------------------------------------------
# Tests: run with `ctest --test-dir build --output-on-failure`, or run build/.../ampsim_tests
# directly to see every measurement. Proof artifacts (renders, editor snapshots, summary.txt)
# land in build/proof.
enable_testing()

juce_add_console_app(ampsim_tests PRODUCT_NAME "ampsim_tests")
target_sources(ampsim_tests PRIVATE
    tests/TestMain.cpp
    tests/TestHelpers.cpp
    tests/AllocationTracking.cpp
    tests/HandoffTests.cpp
    tests/SvfTests.cpp
    tests/AmpSectionTests.cpp
    tests/LoudnessTests.cpp
    tests/CabNormalizationStudy.cpp
    tests/GainTests.cpp
    tests/CabIRTests.cpp
    tests/CabTests.cpp
    tests/CabPackTests.cpp
    tests/CompressorTests.cpp
    tests/EqualizerTests.cpp
    tests/PrimitivesTests.cpp
    tests/DelayTests.cpp
    tests/ChorusTests.cpp
    tests/ReverbTests.cpp
    tests/BloomTests.cpp
    tests/Plot.cpp
    tests/NamAmpTests.cpp
    tests/ChainTests.cpp
    tests/ProcessorTests.cpp
    tests/RealtimeSafetyTests.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/BlockParameters.cpp
    src/Presets.cpp
    ${AMPSIM_DSP_SOURCES})
target_include_directories(ampsim_tests PRIVATE src tests)
target_compile_definitions(ampsim_tests PRIVATE
    ${AMPSIM_JUCE_DEFINITIONS}
    AMPSIM_NAM_DIR="${NAM_DIR}"
    AMPSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    AMPSIM_NAM_RENDER="$<TARGET_FILE:nam_render>")
target_link_libraries(ampsim_tests
    PRIVATE
        ${NAM_CORE_WHOLE}
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
add_dependencies(ampsim_tests nam_render)

add_test(NAME ampsim_tests
         COMMAND ampsim_tests --proof-dir ${CMAKE_BINARY_DIR}/proof
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------------------------------------
# Hardware check: opens the interface through JUCE's CoreAudio backend (the app's code path),
# outputs silence, and reports callback timing and per-channel input levels.
juce_add_console_app(ampsim_device_probe PRODUCT_NAME "ampsim_device_probe")
target_sources(ampsim_device_probe PRIVATE tools/device_probe/Main.cpp)
target_compile_definitions(ampsim_device_probe PRIVATE ${AMPSIM_JUCE_DEFINITIONS})
target_link_libraries(ampsim_device_probe
    PRIVATE
        juce::juce_audio_devices
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
