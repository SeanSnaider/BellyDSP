# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

# Generates THIRD_PARTY_NOTICES.txt: BellyDSP's own licence notice first, then every third-party
# component and every bundled capture or IR that ships inside it, each with its licence text, read from
# the actual licence files in LICENSE, the submodules, the downloaded frameworks, and content/. Runs in
# CMake's script mode at build time (CMakeLists.txt, target AmpSimNotices), so it needs nothing but CMake:
#
#   cmake -DSRC=<repo> -DOUT=<file> -DVERSION=0.1.0 -DPLATFORM=macOS [-DSPARKLE_DIR=...] [-DWINSPARKLE_DIR=...]
#         [-DWITH_ASIO=ON] [-DJUCE_LICENCE=undecided|AGPLv3|JUCE] [-DSOURCE_URL=...] -P make_notices.cmake
#
# It fails the build if content/ holds a file that content/manifest.json doesn't list with a licence:
# nothing gets bundled without a licence entry.

cmake_minimum_required(VERSION 3.22)

foreach(required SRC OUT VERSION PLATFORM)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "make_notices.cmake: -D${required}=... is required")
    endif()
endforeach()
if(NOT DEFINED JUCE_LICENCE OR JUCE_LICENCE STREQUAL "")
    set(JUCE_LICENCE "undecided")
endif()

set(JUCE_DIR "${SRC}/third_party/JUCE")
set(NAM_DIR "${SRC}/third_party/NeuralAmpModelerCore")
set(rule "--------------------------------------------------------------------------------")
set(text "")
set(index "")
set(count 0)

# Appends one component: a heading, what it is and how it's used, then the licence texts.
function(component name summary)
    math(EXPR n "${count} + 1")
    set(count ${n} PARENT_SCOPE)
    set(body "\n${rule}\n${n}. ${name}\n${rule}\n${summary}\n")
    foreach(file IN LISTS ARGN)
        if(file STREQUAL "")
            continue()
        endif()
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "make_notices.cmake: licence file not found: ${file}")
        endif()
        file(READ "${file}" licence)
        get_filename_component(shown "${file}" NAME)
        string(APPEND body "\n[${shown}]\n${licence}\n")
    endforeach()
    set(text "${text}${body}" PARENT_SCOPE)
    set(index "${index}  ${n}. ${name}\n" PARENT_SCOPE)
endfunction()

# Appends a component whose licence text is given inline (when there's no file to read).
function(component_text name summary licence)
    math(EXPR n "${count} + 1")
    set(count ${n} PARENT_SCOPE)
    set(text "${text}\n${rule}\n${n}. ${name}\n${rule}\n${summary}\n\n${licence}\n" PARENT_SCOPE)
    set(index "${index}  ${n}. ${name}\n" PARENT_SCOPE)
endfunction()

# The section of a text file between two markers (for licences inside longer READMEs).
function(section_of file from to outvar)
    file(READ "${file}" all)
    string(FIND "${all}" "${from}" start)
    if(start EQUAL -1)
        message(FATAL_ERROR "make_notices.cmake: '${from}' not found in ${file}")
    endif()
    string(SUBSTRING "${all}" ${start} -1 rest)
    if(NOT to STREQUAL "")
        string(FIND "${rest}" "${to}" stop)
        if(stop GREATER 0)
            string(SUBSTRING "${rest}" 0 ${stop} rest)
        endif()
    endif()
    set(${outvar} "${rest}" PARENT_SCOPE)
endfunction()

# The MIT licence's standard text, for nlohmann/json, whose single header carries only an SPDX tag.
set(mit_body "Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the \"Software\"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.")

# ---- BellyDSP itself ---------------------------------------------------------------------------
if(NOT DEFINED SOURCE_URL OR SOURCE_URL STREQUAL "")
    set(SOURCE_URL "https://github.com/SeanSnaider/BellyDSP")
