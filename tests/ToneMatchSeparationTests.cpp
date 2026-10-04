// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Tone match, Stage C (docs/TONE_MATCH.md): the separation model's installer (size and SHA-256 checks,
// cancel, conversion), the C++ separation against Python's Demucs on the same 10 s clip, and the session
// running a separator before the match.
//
// The weights aren't in the repo. The installer and golden tests use a local copy of the pinned upstream
// file: $AMPSIM_DEMUCS_WEIGHTS, or the Hugging Face cache that `uv run --with demucs` fills (the prototype's
// separation). With neither, they download it when AMPSIM_NETWORK_TESTS=1, and otherwise say they didn't run.

#include "BuiltInCaptures.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "ToneMatchSession.h"
#include "tonematch/GuitarSeparator.h"

namespace
{
using namespace testing;
using ampsim::tonematch::GuitarSeparator;

juce::File fixtures() { return juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/fixtures/tone_match"); }

const juce::var& expected()
{
    static const juce::var v = juce::JSON::parse (fixtures().getChildFile ("expected.json").loadFileAsString());
    return v;
}

std::vector<float> readMono (const juce::File& f)
{
    const auto b = readWav (f);
    return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
}

/// A local copy of the pinned weights, if this machine has one.
juce::File localWeights()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("AMPSIM_DEMUCS_WEIGHTS", {});
    if (env.isNotEmpty() && juce::File (env).existsAsFile())
        return juce::File (env);
    const auto cache = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                           .getChildFile (".cache/huggingface/hub/models--adefossez--HTDemucs-6s/snapshots");
    for (const auto& f : cache.findChildFiles (juce::File::findFiles, true, "5c90dfd2.safetensors"))
        if (f.getSize() == GuitarSeparator::weightsBytes)
            return f;
    return {};
}

juce::String weightsUrlForTests()
{
    if (const auto local = localWeights(); local != juce::File())
        return juce::URL (local).toString (false);
    if (juce::SystemStats::getEnvironmentVariable ("AMPSIM_NETWORK_TESTS", {}) == "1")
        return GuitarSeparator::weightsUrl;
    return {};
}

double sdrDb (const std::vector<float>& reference, const std::vector<float>& estimate)
{
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < std::min (reference.size(), estimate.size()); ++i)
    {
        num += (double) reference[i] * reference[i];
        den += ((double) reference[i] - estimate[i]) * ((double) reference[i] - estimate[i]);
    }
    return 10.0 * std::log10 (num / std::max (den, 1.0e-30));
}

double correlation (const std::vector<float>& a, const std::vector<float>& b)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
    {
        ab += (double) a[i] * b[i];
        aa += (double) a[i] * a[i];
        bb += (double) b[i] * b[i];
    }
    return ab / std::sqrt (aa * bb);
}

/// One installed model for the whole run (installing converts 55 MB; separation loads it once).
GuitarSeparator& installedSeparator (juce::String& problem)
{
    static GuitarSeparator separator (tempDir().getChildFile ("tone_match_separation_model"));
    static juce::String installProblem = [] {
        const auto url = weightsUrlForTests();
        if (url.isEmpty())
            return juce::String ("no local copy of the weights (set AMPSIM_DEMUCS_WEIGHTS, or AMPSIM_NETWORK_TESTS=1 to download them)");
        separator.modelFile().deleteFile();
        return separator.install (url, std::atomic<bool> { false }, {});
    }();
    problem = installProblem;
    return separator;
}

class ToneMatchSeparationTests final : public juce::UnitTest
{
public:
    ToneMatchSeparationTests() : juce::UnitTest ("Tone match separation", "ampsim") {}

