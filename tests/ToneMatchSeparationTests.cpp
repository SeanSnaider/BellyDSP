// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

// Tone match, Stage C (docs/TONE_MATCH.md): the separation model's installer (size and SHA-256 checks,
// cancel, conversion), the C++ separation against Python's Demucs on the same 10 s clip, and the session
// running a separator before the match.
//
// The weights aren't in the repo. The installer and golden tests use a local copy of the pinned upstream
// file: $AMPSIM_DEMUCS_WEIGHTS, or the Hugging Face cache that `uv run --with demucs` fills (the prototype's
// separation). With neither, they download it when AMPSIM_NETWORK_TESTS=1, and otherwise say they didn't run.
//
// Two groups are off unless asked for, so the default suite stays offline and quick:
//   AMPSIM_HTTP_TESTS=1     the download against a local server (tests/model_server.py, run with uv) that
//                           redirects like Hugging Face and misbehaves on purpose: a dropped connection,
//                           503s, a 404, a damaged body, a 25 s wait for the first byte; then the page's
//                           whole flow (empty model folder, install, separate an MP3 section, match).
//   AMPSIM_NETWORK_TESTS=1  the page's whole flow against the real URL, into an empty temporary folder.

#include "BuiltInCaptures.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "ToneMatchSession.h"
#include "tonematch/GuitarSeparator.h"
#include "ui/ToneMatchPage.h"

#if JUCE_MAC
 #include <sys/resource.h>
#endif

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

bool allFinite (const std::vector<float>& x)
{
    return std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v); });
}

double rmsOf (const std::vector<float>& x, size_t from, size_t to)
{
    double sum = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i)
        sum += (double) x[i] * x[i];
    return std::sqrt (sum / (double) std::max<size_t> (1, to - from));
}

bool flagSet (const char* name)
{
    return juce::SystemStats::getEnvironmentVariable (name, {}) == "1";
}

/// tests/model_server.py, run with uv, serving `file` on a free local port. Stopped when it goes.
struct ModelServer
{
    explicit ModelServer (const juce::File& file)
    {
        portFile = tempDir().getChildFile ("model_server_port.txt");
        portFile.deleteFile();
        auto uv = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".local/bin/uv");
        const juce::String uvPath = uv.existsAsFile() ? uv.getFullPathName() : juce::String ("uv");
        started = process.start (juce::StringArray { uvPath, "run", "--no-project", "--python", "3.11", "python",
                                                     juce::File (AMPSIM_SOURCE_DIR).getChildFile ("tests/model_server.py").getFullPathName(),
                                                     "--file", file.getFullPathName(), "--port-file", portFile.getFullPathName() },
                                 0);
        for (int i = 0; i < 600 && started && ! portFile.existsAsFile(); ++i)
            juce::Thread::sleep (50);
        port = portFile.loadFileAsString().getIntValue();
    }
    ~ModelServer() { process.kill(); }

    bool ok() const { return started && port > 0; }
    juce::String url (const juce::String& mode) const
    {
        return "http://127.0.0.1:" + juce::String (port) + "/" + mode + "/resolve/3c5ee475be622df764938de97e4281a7b07ffa58/5c90dfd2.safetensors";
    }

    juce::ChildProcess process;
    juce::File portFile;
    bool started = false;
    int port = 0;
};

/// The page's whole separation flow, as Sean uses it: a page whose model folder is empty, a song file,
/// a selection that doesn't start at 0, the DI, Separate on, Match; the model installs from `url`, the
/// section is separated, and the stem matched. Returns what the page showed at the end.
struct PageRun
{
    bool hasResult = false;
    juce::String status, error, stages, logText;
    double seconds = 0.0, installSeconds = 0.0;
    juce::int64 peakMB = 0;
    int workers = 0;
    ampsim::tonematch::MatchResult result;
};

