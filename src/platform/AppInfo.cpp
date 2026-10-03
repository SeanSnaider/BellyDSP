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

juce::String sourceUrl()
{
   #ifdef AMPSIM_SOURCE_URL_STRING
    if (const juce::String url (AMPSIM_SOURCE_URL_STRING); url.isNotEmpty())
        return url.trimCharactersAtEnd ("/");
   #endif
    return "https://github.com/SeanSnaider/BellyDSP";
}

juce::String sourceUrlForThisVersion()
{
    return sourceUrl() + "/tree/v" + appVersion();
}

juce::File userDataFolder()
{
    // userApplicationDataDirectory is ~/Library on macOS and %APPDATA% (Roaming) on Windows.
    const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    return base.getChildFile ("Application Support").getChildFile (productName);
   #else
    return base.getChildFile (productName);
   #endif
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
