# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

# Build file for the whole project. CMake doesn't compile anything itself: it generates the real
# build instructions (Makefiles here) from this description, a bit like Maven's pom.xml.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release     # configure (once, or after editing this file)
#   cmake --build build -j                        # compile
#   ctest --test-dir build --output-on-failure    # run the tests
#
# Release builds (universal, signed, with the auto-updater) are made by tools/release/release.sh, which
# configures a separate build-release/ directory with the options below (docs/RELEASING.md).

cmake_minimum_required(VERSION 3.22)

# Apple Silicon Macs start at macOS 11. Has to be set before project(). Builds with the auto-updater
# (AMPSIM_UPDATER, below) need macOS 12, because Sparkle 2.10 does (ASSUMPTIONS DS5).
if(AMPSIM_UPDATER)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

# The version comes from the VERSION file (one line, major.minor.patch); the release script passes
# -DAMPSIM_VERSION=... for the version it's building. It becomes CFBundleShortVersionString and
# CFBundleVersion on macOS (Sparkle compares the latter, so it must only ever go up) and the version
# WinSparkle compares on Windows.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AMPSIM_VERSION_FROM_FILE LIMIT_COUNT 1)
if(NOT DEFINED AMPSIM_VERSION OR AMPSIM_VERSION STREQUAL "")
    set(AMPSIM_VERSION "${AMPSIM_VERSION_FROM_FILE}")
endif()
if(NOT AMPSIM_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "The version must be major.minor.patch (got '${AMPSIM_VERSION}')")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/release.conf"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/deps.conf")

project(BellyDSP VERSION ${AMPSIM_VERSION} LANGUAGES C CXX)

# ---------------------------------------------------------------------------------------------
# Release options. A plain `cmake -B build` (Sean's dev build) leaves them all off: native arch, no
# updater, so a dev build never replaces itself with a release.
option(AMPSIM_UPDATER "Embed the auto-updater (Sparkle on macOS, WinSparkle on Windows): release builds" OFF)
option(AMPSIM_WITH_ASIO "Windows: build with ASIO driver support (licensing: docs/RELEASING.md)" OFF)
set(AMPSIM_X86_SIMD "AVX" CACHE STRING "x86_64 baseline instruction set for the Intel slice and Windows: AVX or SSE2")

# Values from tools/release/release.conf and tools/deps.conf (KEY=VALUE lines), unless given with -D.
function(ampsim_conf key file out)
    if(DEFINED ${out})
        return()
    endif()
    file(STRINGS "${file}" lines REGEX "^${key}=")
    set(value "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^${key}=" "" value "${line}")
    endforeach()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()
ampsim_conf(RELEASES_REPO "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/release.conf" AMPSIM_RELEASES_REPO)
ampsim_conf(ED_PUBLIC_KEY "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/release.conf" AMPSIM_ED_PUBLIC_KEY)
# How JUCE is licensed (AGPLv3: BellyDSP is AGPL-3.0-or-later, LICENSE) and where the source is, for the
# notices and the brand menu's "Source code for this version". Every build reads them from release.conf,
# so a dev build says the same as a release.
ampsim_conf(JUCE_LICENCE "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/release.conf" AMPSIM_JUCE_LICENCE)
ampsim_conf(SOURCE_URL "${CMAKE_CURRENT_SOURCE_DIR}/tools/release/release.conf" AMPSIM_SOURCE_URL)
ampsim_conf(SPARKLE_VERSION "${CMAKE_CURRENT_SOURCE_DIR}/tools/deps.conf" AMPSIM_SPARKLE_VERSION)
ampsim_conf(WINSPARKLE_VERSION "${CMAKE_CURRENT_SOURCE_DIR}/tools/deps.conf" AMPSIM_WINSPARKLE_VERSION)

# The appcast URL: GitHub's "latest release" asset link, which redirects to the newest release's file,
# so publishing a release is all it takes (no web hosting). One feed per platform.
if(WIN32)
    set(_appcast_name "appcast-windows.xml")
else()
    set(_appcast_name "appcast.xml")