PageRun runPage (const juce::File& modelFolder, const juce::String& url, const juce::File& song, double start, double length, const juce::File& di)
{
    PageRun run;
    testing::WithBuiltInCaptures builtIns;
    AmpSimProcessor p;
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
    ui::ToneMatchPage page (p);
    modelFolder.deleteRecursively();
    modelFolder.getParentDirectory().getChildFile ("separation-log.txt").deleteFile();
    page.setSeparationSource (modelFolder, url);
    auto& session = page.getSession();
    if (! session.setTargetFile (song) || ! session.setReferenceFile (di))
    {
        run.error = session.getError();
        return run;
    }
    session.setRange (start, start + length);
    session.setSeparate (true);
    page.getModeChoice().setSelected (1, juce::sendNotification); // Anything, as the docs advise for a stem
    page.startMatch();
    juce::StringArray stages;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    while (session.isMatching() && juce::Time::getMillisecondCounterHiRes() - t0 < 20.0 * 60000.0)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        page.refresh();
        const auto st = page.getStatusText().upToFirstOccurrenceOf (" (", false, false).upToFirstOccurrenceOf (":", false, false);
        if (st.isNotEmpty() && ! stages.contains (st))
        {
            stages.add (st);
            if (st.startsWith ("Separating") && run.installSeconds == 0.0)
                run.installSeconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        }
    }
    page.refresh();
    run.seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    run.hasResult = session.hasResult();
    run.status = page.getStatusText();
    run.error = session.getError();
    run.stages = stages.joinIntoString (" | ");
    run.workers = page.getSeparator().getLastWorkers();
    run.logText = page.getSeparator().logFile().loadFileAsString();
    if (run.hasResult)
        run.result = session.getResult();
   #if JUCE_MAC
    rusage r {};
    getrusage (RUSAGE_SELF, &r);
    run.peakMB = (juce::int64) r.ru_maxrss / (1024 * 1024);
   #endif
    return run;
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

        beginTest ("the worker rule: about 2.2 GB a worker, within 40% of the machine's memory and what's free now, 1 to 4, at most half the cores");
        {
            struct Case { juce::int64 physical, available; int cores, expected; };
            const Case cases[] = { { 24576, 17000, 15, 4 }, // this Mac (M5 Pro, 24 GB)
                                   { 16384, 12000, 10, 2 }, // 6.5 GB budget: two
                                   { 8192, 6000, 8, 1 },    // 3.3 GB budget: one
                                   { 65536, 60000, 16, 4 }, // never more than four
                                   { 24576, 3500, 15, 1 },  // most of the memory in use: one
                                   { 24576, 6200, 15, 2 },  // (6200 - 1500) / 2200 = 2
                                   { 24576, 17000, 2, 1 },  // two cores: one
                                   { 4096, 1000, 4, 1 } };  // never fewer than one
            juce::StringArray table;
            for (const auto& c : cases)
            {
                const auto w = GuitarSeparator::chooseWorkers (c.physical, c.available, c.cores);
                expectEquals (w, c.expected, juce::String (c.physical) + " / " + juce::String (c.available) + " / " + juce::String (c.cores));
                table.add (juce::String (c.physical / 1024) + " GB, " + juce::String ((double) c.available / 1024.0, 1) + " GB free, " + juce::String (c.cores) + " cores: " + juce::String (w));
            }
            const auto here = GuitarSeparator::chooseWorkers (GuitarSeparator::physicalMemoryMB(), GuitarSeparator::availableMemoryMB(), (int) std::thread::hardware_concurrency());
            expect (here >= 1 && here <= GuitarSeparator::maxWorkers);
            logMessage ("  -> " + table.joinIntoString ("; ") + ". This machine now: " + juce::String (GuitarSeparator::physicalMemoryMB()) + " MB, "
                        + juce::String (GuitarSeparator::availableMemoryMB()) + " MB available -> " + juce::String (here));
        }

        beginTest ("the separation log is appended to and capped: past 256 KB it moves to separation-log.old.txt");
        {
            const auto dir = tempDir().getChildFile ("tone_match_log");
            dir.deleteRecursively();
            GuitarSeparator s (dir.getChildFile ("Separation"));
            s.log ("first line");
            expect (s.logFile().loadFileAsString().contains ("first line"));
            const juce::String filler = juce::String::repeatedString ("x", 1000);
            for (int i = 0; i < 300; ++i)
                s.log (filler);
            s.log ("after the cap");
            const auto old = dir.getChildFile ("separation-log.old.txt");
            expect (old.existsAsFile() && old.loadFileAsString().contains ("first line"));
            expect (s.logFile().getSize() < GuitarSeparator::maxLogBytes, juce::String (s.logFile().getSize()));
            expect (s.logFile().loadFileAsString().contains ("after the cap"));
            logMessage ("  -> 300 KB logged: separation-log.txt is " + juce::String (s.logFile().getSize()) + " bytes, separation-log.old.txt "
                        + juce::String (old.getSize()) + " bytes (at most two files, about 512 KB)");
        }

        beginTest ("digital silence in a part doesn't turn the stem into NaN (Demucs divides by the part's standard deviation)");
        {
            juce::String problem;
            auto& s = installedSeparator (problem);
            if (problem.isNotEmpty())
                logMessage ("  -> NOT RUN: " + problem);
            else
            {
                // A song with a 12.5 s silent intro: the first 10 s part (with its 0.75 s of context) is
                // all zeros. Before the fix that part came back NaN, the crossfade and the resampler's
                // filter carried the NaN to the end, and the whole stem was NaN (30 s: 1,439,998 of
                // 1,440,000 samples), which the matcher then called "too little playing".
                const auto mix = readMono (fixtures().getChildFile ("separation_mix.wav"));
                std::vector<float> x ((size_t) (12.5 * 48000.0), 0.0f);
                x.insert (x.end(), mix.begin(), mix.end());
                juce::String error;
                const auto stem = s.separate (x, std::atomic<bool> { false }, {}, error, 2);
                expect (error.isEmpty(), error);
                expectEquals (stem.size(), x.size());
                expect (allFinite (stem));
                const auto silentRms = rmsOf (stem, 0, (size_t) (9.0 * 48000.0));
                const auto musicRms = rmsOf (stem, (size_t) (13.5 * 48000.0), stem.size());
                expect (silentRms < 1.0e-3 && musicRms > 1.0e-3, juce::String (silentRms) + " / " + juce::String (musicRms));
                const auto logged = s.logFile().loadFileAsString().contains ("were digital silence");
                expect (logged);
                logMessage ("  -> 12.5 s of silence then the 10 s mix: every sample finite; the stem's RMS is " + juce::String (20.0 * std::log10 (silentRms + 1.0e-12), 1)
                            + " dBFS over the silence and " + juce::String (20.0 * std::log10 (musicRms), 1) + " dBFS over the music; the log says the silent part wasn't run");
            }
        }

        beginTest ("out of memory: retried with one worker, and if even one doesn't fit, the page is told so");
        {
            juce::String problem;
            auto& s = installedSeparator (problem);
            if (problem.isNotEmpty())
                logMessage ("  -> NOT RUN: " + problem);
            else
            {
                const auto mix = readMono (fixtures().getChildFile ("separation_mix.wav"));
                std::vector<float> x (mix);
                x.insert (x.end(), mix.begin(), mix.begin() + (std::ptrdiff_t) (3 * 48000)); // 13 s: two parts
                juce::String error;
                s.simulateOutOfMemoryAtWorkers = 2; // two at once fail; one alone runs
                const auto stem = s.separate (x, std::atomic<bool> { false }, {}, error, 2);
                expect (error.isEmpty(), error);
                expect (! stem.empty() && allFinite (stem));
                expectEquals (s.getLastWorkers(), 1);
                expect (s.logFile().loadFileAsString().contains ("out of memory with 2 workers; trying again with 1"));

                s.simulateOutOfMemoryAtWorkers = 1; // even one fails
                juce::String error2;
                const auto none = s.separate (x, std::atomic<bool> { false }, {}, error2, 2);
                s.simulateOutOfMemoryAtWorkers = 0;
                expect (none.empty());
                expect (error2.startsWith ("not enough memory to separate"), error2);
                expect (error2.contains ("separation-log.txt"), error2);
                logMessage ("  -> two workers out of memory: separated with 1 instead; one worker out of memory: \"" + error2 + "\"");
            }
        }

        beginTest ("the download against a local server that redirects like Hugging Face and misbehaves (AMPSIM_HTTP_TESTS=1)");
        {
            const auto weights = localWeights();
            if (! flagSet ("AMPSIM_HTTP_TESTS") || weights == juce::File())
                logMessage ("  -> NOT RUN: set AMPSIM_HTTP_TESTS=1 (needs uv, and a local copy of the weights to serve)");
            else
            {
                ModelServer server (weights);
                expect (server.ok(), "the local server didn't start");
                if (server.ok())
                {
                    juce::StringArray report;
                    auto attempt = [&] (const juce::String& mode, bool shouldInstall, const juce::String& messagePart) {
                        const auto dir = tempDir().getChildFile ("tone_match_http_" + mode);
                        dir.deleteRecursively();
                        GuitarSeparator g (dir.getChildFile ("Separation"));
                        g.retryDelayMs = 200;
                        const auto t0 = juce::Time::getMillisecondCounterHiRes();
                        const auto e = g.install (server.url (mode), std::atomic<bool> { false }, {});
                        const auto secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
                        expect (g.isInstalled() == shouldInstall, mode + ": " + e);
                        expect (e.contains (messagePart), mode + ": " + e);
                        juce::StringArray triesSeen; // "try 2 of 5" appears once per try, or twice when it also failed mid-way
                        for (const auto& line : juce::StringArray::fromLines (g.logFile().loadFileAsString()))
                            if (line.contains ("  try "))
                                triesSeen.addIfNotAlreadyThere (line.fromFirstOccurrenceOf ("  try ", false, false).upToFirstOccurrenceOf (" of", false, false));
                        const auto triesLogged = triesSeen.size();
                        report.add (mode + ": " + (shouldInstall ? "installed" : "\"" + e.upToFirstOccurrenceOf (" (details", false, false) + "\"") + " in "
                                    + juce::String (secs, 1) + " s, " + juce::String (triesLogged) + (triesLogged == 1 ? " try" : " tries"));
                        return e;
                    };
                    attempt ("ok", true, "");
                    attempt ("drop", true, "");
                    attempt ("flaky", true, "");
                    attempt ("stall", true, "");
                    attempt ("missing", false, "answered HTTP 404 Not Found");
                    attempt ("corrupt", false, "SHA-256 doesn't match");
                    logMessage ("  -> 302 to another host with a 1 KB signed query (%2F, %3D, %7E, ~; the server refuses it changed): " + report.joinIntoString ("; "));

                    const auto run = runPage (tempDir().getChildFile ("tone_match_http_page/Separation"), server.url ("drop-page"),
                                              fixtures().getChildFile ("song_44k.mp3"), 7.3, 10.0, fixtures().getChildFile ("reference_di.wav"));
                    expect (run.hasResult, run.error);
                    expect (run.stages.contains ("Downloading the separation model") && run.stages.contains ("Separating the guitar"), run.stages);
                    expect (run.logText.contains ("resuming"), run.logText);
                    logMessage ("  -> the page, empty model folder, a 44.1 kHz MP3 from 7.3 s to 17.3 s, the server dropping the first connection at 20 MB: "
                                + (run.hasResult ? "matched " + run.result.cab.getFileNameWithoutExtension() + ", closeness " + juce::String (run.result.closeness, 0) : "\"" + run.status + "\"")
                                + " in " + juce::String (run.seconds, 1) + " s; stages: " + run.stages);
                }
            }
        }

        beginTest ("the page's whole flow from an empty model folder, downloading from the real URL (AMPSIM_NETWORK_TESTS=1)");
        {
            if (! flagSet ("AMPSIM_NETWORK_TESTS"))
                logMessage ("  -> NOT RUN: set AMPSIM_NETWORK_TESTS=1 (downloads 55 MB from huggingface.co)");
            else
            {
                const auto song = juce::SystemStats::getEnvironmentVariable ("AMPSIM_SONG", fixtures().getChildFile ("song_44k.mp3").getFullPathName());
                const auto di = juce::SystemStats::getEnvironmentVariable ("AMPSIM_SONG_DI", fixtures().getChildFile ("reference_di.wav").getFullPathName());
                const auto start = juce::SystemStats::getEnvironmentVariable ("AMPSIM_SONG_START", "7.3").getDoubleValue();
                const auto length = juce::SystemStats::getEnvironmentVariable ("AMPSIM_SONG_SECONDS", "10").getDoubleValue();
                const auto run = runPage (tempDir().getChildFile ("tone_match_network/Separation"), GuitarSeparator::weightsUrl, juce::File (song), start, length, juce::File (di));
                expect (run.hasResult, run.error);
                logMessage ("  -> " + juce::File (song).getFileName() + ", " + juce::String (start, 1) + " s to " + juce::String (start + length, 1) + " s, empty model folder: "
                            + (run.hasResult ? "matched " + run.result.cab.getFileNameWithoutExtension() + ", closeness " + juce::String (run.result.closeness, 0) : "\"" + run.status + "\"")
                            + "; install done " + juce::String (run.installSeconds, 1) + " s in, all done in " + juce::String (run.seconds, 1) + " s, " + juce::String (run.workers)
                            + " workers, the process's peak memory " + juce::String (run.peakMB) + " MB; stages: " + run.stages);
                logMessage ("  -> the log:\n" + run.logText);
            }
        }
    }
};

static ToneMatchSeparationTests toneMatchSeparationTests;
} // namespace
