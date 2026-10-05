// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// ampsim_tests: every test in tests/, run with JUCE's UnitTest framework.
//
//   ampsim_tests [--proof-dir <dir>] [--only <test name substring>] [--skip <substring>]...
//
// Measurements are logged on lines starting with "->". They're collected into <proof-dir>/summary.txt
// along with the rendered WAVs and editor snapshots the tests write there.
//
// --skip (repeatable) and the environment variable AMPSIM_SKIP_TESTS (a ';'-separated list) name tests to
// skip, by a substring of "<test> / <subtest>" (testing::skipPatterns() in TestHelpers.h says how). CI uses
// them for the few checks that measure wall-clock pacing, which a starved shared runner can't hold.
//
// Every subtest's duration is printed when it ends, and the summary lists the slowest. If the process
// crashes, the crash handler prints "CRASH in <the subtest that was running>" and a stack backtrace.

#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "platform/AppSettings.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <csignal>
#endif

namespace
{
// The subtest running now, for the crash handler. A fixed buffer, so the handler reads it without
// allocating (the heap may be what broke).
char crashTestName[1024] = "(before the first test)";

void onCrash (void* platformData)
{
    std::fflush (stdout);
    std::fputs ("\n\nCRASH in ", stderr);
    std::fputs (crashTestName, stderr);
   #if JUCE_WINDOWS
    if (const auto* pointers = static_cast<EXCEPTION_POINTERS*> (platformData); pointers != nullptr && pointers->ExceptionRecord != nullptr)
        std::fprintf (stderr, "\nException code 0x%08lX (0xC0000005 access violation, 0xC00000FD stack overflow)",
                      (unsigned long) pointers->ExceptionRecord->ExceptionCode);
   #else
    std::fprintf (stderr, "\nSignal %d", (int) (juce::pointer_sized_int) platformData);
   #endif
    std::fputs ("\nStack backtrace:\n", stderr);
    std::fflush (stderr);
    std::fputs (juce::SystemStats::getStackBacktrace().toRawUTF8(), stderr);
    std::fputs ("\n", stderr);
    std::fflush (stderr);
}

void installCrashHandler()
{
    juce::SystemStats::setApplicationCrashHandler (onCrash);

   #if JUCE_WINDOWS
    // A stack overflow leaves the crashing thread no stack for the handler: keep 128 KB in reserve for it.
    ULONG reserve = 128 * 1024;
    SetThreadStackGuarantee (&reserve);
   #else
    // The same on POSIX: JUCE's handler is installed with signal(), on the crashing stack, so a stack overflow
    // would fault again inside it. Give it an alternate stack and re-install it with SA_ONSTACK.
    static char alternateStack[256 * 1024];
    stack_t stack {};
    stack.ss_sp = alternateStack;
    stack.ss_size = sizeof (alternateStack);
    sigaltstack (&stack, nullptr);
    for (const auto sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE })
    {
        struct sigaction action {};
        if (sigaction (sig, nullptr, &action) == 0)
        {
            action.sa_flags |= SA_ONSTACK;
            sigaction (sig, &action, nullptr);
        }
    }
   #endif
}

class Runner final : public juce::UnitTestRunner
{
public:
    juce::StringArray measurements;

    struct Timing
    {
        juce::String title;
        double ms = 0.0;
    };
    std::vector<Timing> timings;

    void logMessage (const juce::String& message) override
    {
        std::cout << message << std::endl;

        if (message.trimStart().startsWith ("->"))
            measurements.add (currentTest + ": " + message.trimStart().substring (2).trim());
        else if (message.startsWith ("Starting tests in: "))
        {
            currentTest = message.fromFirstOccurrenceOf ("Starting tests in: ", false, false).upToLastOccurrenceOf ("...", false, false);
            testing::currentTestTitle() = currentTest;
            const auto name = currentTest.toRawUTF8();
            std::strncpy (crashTestName, name, sizeof (crashTestName) - 1);
            crashTestName[sizeof (crashTestName) - 1] = '\0';
            startMs = juce::Time::getMillisecondCounterHiRes();
            std::cout << "  [started at +" << juce::String ((startMs - launchMs) / 1000.0, 1) << " s]" << std::endl;
        }
        else if (startMs > 0.0 && (message.startsWith ("Completed tests in ") || message.startsWith ("FAILED!!")))
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();
            timings.push_back ({ currentTest, now - startMs });
            std::cout << "  [took " << juce::String ((now - startMs) / 1000.0, 2) << " s; ended at +" << juce::String ((now - launchMs) / 1000.0, 1)
                      << " s]" << std::endl;
            startMs = 0.0;
        }
    }

