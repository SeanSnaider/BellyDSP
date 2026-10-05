// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// ampsim_tests: every test in tests/, run with JUCE's UnitTest framework.
//
//   ampsim_tests [--proof-dir <dir>] [--only <test name substring>]
//
// Measurements are logged on lines starting with "->". They're collected into <proof-dir>/summary.txt
// along with the rendered WAVs and editor snapshots the tests write there.

#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "platform/AppSettings.h"

#include <juce_events/juce_events.h>

#include <iostream>

namespace
{
class Runner final : public juce::UnitTestRunner
{
public:
    juce::StringArray measurements;

    void logMessage (const juce::String& message) override
    {
        std::cout << message << std::endl;

        if (message.trimStart().startsWith ("->"))
            measurements.add (currentTest + ": " + message.trimStart().substring (2).trim());
        else if (message.startsWith ("Starting tests in: "))
            currentTest = message.fromFirstOccurrenceOf ("Starting tests in: ", false, false).upToLastOccurrenceOf ("...", false, false);
    }

private:
    juce::String currentTest;
};
} // namespace

int main (int argc, char* argv[])
{
    // Starts JUCE's message manager, which the editor snapshot test needs.
    juce::ScopedJuceInitialiser_GUI juce;

    juce::String only;

    for (int i = 1; i + 1 < argc; ++i)
    {
        const juce::String arg (argv[i]);

        if (arg == "--proof-dir")
            testing::proofDir() = juce::File::getCurrentWorkingDirectory().getChildFile (argv[++i]);
        else if (arg == "--only")
            only = argv[++i];
    }

    testing::proofDir().createDirectory();

    // The tests were written against processors that start with empty amp slots; the ones about the
    // built-in captures turn them back on (testing::WithBuiltInCaptures).
    AmpSimProcessor::builtInCapturesForFreshSlots = false;

    // The app's own settings (the gate's first switch-on, ...) go to a fresh file of the tests' own, never the user's.
    const auto settingsFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ampsim_tests_settings.json");
    settingsFile.deleteFile();
    platform::settings::setFileForTests (settingsFile);

    juce::Array<juce::UnitTest*> tests;
    for (auto* test : juce::UnitTest::getAllTests())
        if (only.isEmpty() || test->getName().containsIgnoreCase (only))
            tests.add (test);

    Runner runner;
    runner.setAssertOnFailure (false);
    runner.runTests (tests);

    int passes = 0, failures = 0;
    std::cout << "\n================ Summary ================\n";

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult (i);
        passes += result->passes;
        failures += result->failures;
        std::cout << (result->failures == 0 ? "PASS  " : "FAIL  ") << result->unitTestName << " / "
                  << result->subcategoryName << "  (" << result->passes << " checks)\n";
    }

    std::cout << "\n" << passes << " checks passed, " << failures << " failed\n";

    const auto summary = testing::proofDir().getChildFile ("summary.txt");
    summary.replaceWithText (runner.measurements.joinIntoString ("\n") + "\n\n" + juce::String (passes)
                             + " checks passed, " + juce::String (failures) + " failed\n");
    std::cout << "Measurements written to " << summary.getFullPathName() << "\n";

    return failures == 0 ? 0 : 1;
}