endif()
set(notice "BellyDSP ${VERSION}
Copyright (C) 2026 Sean Snaider

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version (SPDX: AGPL-3.0-or-later).

This program comes with ABSOLUTELY NO WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Affero General Public License for more details.

Source code: ${SOURCE_URL}
The source of this exact version: ${SOURCE_URL}/tree/v${VERSION}")
component("BellyDSP ${VERSION} (${SOURCE_URL})"
    "The app itself, under the GNU Affero General Public License, version 3 or later. Its
full text follows (LICENSE in the source, LICENSE.txt next to this file). Copyright (C)
2026 Sean Snaider. Source code: ${SOURCE_URL}"
    "${SRC}/LICENSE")

# ---- JUCE ------------------------------------------------------------------------------------
if(JUCE_LICENCE STREQUAL "AGPLv3")
    set(juce_terms "BellyDSP uses JUCE under the GNU Affero General Public License v3. As the AGPLv3
requires, the complete source code of this version of BellyDSP is available to
everyone who receives it: ${SOURCE_URL}/tree/v${VERSION}")
elseif(JUCE_LICENCE STREQUAL "JUCE")
    set(juce_terms "BellyDSP uses JUCE under the JUCE 8 licence (https://juce.com/legal/juce-8-licence/).")
else()
    set(juce_terms "JUCE is dual-licensed under the AGPLv3 and the JUCE 8 licence. (Which of the two
applies to this build hasn't been decided yet: see docs/RELEASING.md, \"Licences\".)")
endif()
component("JUCE 8.0.15 (https://juce.com)"
    "The application framework: audio devices, the window and controls, file formats, and DSP helpers.
${juce_terms}"
    "${JUCE_DIR}/LICENSE.md")

component("zlib, inside JUCE (https://zlib.net)" "Compression, in juce_core.")
section_of("${JUCE_DIR}/modules/juce_core/zip/zlib/README" "Copyright notice:" "" zlib_notice)
string(APPEND text "\n${zlib_notice}\n")
component("libpng, inside JUCE (http://www.libpng.org)" "PNG images, in juce_graphics."
    "${JUCE_DIR}/modules/juce_graphics/image_formats/pnglib/LICENSE")
section_of("${JUCE_DIR}/modules/juce_graphics/image_formats/jpglib/README" "LEGAL ISSUES" "REFERENCES" jpeg_notice)
component_text("The Independent JPEG Group's JPEG library, inside JUCE" "JPEG images, in juce_graphics.
This software is based in part on the work of the Independent JPEG Group." "${jpeg_notice}")
component("FLAC, inside JUCE (https://xiph.org/flac)" "FLAC audio files, in juce_audio_formats."
    "${JUCE_DIR}/modules/juce_audio_formats/codecs/flac/Flac Licence.txt")
component("Ogg Vorbis, inside JUCE (https://xiph.org/vorbis)" "Ogg Vorbis audio files, in juce_audio_formats."
    "${JUCE_DIR}/modules/juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt")
component("HarfBuzz, inside JUCE (https://harfbuzz.github.io)" "Text shaping, in juce_graphics."
    "${JUCE_DIR}/modules/juce_graphics/fonts/harfbuzz/COPYING")
component("SheenBidi, inside JUCE (https://github.com/Tehreer/SheenBidi)" "Bidirectional text, in juce_graphics."
    "${JUCE_DIR}/modules/juce_graphics/unicode/sheenbidi/LICENSE")
if(WITH_ASIO)
    component("Steinberg ASIO SDK headers, inside JUCE" "ASIO audio drivers on Windows (built with AMPSIM_WITH_ASIO).
ASIO is a trademark and software of Steinberg Media Technologies GmbH."
        "${JUCE_DIR}/modules/juce_audio_devices/native/asio/LICENSE.txt")
endif()

# ---- NAM ---------------------------------------------------------------------------------------
component("NeuralAmpModelerCore v0.6.0 (https://github.com/sdatkinson/NeuralAmpModelerCore)"
    "The neural amp model (NAM) engine that runs the captures." "${NAM_DIR}/LICENSE")
component("Eigen (https://eigen.tuxfamily.org)"
    "Linear algebra used by NeuralAmpModelerCore. Mozilla Public License 2.0: Eigen's source is
available at https://gitlab.com/libeigen/eigen, unmodified."
    "${NAM_DIR}/Dependencies/eigen/COPYING.README"
    "${NAM_DIR}/Dependencies/eigen/COPYING.MPL2")
component_text("JSON for Modern C++ 3.12.0 (https://github.com/nlohmann/json)"
    "Reads .nam files, in NeuralAmpModelerCore."
    "MIT License\n\nCopyright (c) 2013-2025 Niels Lohmann\n\n${mit_body}")

# ---- Tone match's guitar separation (docs/TONE_MATCH.md) ----------------------------------------
component("demucs.cpp (https://github.com/sevagh/demucs.cpp)"
    "Runs Demucs to separate the guitar out of a song for tone match."
    "${SRC}/third_party/demucs.cpp/LICENSE")
component_text("Demucs (https://github.com/adefossez/demucs)"
    "The source separation model demucs.cpp implements (Hybrid Transformer Demucs, htdemucs_6s). Its
weights are not part of BellyDSP: the app downloads them from the author's page the first time
separation is used."
    "MIT License\n\nCopyright (c) Meta Platforms, Inc. and affiliates.\n\n${mit_body}")

# ---- Updaters ----------------------------------------------------------------------------------
if(DEFINED SPARKLE_DIR AND NOT SPARKLE_DIR STREQUAL "")
    component("Sparkle 2 (https://sparkle-project.org)" "Automatic updates on macOS." "${SPARKLE_DIR}/LICENSE")
endif()
if(DEFINED WINSPARKLE_DIR AND NOT WINSPARKLE_DIR STREQUAL "")
    component("WinSparkle (https://winsparkle.org)" "Automatic updates on Windows."
        "${WINSPARKLE_DIR}/COPYING")
endif()

# ---- Fonts -------------------------------------------------------------------------------------
component("Geist (https://github.com/vercel/geist-font)" "The interface font." "${SRC}/resources/fonts/Geist/OFL.txt")
component("Fraunces (https://github.com/undercasetype/Fraunces)" "One amp's badge (an instance of Fraunces Italic)."
    "${SRC}/resources/fonts/Fraunces/OFL.txt")

# ---- Bundled content (content/manifest.json) -----------------------------------------------------
set(content_dir "${SRC}/content")
set(manifest "${content_dir}/manifest.json")
set(content_text "")
set(listed "")
if(EXISTS "${manifest}")
    file(READ "${manifest}" json)
    string(JSON n_files ERROR_VARIABLE json_error LENGTH "${json}" files)
    if(json_error)
        message(FATAL_ERROR "content/manifest.json: ${json_error}")
    endif()
    set(licence_files "")
    # Optional "credits": thank-you notes printed once above the files (even where a licence such as CC0
    # asks for no credit).
    string(JSON n_credits ERROR_VARIABLE no_credits LENGTH "${json}" credits)
    if(NOT no_credits AND n_credits GREATER 0)
        math(EXPR last_credit "${n_credits} - 1")
        foreach(i RANGE ${last_credit})
            string(JSON credit GET "${json}" credits ${i})
            string(APPEND content_text "\n  ${credit}\n")
        endforeach()
    endif()
    if(n_files GREATER 0)
        math(EXPR last "${n_files} - 1")
        foreach(i RANGE ${last})
            foreach(key path title author source license license_file)
                string(JSON ${key} ERROR_VARIABLE missing GET "${json}" files ${i} ${key})
                if(missing OR "${${key}}" STREQUAL "")
                    message(FATAL_ERROR "content/manifest.json: entry ${i} has no \"${key}\" (every bundled file needs a licence entry)")
                endif()
            endforeach()
            string(JSON notes ERROR_VARIABLE no_notes GET "${json}" files ${i} notes)
            if(no_notes)
                set(notes "")
            endif()
            if(NOT EXISTS "${content_dir}/${path}")
                message(FATAL_ERROR "content/manifest.json lists ${path}, which isn't in content/")
            endif()
            if(NOT EXISTS "${content_dir}/${license_file}")
                message(FATAL_ERROR "content/manifest.json: ${path}'s licence file ${license_file} isn't in content/")
            endif()
            list(APPEND listed "${path}")
            list(APPEND licence_files "${license_file}")
            string(APPEND content_text "\n  ${path}\n    \"${title}\" by ${author}\n    Source: ${source}\n    Licence: ${license} (text below: ${license_file})\n")
            if(NOT notes STREQUAL "")
                string(APPEND content_text "    Notes: ${notes}\n")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES licence_files)
        foreach(f IN LISTS licence_files)
            file(READ "${content_dir}/${f}" licence)
            string(APPEND content_text "\n[${f}]\n${licence}\n")
        endforeach()
    endif()
endif()

# Every file in content/ (except the README, the manifest, and the licence texts) must be listed.
file(GLOB_RECURSE content_files RELATIVE "${content_dir}" "${content_dir}/*")
foreach(f IN LISTS content_files)
    if(f STREQUAL "README.md" OR f STREQUAL "manifest.json" OR f MATCHES "^licenses/" OR f MATCHES "(^|/)\\.DS_Store$")
        continue()
    endif()
    list(FIND listed "${f}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "content/${f} has no entry in content/manifest.json. Every bundled capture or IR needs "
                            "its source, author, and licence there (see content/README.md).")
    endif()
endforeach()

if(content_text STREQUAL "")
    component_text("Bundled captures and impulse responses" "Files in the app's content folder." "None in this version.")
else()
    component_text("Bundled captures and impulse responses" "Files in the app's content folder, each under its own licence." "${content_text}")
endif()

# ---- Write -------------------------------------------------------------------------------------
set(header "BellyDSP ${VERSION} (${PLATFORM}): licence and third-party notices
${rule}

${notice}

BellyDSP is made by Sean Snaider. It includes the software and content below, each
under its own licence, reproduced in full. Thanks to everyone who made them.

Contents:
${index}")
file(WRITE "${OUT}.tmp" "${header}${text}")
# Only touch the real file when it changes, so the app isn't re-copied on every build.
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