private:
    juce::String currentTest;
    const double launchMs = juce::Time::getMillisecondCounterHiRes();
    double startMs = 0.0;
};
} // namespace

int main (int argc, char* argv[])
{
    installCrashHandler();

    // Starts JUCE's message manager, which the editor snapshot test needs.
    juce::ScopedJuceInitialiser_GUI juce;

    juce::String only;
    auto& skips = testing::skipPatterns();
    skips.addTokens (juce::SystemStats::getEnvironmentVariable ("AMPSIM_SKIP_TESTS", {}), ";", "\"");

    for (int i = 1; i + 1 < argc; ++i)
    {
        const juce::String arg (argv[i]);

        if (arg == "--proof-dir")
            testing::proofDir() = juce::File::getCurrentWorkingDirectory().getChildFile (argv[++i]);
        else if (arg == "--only")
            only = argv[++i];
        else if (arg == "--skip")
            skips.add (argv[++i]);
    }

    skips.trim();
    skips.removeEmptyStrings();
    if (! skips.isEmpty())
        std::cout << "Skipping (--skip, AMPSIM_SKIP_TESTS): \"" << skips.joinIntoString ("\", \"") << "\"\n";

    testing::proofDir().createDirectory();

    // The tests were written against processors that start with empty amp slots; the ones about the
    // built-in captures turn them back on (testing::WithBuiltInCaptures).
    AmpSimProcessor::builtInCapturesForFreshSlots = false;

    // The app's own settings (the gate's first switch-on, ...) go to a fresh file of the tests' own, never the user's.
    const auto settingsFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ampsim_tests_settings.json");
    settingsFile.deleteFile();
    platform::settings::setFileForTests (settingsFile);

    juce::Array<juce::UnitTest*> tests;
    juce::StringArray skippedWhole;
    for (auto* test : juce::UnitTest::getAllTests())
    {
        if (only.isNotEmpty() && ! test->getName().containsIgnoreCase (only))
            continue;

        // A whole test is left out when a pattern matches its name; subtests ask testing::skipped().
        const auto pattern = std::find_if (skips.begin(), skips.end(), [test] (const juce::String& s) { return test->getName().containsIgnoreCase (s); });
        if (pattern != skips.end())
        {
            skippedWhole.add (test->getName());
            testing::usedSkipPatterns().addIfNotAlreadyThere (*pattern);
            continue;
        }
        tests.add (test);
    }

    Runner runner;
    runner.setAssertOnFailure (false);
    runner.runTests (tests);
    std::strncpy (crashTestName, "(after the last test: static destructors or shutdown)", sizeof (crashTestName) - 1);

    int passes = 0, failures = 0;
    std::cout << "\n================ Summary ================\n";

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult (i);
        passes += result->passes;
        failures += result->failures;
        const auto title = result->unitTestName + " / " + result->subcategoryName;
        const auto wasSkipped = testing::skippedTitles().contains (title);
        std::cout << (result->failures > 0 ? "FAIL  " : (wasSkipped ? "SKIP  " : "PASS  ")) << title << "  (" << result->passes << " checks, "
                  << juce::String ((double) (result->endTime - result->startTime).inMilliseconds() / 1000.0, 2) << " s)\n";
    }

    for (const auto& name : skippedWhole)
        std::cout << "SKIP  " << name << " (the whole test)\n";

    auto slowest = runner.timings;
    std::sort (slowest.begin(), slowest.end(), [] (const auto& a, const auto& b) { return a.ms > b.ms; });
    std::cout << "\nSlowest:\n";
    for (size_t i = 0; i < std::min<size_t> (slowest.size(), 10); ++i)
        std::cout << "  " << juce::String (slowest[i].ms / 1000.0, 2) << " s  " << slowest[i].title << "\n";

    for (const auto& pattern : skips)
        if (! testing::usedSkipPatterns().contains (pattern))
            std::cout << "\nWARNING: the skip pattern \"" << pattern << "\" matched no test that ran\n";

    std::cout << "\n" << passes << " checks passed, " << failures << " failed";
    const auto numSkipped = testing::skippedTitles().size() + skippedWhole.size();
    if (numSkipped > 0)
        std::cout << " (" << numSkipped << " skipped)";
    std::cout << "\n";

    const auto summary = testing::proofDir().getChildFile ("summary.txt");
    summary.replaceWithText (runner.measurements.joinIntoString ("\n") + "\n\n" + juce::String (passes)
                             + " checks passed, " + juce::String (failures) + " failed\n");
    std::cout << "Measurements written to " << summary.getFullPathName() << "\n";

    return failures == 0 ? 0 : 1;
}