    void runTest() override
    {
        beginTest ("the installer refuses a download of the wrong size or with the wrong SHA-256 (nothing installed), and stops on cancel");
        {
            const auto dir = tempDir().getChildFile ("tone_match_installer");
            dir.deleteRecursively();
            GuitarSeparator s (dir.getChildFile ("model"));

            const auto small = dir.getChildFile ("small.safetensors");
            dir.createDirectory();
            small.replaceWithText ("not a model");
            const auto e1 = s.install (juce::URL (small).toString (false), std::atomic<bool> { false }, {});
            expect (e1.contains ("not " + juce::String (GuitarSeparator::weightsBytes)), e1);
            expect (! s.isInstalled());

            const auto wrong = dir.getChildFile ("wrong.safetensors");
            {
                juce::FileOutputStream o (wrong);
                std::vector<char> zeros (1 << 20, 0);
                for (juce::int64 left = GuitarSeparator::weightsBytes; left > 0; left -= (juce::int64) zeros.size())
                    o.write (zeros.data(), (size_t) std::min (left, (juce::int64) zeros.size()));
            }
            double lastProgress = 0.0;
            juce::String lastStage;
            const auto e2 = s.install (juce::URL (wrong).toString (false), std::atomic<bool> { false }, [&] (double f, const juce::String& st) {
                lastProgress = f;
                lastStage = st;
            });
            expect (e2.contains ("SHA-256"), e2);
            expect (! s.isInstalled() && ! dir.getChildFile ("model/5c90dfd2.safetensors.part").exists());

            const auto e3 = s.install (juce::URL (wrong).toString (false), std::atomic<bool> { true }, {});
            expectEquals (e3, juce::String ("Cancelled"));
            expect (! s.isInstalled());
            logMessage ("  -> 11 bytes: \"" + e1 + "\"; 55 MB of zeros: \"" + e2 + "\" (it had reached " + juce::String (100.0 * lastProgress, 0)
                        + "%: \"" + lastStage + "\"); cancelled: \"" + e3 + "\"; nothing installed after any of them");
        }

        beginTest ("installs the pinned weights (size and SHA-256 checked), converts them to demucs.cpp's format, and the model loads");
        {
            const auto url = weightsUrlForTests();
            if (url.isEmpty())
            {
                logMessage ("  -> NOT RUN: no local copy of the weights (set AMPSIM_DEMUCS_WEIGHTS, or AMPSIM_NETWORK_TESTS=1 to download them)");
            }
            else
            {
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                juce::String problem;
                auto& s = installedSeparator (problem);
                const auto t1 = juce::Time::getMillisecondCounterHiRes();
                expect (problem.isEmpty(), problem);
                expect (s.isInstalled());
                const auto loadError = s.load();
                const auto t2 = juce::Time::getMillisecondCounterHiRes();
                expect (loadError.isEmpty(), loadError);
                logMessage ("  -> from " + url.upToLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("/", false, false) + "/"
                            + url.fromLastOccurrenceOf ("/", false, false) + ": " + juce::String (GuitarSeparator::weightsBytes) + " bytes, SHA-256 "
                            + juce::String (GuitarSeparator::weightsSha256).substring (0, 16) + "..., installed and converted in "
                            + juce::String ((t1 - t0) / 1000.0, 2) + " s (" + juce::String ((double) s.modelFile().getSize() / 1.0e6, 1)
                            + " MB on disk, the download deleted), loaded in " + juce::String ((t2 - t1) / 1000.0, 2) + " s");
            }
        }

        beginTest ("golden: the C++ separation against Python's Demucs on the same 10 s clip (htdemucs_6s, guitar stem)");
        {
            juce::String problem;
            auto& s = installedSeparator (problem);
            if (problem.isNotEmpty())
                logMessage ("  -> NOT RUN: " + problem);
            else
            {
                const auto mix = readMono (fixtures().getChildFile ("separation_mix.wav"));
                const auto python = readMono (fixtures().getChildFile ("separation_guitar_python.wav"));
                juce::String error;
                int reports = 0;
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                const auto stem = s.separate (mix, std::atomic<bool> { false }, [&] (double, const juce::String&) { ++reports; }, error);
                const auto seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
                expect (error.isEmpty(), error);
                expectEquals (stem.size(), mix.size());
                const auto sdr = sdrDb (python, stem), corr = correlation (python, stem);
                const auto mixVsPython = sdrDb (python, mix);
                // Demucs differs from itself between runs (its shift trick draws a random offset, which
                // demucs.cpp can't draw the same way): Python against itself is the yardstick. The port must
                // land within 4 dB of the least similar Python rerun, and correlate above 0.98.
                const auto reruns = expected()["separation"]["python_rerun_sdr_db"];
                double worstRerun = 1.0e9;
                juce::StringArray rerunText;
                if (const auto* a = reruns.getArray())
                    for (const auto& v : *a)
                    {
                        worstRerun = std::min (worstRerun, (double) v);
                        rerunText.add (juce::String ((double) v, 1));
                    }
                expect (worstRerun < 1.0e8, "expected.json has no Python reruns");
                expect (sdr > worstRerun - 4.0, juce::String (sdr));
                expect (corr > 0.98, juce::String (corr));
                logMessage ("  -> 10 s: C++ stem vs Python's: SDR " + juce::String (sdr, 1) + " dB, correlation " + juce::String (corr, 4)
                            + "; Python against its own reruns: " + rerunText.joinIntoString (", ") + " dB; the unseparated mix against Python's stem: "
                            + juce::String (mixVsPython, 1) + " dB; "
                            + juce::String (seconds, 1) + " s (" + juce::String (60.0 * seconds / 10.0, 0) + " s per minute at this length: one part, one worker), "
                            + juce::String (reports) + " progress reports");
            }
        }

        beginTest ("a separation is cancelled from inside demucs.cpp's inference and says so");
        {
            juce::String problem;
            auto& s = installedSeparator (problem);
            if (problem.isNotEmpty())
                logMessage ("  -> NOT RUN: " + problem);
            else
            {
                const auto mix = readMono (fixtures().getChildFile ("separation_mix.wav"));
                std::atomic<bool> cancel { false };
                juce::String error;
                double cancelledAt = 0.0;
                std::thread canceller ([&] {
                    juce::Thread::sleep (1500);
                    cancelledAt = juce::Time::getMillisecondCounterHiRes();
                    cancel = true;
                });
                const auto stem = s.separate (mix, cancel, {}, error);
                const auto returned = juce::Time::getMillisecondCounterHiRes();
                canceller.join();
                expect (stem.empty());
                expectEquals (error, juce::String ("Cancelled"));
                expect (returned - cancelledAt < 3000.0, juce::String (returned - cancelledAt));
                logMessage ("  -> cancelled 1.5 s in; returned " + juce::String (returned - cancelledAt, 0) + " ms later with \"" + error + "\"");
            }
        }

        beginTest ("the session separates first when asked (progress in two halves), then matches the stem; a failed separation is reported");
        {
            WithBuiltInCaptures builtIns;
            AmpSimProcessor p;
            for (int i = 0; i < 4000 && p.isLoading(); ++i)
                juce::Thread::sleep (5);
            ToneMatchSession session (p);
            expect (session.setTargetFile (fixtures().getChildFile ("target_anything.wav")));
            expect (session.setReferenceFile (fixtures().getChildFile ("reference_di.wav")));
            session.setSeparate (true);
            expect (session.whyCantMatch().contains ("Separation"), session.whyCantMatch());

            // A stand-in separator (the real one is above): it reports progress and hands back its input.
            std::atomic<int> calls { 0 };
            double highestDuringSeparation = 0.0;
            session.setSeparator ([&] (const std::vector<float>& x, const std::atomic<bool>&, const ampsim::tonematch::ProgressFn& progress, juce::String&) {
                ++calls;
                for (int i = 1; i <= 10; ++i)
                    progress (i / 10.0, "Separating the guitar");
                highestDuringSeparation = session.getProgress();
                return x;
            });
            expect (session.whyCantMatch().isEmpty(), session.whyCantMatch());
            expect (session.startMatch());
            expect (session.waitForMatch (120000));
            expect (session.hasResult(), session.getError());
            expectEquals (calls.load(), 1);
            expectWithinAbsoluteError (highestDuringSeparation, 0.5, 1.0e-9);

            session.setSeparator ([] (const std::vector<float>&, const std::atomic<bool>&, const ampsim::tonematch::ProgressFn&, juce::String& error) {
                error = "out of memory";
                return std::vector<float>();
            });
            expect (session.startMatch());
            expect (session.waitForMatch (10000));
            expectEquals (session.getError(), juce::String ("Separation failed: out of memory"));
            logMessage ("  -> with a separator set, Separate on: the separation took the first half of the progress (" + juce::String (highestDuringSeparation, 2)
                        + " at its end), the match the rest, and the result came from the stem (" + session.getResult().cab.getFileNameWithoutExtension()
                        + "); a failing separator: \"" + session.getError() + "\"");
        }
    }
};

static ToneMatchSeparationTests toneMatchSeparationTests;
} // namespace
