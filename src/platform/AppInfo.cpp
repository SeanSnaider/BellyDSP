// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "AppInfo.h"

#ifndef AMPSIM_VERSION_STRING
 #error "CMakeLists.txt defines AMPSIM_VERSION_STRING for every target"
#endif

namespace platform
{
juce::String appVersion()
{
    return AMPSIM_VERSION_STRING;
}

juce::File resourcesFolder()
{
    const auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
   #if JUCE_MAC
    // Inside a bundle, currentApplicationFile is the .app itself; a console tool is just its executable.
    if (app.isDirectory() && app.getChildFile ("Contents/Resources").isDirectory())
        return app.getChildFile ("Contents/Resources");
   #endif
    return app.getParentDirectory();
}

juce::File noticesFile()
{
    return resourcesFolder().getChildFile ("THIRD_PARTY_NOTICES.txt");
}

juce::File factoryContentFolder()
{
    return resourcesFolder().getChildFile ("content");
}
} // namespace platform