endif()
if(NOT DEFINED AMPSIM_FEED_URL)
    set(AMPSIM_FEED_URL "https://github.com/${AMPSIM_RELEASES_REPO}/releases/latest/download/${_appcast_name}")
endif()

# The Intel slice and Windows: AVX, which every Intel Mac that runs macOS 11 has (Ivy Bridge and later;
# the 2013 Mac Pro has AVX but not AVX2) and nearly every Windows PC since 2011. Eigen, under NAM core,
# vectorises with it. SSE2 is the fallback for very old or low-end PCs (ASSUMPTIONS DS4).
if(AMPSIM_X86_SIMD STREQUAL "AVX")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        add_compile_options(-Xarch_x86_64 -mavx)
    elseif(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_compile_options(/arch:AVX)
    endif()
endif()
if(MSVC)
    add_compile_options(/bigobj /utf-8) # Eigen-heavy files exceed the default section count
    # The C++ runtime linked in statically, so friends don't need the Visual C++ Redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
if(MSVC)
    set(AMPSIM_NAM_FLAGS /O2 /w)
else()
    set(AMPSIM_NAM_FLAGS -O3 -w)
endif()
target_compile_options(nam_core PRIVATE ${AMPSIM_NAM_FLAGS})

# Each NAM architecture (WaveNet, LSTM, ...) registers itself from a static object in its own .cpp
# file, and nothing else references those objects. A normal static-library link drops them, which
# leaves get_dsp() with "No config parser registered for architecture: WaveNet". WHOLE_ARCHIVE
# (-force_load on macOS) keeps every object.
set(NAM_CORE_WHOLE "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")

# ---------------------------------------------------------------------------------------------
# demucs.cpp (MIT; sevagh/demucs.cpp, pinned): Demucs v4 inference in C++ on Eigen, for tone match's
# guitar separation (docs/TONE_MATCH.md, Stage C). Built from its src/ only, against NAM core's Eigen (its
# own vendored Eigen, googletest, and libnyquist are never used), single-threaded per call: our separator
# runs segments on its own threads. Always optimized, like NAM, or a minute of audio takes far too long.
set(DEMUCS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/demucs.cpp)
file(GLOB DEMUCS_SOURCES CONFIGURE_DEPENDS ${DEMUCS_DIR}/src/*.cpp)
add_library(demucs_cpp STATIC ${DEMUCS_SOURCES})
target_include_directories(demucs_cpp SYSTEM PUBLIC ${DEMUCS_DIR}/src ${NAM_DIR}/Dependencies/eigen)
target_compile_features(demucs_cpp PUBLIC cxx_std_17)
target_compile_options(demucs_cpp PRIVATE ${AMPSIM_NAM_FLAGS})
target_compile_definitions(demucs_cpp PRIVATE NDEBUG)
if(APPLE)
    # Matrix products through Accelerate's BLAS (Apple's, multithreaded) instead of Eigen's own: 
    # measured below in the separation test. Eigen is renamed for demucs.cpp (namespace EigenDemucs) so
    # its BLAS-backed templates can never be confused with the plain ones NAM core is compiled with.
    target_compile_definitions(demucs_cpp PUBLIC Eigen=EigenDemucs PRIVATE EIGEN_USE_BLAS)
    target_link_libraries(demucs_cpp PUBLIC "-framework Accelerate")
endif()

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
    src/dsp/GainSet.cpp
    src/dsp/CabIR.cpp
    src/dsp/CutFilter.cpp
    src/dsp/Equalizer.cpp
    src/dsp/Compressor.cpp
    src/dsp/ModulatedDelay.cpp
    src/dsp/Chorus.cpp
    src/dsp/Gate.cpp
    src/dsp/LinkedGates.cpp
    src/dsp/Tempo.cpp
    src/dsp/Delay.cpp
    src/dsp/Reverb.cpp
    src/dsp/PitchDetector.cpp
    src/dsp/TunerAnalysis.cpp
    src/dsp/TunerThread.cpp
    src/dsp/Scales.cpp
    src/dsp/NoteTracker.cpp
    src/dsp/Harmonizer.cpp
    src/dsp/Oversampler.cpp
    src/dsp/DriveCircuits.cpp
    src/dsp/DriveEngine.cpp
    src/dsp/Overdrive.cpp
    src/dsp/Boost.cpp
    src/dsp/Bitcrush.cpp
    src/dsp/Phaser.cpp
    src/dsp/Flanger.cpp
    src/dsp/Bloom.cpp
    src/dsp/PitchShifter.cpp
    src/dsp/Psola.cpp
    src/dsp/Multivoicer.cpp
    src/dsp/OutputLimiter.cpp
    src/dsp/PreviewPlayer.cpp)

# Tone match (docs/TONE_MATCH.md): the analysis, the search, and reading audio files. Offline, on worker
# threads; never on the audio thread.
set(AMPSIM_TONEMATCH_SOURCES
    src/tonematch/ToneMatchAnalysis.cpp
    src/tonematch/ToneMatcher.cpp
    src/tonematch/AudioFileInput.cpp
    src/tonematch/GuitarSeparator.cpp)

# The GUI (Phase 11; the design is docs/ui/amp-ui-handoff/): the tokens and fonts, the LookAndFeel, the
# components, and the pages.
set(AMPSIM_UI_SOURCES
    src/ui/Fonts.cpp
    src/ui/LookAndFeel.cpp
    src/ui/Controls.cpp
    src/ui/Meters.cpp
    src/ui/Analyzer.cpp
    src/ui/Blocks.cpp
    src/ui/ChainNav.cpp
    src/ui/SectionTabs.cpp
    src/ui/ScenesBar.cpp
    src/ui/TopBar.cpp
    src/ui/TunerPage.cpp
    src/ui/AmpHead.cpp
    src/ui/AmpView.cpp
    src/ui/MainPages.cpp
    src/ui/Pages.cpp
    src/ui/CabView.cpp
    src/ui/EqPage.cpp
    src/ui/FxPages.cpp
    src/ui/ToneMatchPage.cpp)

set(AMPSIM_JUCE_DEFINITIONS
    # Text antialiased without macOS's font smoothing (which thickens glyphs), as the UI handoff's
    # reference renders it (its CSS: -webkit-font-smoothing: antialiased). Ignored on other platforms.
    JUCE_DISABLE_COREGRAPHICS_FONT_SMOOTHING=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    AMPSIM_VERSION_STRING="${PROJECT_VERSION}"
    AMPSIM_SOURCE_URL_STRING="${AMPSIM_SOURCE_URL}")
if(WIN32 AND AMPSIM_WITH_ASIO)
    # ASIO, from the SDK headers JUCE bundles (Steinberg's ASIO licence or GPLv3: docs/RELEASING.md).
    list(APPEND AMPSIM_JUCE_DEFINITIONS JUCE_ASIO=1)
endif()

# Where the app finds out about itself (name, version, source, bundled files, the user's folder) and the
# one-time copy of the user's files from the old name: compiled into the app and the tests.
set(AMPSIM_PLATFORM_SOURCES src/platform/AppInfo.cpp src/platform/AppSettings.cpp src/platform/DataMigration.cpp)

# ---------------------------------------------------------------------------------------------
# The auto-updater (docs/RELEASING.md). Off: a no-op. On: Sparkle 2 on macOS, WinSparkle on Windows,
# both checking Ed25519 signatures against AMPSIM_ED_PUBLIC_KEY.
set(AMPSIM_UPDATER_SOURCE src/platform/Updater_none.cpp)
set(AMPSIM_PLIST_ARGS "")
if(AMPSIM_UPDATER)
    if(AMPSIM_ED_PUBLIC_KEY STREQUAL "")
        message(FATAL_ERROR "AMPSIM_UPDATER needs the Ed25519 public key: set ED_PUBLIC_KEY in tools/release/release.conf "
                            "(docs/RELEASING.md, one-time setup) or pass -DAMPSIM_ED_PUBLIC_KEY=...")
    endif()
    if(APPLE)
        set(AMPSIM_SPARKLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build-deps/Sparkle-${AMPSIM_SPARKLE_VERSION}" CACHE PATH "Sparkle's release folder")
        if(NOT EXISTS "${AMPSIM_SPARKLE_DIR}/Sparkle.framework")
            message(FATAL_ERROR "Sparkle ${AMPSIM_SPARKLE_VERSION} not found in ${AMPSIM_SPARKLE_DIR}: run tools/fetch_deps.sh")
        endif()
        if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 12.0)
            message(FATAL_ERROR "Sparkle ${AMPSIM_SPARKLE_VERSION} needs macOS 12 or later: CMAKE_OSX_DEPLOYMENT_TARGET is ${CMAKE_OSX_DEPLOYMENT_TARGET}")
        endif()
        set(AMPSIM_UPDATER_SOURCE src/platform/Updater_mac.mm)
        # Sparkle's settings (src/platform/Updater_mac.mm explains each). SUScheduledCheckInterval is in
        # seconds: once a day. LSMinimumSystemVersion makes Finder explain an older macOS instead of
        # failing to launch.
        set(AMPSIM_PLIST_ARGS PLIST_TO_MERGE "<plist><dict>
            <key>SUFeedURL</key><string>${AMPSIM_FEED_URL}</string>
            <key>SUPublicEDKey</key><string>${AMPSIM_ED_PUBLIC_KEY}</string>
            <key>SUEnableAutomaticChecks</key><true/>
            <key>SUAutomaticallyUpdate</key><true/>
            <key>SUScheduledCheckInterval</key><integer>86400</integer>
            <key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>
            </dict></plist>")
    elseif(WIN32)
        set(AMPSIM_WINSPARKLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build-deps/WinSparkle-${AMPSIM_WINSPARKLE_VERSION}" CACHE PATH "WinSparkle's release folder")
        if(NOT EXISTS "${AMPSIM_WINSPARKLE_DIR}/include/winsparkle.h")
            message(FATAL_ERROR "WinSparkle ${AMPSIM_WINSPARKLE_VERSION} not found in ${AMPSIM_WINSPARKLE_DIR} (the CI workflow downloads it)")
        endif()
        set(AMPSIM_UPDATER_SOURCE src/platform/Updater_win.cpp)
    else()
        message(WARNING "AMPSIM_UPDATER: no updater on this platform; building without one")
    endif()
endif()

# ---------------------------------------------------------------------------------------------
# The app. FORMATS Standalone builds a macOS .app; adding VST3 or AU later is a one-word change.
juce_add_plugin(BellyDSP
    COMPANY_NAME "Sean Snaider"
    PRODUCT_NAME "BellyDSP"
    BUNDLE_ID com.seansnaider.bellydsp
    PLUGIN_MANUFACTURER_CODE Ssna
    PLUGIN_CODE Bdsp
    FORMATS Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE          # for the footswitch later
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "BellyDSP needs audio input to hear your guitar."
    ${AMPSIM_PLIST_ARGS})

target_sources(BellyDSP PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    ${AMPSIM_PLATFORM_SOURCES}
    ${AMPSIM_UPDATER_SOURCE}
    src/platform/SampleRateGuard_standalone.cpp
    ${AMPSIM_UI_SOURCES}
    src/BlockParameters.cpp
    src/Presets.cpp
    src/MidiMap.cpp
    src/Scenes.cpp
    src/ToneMatchSession.cpp
    ${AMPSIM_TONEMATCH_SOURCES}
    ${AMPSIM_DSP_SOURCES})
# The factory presets (BUILD_PLAN "Presets and scenes": the five style presets), embedded as BinaryData.
juce_add_binary_data(AmpSimFactoryPresets SOURCES
    presets/factory/ModernProg.json
    presets/factory/MathRock.json
    presets/factory/TechDeath.json
    presets/factory/Metal.json
    presets/factory/MidwestEmo.json)

# The GUI's fonts (SIL Open Font License; the licences sit next to the files): Geist 300 to 600 for
# everything, Fraunces SemiBold Italic for one amp's badge. Their own namespace, so they don't collide
# with the presets' BinaryData.
juce_add_binary_data(AmpSimFonts HEADER_NAME "FontData.h" NAMESPACE FontData SOURCES
    resources/fonts/Geist/Geist-Light.ttf
    resources/fonts/Geist/Geist-Regular.ttf
    resources/fonts/Geist/Geist-Medium.ttf
    resources/fonts/Geist/Geist-SemiBold.ttf
    resources/fonts/Fraunces/Fraunces-SemiBoldItalic.ttf)

target_include_directories(BellyDSP PRIVATE src)
target_compile_definitions(BellyDSP PUBLIC ${AMPSIM_JUCE_DEFINITIONS})
target_link_libraries(BellyDSP
    PRIVATE
        AmpSimFactoryPresets
        AmpSimFonts
        ${NAM_CORE_WHOLE}
        demucs_cpp
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------------
# What ships inside the app besides the code: BellyDSP's licence (LICENSE), the licence notices (generated
# from the actual licence files; tools/notices/make_notices.cmake), the bundled content (content/), and
# the updater's framework.
set(AMPSIM_NOTICES "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.txt")
if(WIN32 AND AMPSIM_WITH_ASIO)
    set(_asio ON)
else()
    set(_asio OFF)
endif()
if(APPLE)
    set(_platform macOS)
elseif(WIN32)
    set(_platform Windows)
else()
    set(_platform ${CMAKE_SYSTEM_NAME})
endif()
set(_notices_args
    -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${AMPSIM_NOTICES} -DVERSION=${PROJECT_VERSION}
    -DPLATFORM=${_platform} -DWITH_ASIO=${_asio}
    -DJUCE_LICENCE=${AMPSIM_JUCE_LICENCE} -DSOURCE_URL=${AMPSIM_SOURCE_URL})
if(AMPSIM_UPDATER AND APPLE)
    list(APPEND _notices_args -DSPARKLE_DIR=${AMPSIM_SPARKLE_DIR})
elseif(AMPSIM_UPDATER AND WIN32)
    list(APPEND _notices_args -DWINSPARKLE_DIR=${AMPSIM_WINSPARKLE_DIR})
endif()
file(GLOB_RECURSE AMPSIM_CONTENT_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/content/*")
# Always runs (it's cheap), and only rewrites the file when the text changes.
add_custom_target(AmpSimNotices
    COMMAND ${CMAKE_COMMAND} ${_notices_args} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/notices/make_notices.cmake
    BYPRODUCTS ${AMPSIM_NOTICES}
    COMMENT "Generating THIRD_PARTY_NOTICES.txt"
    VERBATIM)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
    set(_copy_dir copy_directory_if_different)
else()
    set(_copy_dir copy_directory)
endif()
if(APPLE)
    set(AMPSIM_RESOURCES_DIR "$<TARGET_BUNDLE_CONTENT_DIR:BellyDSP_Standalone>/Resources")
else()
    set(AMPSIM_RESOURCES_DIR "$<TARGET_FILE_DIR:BellyDSP_Standalone>")
endif()
add_dependencies(BellyDSP_Standalone AmpSimNotices)
add_custom_command(TARGET BellyDSP_Standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AMPSIM_NOTICES} "${AMPSIM_RESOURCES_DIR}/THIRD_PARTY_NOTICES.txt"
    # BellyDSP's own licence (the AGPLv3 text), next to the notices, so every copy carries it.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE "${AMPSIM_RESOURCES_DIR}/LICENSE.txt"
    # Emptied first, so a bundled file that was renamed or removed doesn't linger in a dev build.
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${AMPSIM_RESOURCES_DIR}/content"
    COMMAND ${CMAKE_COMMAND} -E ${_copy_dir} ${CMAKE_CURRENT_SOURCE_DIR}/content "${AMPSIM_RESOURCES_DIR}/content"
    COMMENT "Copying the notices and the bundled content into the app"
    VERBATIM)

if(AMPSIM_UPDATER AND APPLE)
    # Sparkle.framework goes in Contents/Frameworks; the executable finds it through an rpath. Its
    # XPCServices (Installer.xpc, Downloader.xpc) are only for sandboxed apps, and BellyDSP isn't one, so
    # they're left out. After that the framework's own (ad-hoc) seal no longer matches, so the bundle is
    # re-signed ad hoc here; the release script signs it properly afterwards (tools/release/sign_app.sh).
    target_compile_options(BellyDSP PRIVATE "-F${AMPSIM_SPARKLE_DIR}")
    target_link_libraries(BellyDSP PUBLIC "-F${AMPSIM_SPARKLE_DIR}" "-framework Sparkle")
    target_link_options(BellyDSP_Standalone PRIVATE "-Wl,-rpath,@executable_path/../Frameworks")
    set(_frameworks "$<TARGET_BUNDLE_CONTENT_DIR:BellyDSP_Standalone>/Frameworks")
    add_custom_command(TARGET BellyDSP_Standalone POST_BUILD
        COMMAND rm -rf "${_frameworks}/Sparkle.framework"
        COMMAND mkdir -p "${_frameworks}"
        COMMAND ditto "${AMPSIM_SPARKLE_DIR}/Sparkle.framework" "${_frameworks}/Sparkle.framework"
        COMMAND rm -rf "${_frameworks}/Sparkle.framework/Versions/B/XPCServices" "${_frameworks}/Sparkle.framework/XPCServices"
        COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:BellyDSP_Standalone>"
        COMMENT "Embedding Sparkle.framework (without its sandbox-only XPC services)"
        VERBATIM)
elseif(AMPSIM_UPDATER AND WIN32)
    target_include_directories(BellyDSP PRIVATE "${AMPSIM_WINSPARKLE_DIR}/include")
    target_link_libraries(BellyDSP PUBLIC "${AMPSIM_WINSPARKLE_DIR}/x64/Release/WinSparkle.lib")
    target_compile_definitions(BellyDSP PRIVATE
        AMPSIM_FEED_URL="${AMPSIM_FEED_URL}"
        AMPSIM_ED_PUBLIC_KEY="${AMPSIM_ED_PUBLIC_KEY}")
    add_custom_command(TARGET BellyDSP_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${AMPSIM_WINSPARKLE_DIR}/x64/Release/WinSparkle.dll" "$<TARGET_FILE_DIR:BellyDSP_Standalone>"
        COMMENT "Copying WinSparkle.dll next to the app"
        VERBATIM)
endif()

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
# Tone match's guitar separation from the command line (docs/TONE_MATCH.md): any audio file in, the guitar
# stem out, with timing; installs the model on first use like the app.
juce_add_console_app(ampsim_separate PRODUCT_NAME "ampsim_separate")
target_sources(ampsim_separate PRIVATE tools/separate/Main.cpp src/tonematch/GuitarSeparator.cpp src/tonematch/AudioFileInput.cpp
    src/platform/AppInfo.cpp)
target_include_directories(ampsim_separate PRIVATE src)
target_compile_definitions(ampsim_separate PRIVATE ${AMPSIM_JUCE_DEFINITIONS})
target_link_libraries(ampsim_separate
    PRIVATE
        demucs_cpp
        juce::juce_audio_formats
        juce::juce_cryptography
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
target_compile_options(nam_render PRIVATE ${AMPSIM_NAM_FLAGS})

# ---------------------------------------------------------------------------------------------
# Tests: run with `ctest --test-dir build --output-on-failure`, or run build/.../ampsim_tests
# directly to see every measurement. Proof artifacts (renders, editor snapshots, summary.txt)
# land in build/proof.
enable_testing()

juce_add_console_app(ampsim_tests PRODUCT_NAME "ampsim_tests")
target_sources(ampsim_tests PRIVATE
    tests/TestMain.cpp
    tests/TestHelpers.cpp
    $<IF:$<BOOL:${APPLE}>,tests/AllocationTracking.cpp,tests/AllocationTracking_portable.cpp>
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
    tests/TunerTests.cpp
    tests/HarmonyTests.cpp
    tests/HarmonizerTests.cpp
    tests/FullRigTests.cpp
    tests/GateTests.cpp
    tests/OversamplerTests.cpp
    tests/DriveTests.cpp
    tests/BloomTests.cpp
    tests/PitchShifterTests.cpp
    tests/MultivoicerTests.cpp
    tests/Plot.cpp
    tests/NamAmpTests.cpp
    tests/AmpGainTests.cpp
    tests/ChainTests.cpp
    tests/ProcessorTests.cpp
    tests/BuiltInCaptureTests.cpp
    tests/GainStagingTests.cpp
    tests/OutputLimiterTests.cpp
    tests/GateInAppTests.cpp
    tests/PlayTestUiTests.cpp
    tests/RealtimeSafetyTests.cpp
    tests/HandoffEngineTests.cpp
    tests/EditorTests.cpp
    tests/ReleaseTests.cpp
    tests/MigrationTests.cpp
    tests/CaptureTests.cpp
    tests/ToneMatchTests.cpp
    tests/ToneMatchAppTests.cpp
    tests/ToneMatchSeparationTests.cpp
    tests/ToneMatchCompareTests.cpp
    tests/AudioDeviceTests.cpp
    tools/capture/CaptureEngine.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    ${AMPSIM_PLATFORM_SOURCES}
    src/platform/Updater_none.cpp
    src/platform/SampleRateGuard_none.cpp
    ${AMPSIM_UI_SOURCES}
    src/BlockParameters.cpp
    src/Presets.cpp
    src/MidiMap.cpp
    src/Scenes.cpp
    src/ToneMatchSession.cpp
    ${AMPSIM_TONEMATCH_SOURCES}
    ${AMPSIM_DSP_SOURCES})
target_include_directories(ampsim_tests PRIVATE src tests tools/capture)
target_compile_definitions(ampsim_tests PRIVATE
    ${AMPSIM_JUCE_DEFINITIONS}
    # Tests only: MessageManager::runDispatchLoopUntil, so an editor test can let a dialog's clicks
    # and callbacks arrive as the app's message loop delivers them. Our code never runs modal loops.
    JUCE_MODAL_LOOPS_PERMITTED=1
    AMPSIM_NAM_DIR="${NAM_DIR}"
    AMPSIM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    AMPSIM_NAM_RENDER="$<TARGET_FILE:nam_render>"
    # The built app's own content folder, for the test that bundled IRs resolve inside a real build.
    AMPSIM_APP_CONTENT_DIR="${AMPSIM_RESOURCES_DIR}/content")
target_link_libraries(ampsim_tests
    PRIVATE
        AmpSimFactoryPresets
        AmpSimFonts
        ${NAM_CORE_WHOLE}
        demucs_cpp
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
if(MSVC)
    # The tests run on the main thread, which Windows gives a 1 MB stack (macOS: 8 MB). Unlike clang, MSVC
    # gives most locals in sibling { } scopes their own slot, so a runTest() with dozens of AmpSimProcessor
    # locals (152 KB each) asks for one huge frame: ProcessorTests' is 9.2 MB, and several others are near
    # 1 MB, depending on inlining. 64 MB is address space only; pages are committed as they're touched.
    target_link_options(ampsim_tests PRIVATE /STACK:67108864)
endif()
add_dependencies(ampsim_tests nam_render AmpSimNotices)
# The notices and the bundled content sit next to the test binary too, so the tests read the same
# generated file and the same content folder (platform::factoryContentFolder()) the app ships.
add_custom_command(TARGET ampsim_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AMPSIM_NOTICES} "$<TARGET_FILE_DIR:ampsim_tests>/THIRD_PARTY_NOTICES.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE "$<TARGET_FILE_DIR:ampsim_tests>/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:ampsim_tests>/content"
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/content "$<TARGET_FILE_DIR:ampsim_tests>/content"
    VERBATIM)

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

# ---------------------------------------------------------------------------------------------
# Capturing your own gear for NAM (docs/CAPTURING.md): plays NAM's input file out of the interface and
# records the gear's return in the same callback. --simulate runs it without hardware.
juce_add_console_app(ampsim_capture PRODUCT_NAME "ampsim_capture")
target_sources(ampsim_capture PRIVATE tools/capture/Main.cpp tools/capture/CaptureEngine.cpp ${AMPSIM_DSP_SOURCES})
target_include_directories(ampsim_capture PRIVATE src tools/capture)
target_compile_definitions(ampsim_capture PRIVATE ${AMPSIM_JUCE_DEFINITIONS})
target_link_libraries(ampsim_capture
    PRIVATE
        ${NAM_CORE_WHOLE}
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
