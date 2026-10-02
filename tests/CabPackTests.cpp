#include "Plot.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "dsp/Cab.h"
#include "dsp/CabPack.h"
#include "dsp/Loudness.h"
#include "dsp/MinimumPhase.h"

#include <complex>

namespace
{
using namespace testing;
using ampsim::CabPack;
namespace minphase = ampsim::minphase;

/// A synthetic close-mic capture: the test cab with the given colour, arriving `delay` samples late.
std::vector<float> capture (double presenceDb, double lowpassHz, int delay, int length = 4096)
{
    const auto h = syntheticCabIR (length - delay, presenceDb, lowpassHz);
    std::vector<float> out ((size_t) length, 0.0f);
    for (size_t n = 0; n < h.size(); ++n)
        out[n + (size_t) delay] = (float) h[n];
    return out;
}

std::complex<double> responseAt (const std::vector<float>& h, double f)
{
    std::complex<double> sum = 0.0;
    const auto w = -juce::MathConstants<double>::twoPi * f / fs;
    for (size_t n = 0; n < h.size(); ++n)
        sum += (double) h[n] * std::polar (1.0, w * (double) n);
    return sum;
}

double magnitudeDb (const std::vector<float>& h, double f)
{
    return 20.0 * std::log10 (std::abs (responseAt (h, f)));
}

std::vector<double> logSpaced (double lo, double hi, int count)
{
    std::vector<double> f ((size_t) count);
    for (int i = 0; i < count; ++i)
        f[(size_t) i] = lo * std::pow (hi / lo, (double) i / (count - 1));
    return f;
}

std::vector<double> magnitudesDb (const std::vector<float>& h, const std::vector<double>& freqs)
{
    std::vector<double> out;
    for (auto f : freqs)
        out.push_back (magnitudeDb (h, f));
    return out;
}

/// How far a is from the best-scaled copy of b, relative to a, in dB (shape only, level ignored).
double shapeErrorDb (const std::vector<float>& a, const std::vector<float>& b)
{
    double ab = 0.0, bb = 0.0, aa = 0.0;
    for (size_t n = 0; n < std::min (a.size(), b.size()); ++n)
    {
        ab += (double) a[n] * b[n];
        bb += (double) b[n] * b[n];
        aa += (double) a[n] * a[n];
    }
    const auto g = ab / bb;
    double err = 0.0;
    for (size_t n = 0; n < std::max (a.size(), b.size()); ++n)
    {
        const auto av = n < a.size() ? (double) a[n] : 0.0;
        const auto bv = n < b.size() ? (double) b[n] : 0.0;
        err += (av - g * bv) * (av - g * bv);
    }
    return 10.0 * std::log10 (err / aa + 1.0e-30);
}

/// Writes IRs as WAV files into a fresh folder in the temp directory, plus an optional manifest.
juce::File writePack (const juce::String& folderName, const std::vector<std::pair<juce::String, std::vector<float>>>& files,
                      const juce::String& manifest = {}, double rate = fs)
{
    const auto folder = tempDir().getChildFile (folderName);
    folder.deleteRecursively();
    folder.createDirectory();
    for (const auto& [name, h] : files)
        writeWav (folder.getChildFile (name), h, rate);
    if (manifest.isNotEmpty())
        folder.getChildFile ("cabpack.json").replaceWithText (manifest);
    return folder;
}

/// The four captures of the test grid pack: the dust cap is brightest, the cone's edge darkest, and
/// the far captures arrive later with a little room reflection.
std::vector<std::pair<juce::String, std::vector<float>>> gridCaptures()
{
    auto far = [] (std::vector<float> h)
    {
        const auto direct = h;
        for (size_t n = 300; n < h.size(); ++n)
            h[n] += 0.25f * direct[n - 300];
        return h;
    };

    return { { "MyCab_Dyn_Cap_1in.wav", capture (8.0, 7000.0, 10) },
             { "MyCab_Dyn_Edge_1in.wav", capture (0.0, 3500.0, 14) },
             { "MyCab_Dyn_Cap_4in.wav", far (capture (6.0, 6000.0, 30)) },
             { "MyCab_Dyn_Edge_4in.wav", far (capture (-2.0, 3000.0, 35)) } };
}

void setParam (AmpSimProcessor& p, const juce::String& id, float plainValue)
{
    auto* param = p.parameters.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

void waitForLoads (AmpSimProcessor& p)
{
    for (int i = 0; i < 4000 && p.isLoading(); ++i)
        juce::Thread::sleep (5);
}

class CabPackTests final : public juce::UnitTest
{
public:
    CabPackTests() : juce::UnitTest ("Cab packs (movable mics)", "ampsim") {}

    void runTest() override
    {
        beginTest ("minimum phase keeps the magnitude and moves the energy as early as possible");
        {
            // Analytic case: 0.4 + z^-1 has its zero at -2.5, outside the unit circle. The minimum-phase
            // sequence with the same magnitude reflects the zero inside: 1 + 0.4 z^-1.
            const auto twoTap = minphase::minimumPhase ({ 0.4f, 1.0f }, minphase::fftOrderFor (2));
            expectWithinAbsoluteError (twoTap[0], 1.0f, 1.0e-4f);
            expectWithinAbsoluteError (twoTap[1], 0.4f, 1.0e-4f);

            // A mixed-phase cab IR: the cab arriving 30 samples late, plus a reflection 200 samples later.
            auto h = capture (4.0, 5000.0, 30);
            const auto direct = h;
            for (size_t n = 200; n < h.size(); ++n)
                h[n] += 0.6f * direct[n - 200];
            const auto hm = minphase::minimumPhase (h, minphase::fftOrderFor ((int) h.size()));

            double worst = 0.0;
            for (auto f : logSpaced (30.0, 16000.0, 300))
                worst = std::max (worst, std::abs (magnitudeDb (h, f) - magnitudeDb (hm, f)));
            expectLessThan (worst, 0.01);

            // Among all IRs with one magnitude response, the minimum-phase one has the most energy
            // up to every sample n (its partial energy dominates).
            double eh = 0.0, em = 0.0, total = 0.0, firstMsH = 0.0, firstMsM = 0.0;
            for (auto v : h)
                total += (double) v * v;
            int violations = 0;
            for (size_t n = 0; n < h.size(); ++n)
            {
                eh += (double) h[n] * h[n];
                em += (double) hm[n] * hm[n];
                if (em < eh - 1.0e-5 * total)
                    ++violations;
                if (n == 47)
                {
                    firstMsH = eh / total;
                    firstMsM = em / total;
                }
            }
            expectEquals (violations, 0);
            logMessage ("  -> 0.4 + z^-1 becomes " + juce::String (twoTap[0], 5) + " + " + juce::String (twoTap[1], 5) + " z^-1 (exact: 1 + 0.4 z^-1)");
            logMessage ("  -> mixed-phase cab IR: magnitude kept to " + juce::String (worst, 4) + " dB (30 Hz to 16 kHz); partial energy "
                        "falls short of the original's at " + juce::String (violations) + " of " + juce::String ((int) h.size())
                        + " samples; energy in the first 1 ms: original " + juce::String (100.0 * firstMsH, 1)
                        + "%, minimum phase " + juce::String (100.0 * firstMsM, 1) + "%");
        }

        beginTest ("rebuilding puts the arrival time back exactly, and minimum phase is idempotent");
        {
            const auto h = capture (4.0, 5000.0, 0);
            const auto order = minphase::fftOrderFor ((int) h.size());
            const auto hm = minphase::minimumPhase (h, order);
            const auto twice = minphase::minimumPhase (hm, order);
            const auto idempotence = relativeErrorDb (twice, hm);
            expectLessThan (idempotence, -70.0);

            const auto logMag = minphase::logMagnitude (hm, order);
            const auto length = (int) h.size() + 64;
            const auto base = minphase::fromLogMagnitude (logMag, order, 0.0, length);
            juce::StringArray results;

            for (auto d : { 7, 30 })
            {
                const auto rebuilt = minphase::fromLogMagnitude (logMag, order, (double) d, length);
                std::vector<float> realigned (rebuilt.begin() + d, rebuilt.end());
                std::vector<float> reference (base.begin(), base.end() - d);
                const auto error = relativeErrorDb (realigned, reference);
                expectLessThan (error, -80.0);
                results.add ("delay " + juce::String (d) + ": the same IR " + juce::String (d) + " samples later to " + juce::String (error, 1) + " dB");
            }

            // A fractional delay is a phase slope: rebuilt / base = exp(-j 2 pi f d / fs). Measured at
            // 500 Hz and 2 kHz.
            for (auto d : { 0.25, 12.5, 30.75 })
            {
                const auto rebuilt = minphase::fromLogMagnitude (logMag, order, d, length);
                double worstDelayError = 0.0;
                for (auto f : { 500.0, 2000.0 })
                {
                    const auto ratio = responseAt (rebuilt, f) / responseAt (base, f);
                    const auto wrapped = -std::arg (ratio) * fs / (juce::MathConstants<double>::twoPi * f);
                    const auto period = fs / f; // the phase only knows the delay modulo one period
                    const auto measured = wrapped + period * std::round ((d - wrapped) / period);
                    worstDelayError = std::max (worstDelayError, std::abs (measured - d));
                }
                expectLessThan (worstDelayError, 1.0e-3);
                results.add ("delay " + juce::String (d, 2) + ": phase slope off by " + juce::String (worstDelayError, 6) + " samples");
            }

            logMessage ("  -> minimum phase of a minimum-phase IR changes it by " + juce::String (idempotence, 1) + " dB");
            logMessage ("  -> " + results.joinIntoString ("; "));
        }

        beginTest ("file names place a pack on the map");
        {
            const auto parse = [] (std::initializer_list<const char*> names, bool& allParsed)
            {
                juce::Array<juce::File> files;
                for (auto n : names)
                    files.add (juce::File ("/tmp/pack").getChildFile (n));
                return CabPack::placeFromNames (files, allParsed);
            };

            bool ok = false;
            const auto grid = parse ({ "MyCab Dyn Cap 1in.wav", "MyCab_Dyn_CapEdge_1in.wav", "MyCab-Dyn-Cone-1in.wav", "MyCab_Dyn_Edge_1in.wav",
                                       "MyCab Dyn Cap 4in.wav", "MyCab_Dyn_Cap-Edge_4in.wav", "MyCab-Dyn-Cone-4in.wav", "MyCab_Dyn_Edge_4 in.wav" }, ok);
            expect (ok);
            const double expectedX[] = { 0.0, 0.25, 0.6, 1.0, 0.0, 0.25, 0.6, 1.0 };
            const double expectedY[] = { 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0 };
            for (size_t i = 0; i < grid.size(); ++i)
            {
                expectWithinAbsoluteError (grid[i].x, expectedX[i], 1.0e-9);
                expectWithinAbsoluteError (grid[i].y, expectedY[i], 1.0e-9);
            }

            const auto metric = parse ({ "Cone 10mm.wav", "Cone 2.5cm.wav", "Cone 40 mm.wav" }, ok);
            expect (ok);
            expectWithinAbsoluteError (metric[1].y, 0.5, 1.0e-9); // 10, 25, 40 mm -> 0, 0.5, 1

            bool unnamed = true, duplicate = true, partial = true;
            parse ({ "Bright.wav", "Dark.wav", "Fizzy.wav" }, unnamed);
            parse ({ "Cap_1in_a.wav", "Cap_1in_b.wav" }, duplicate);
            parse ({ "Cap_1in.wav", "Mystery.wav" }, partial);
            expect (! unnamed && ! duplicate && ! partial);

            logMessage ("  -> Cap/CapEdge/Cap-Edge/Cone/Edge at 1in and 4in: x = 0, 0.25, 0.6, 1 and y = 0 (1in), 1 (4in), all 8 exact; "
                        "10mm/2.5cm/40 mm -> y 0, " + juce::String (metric[1].y, 2) + ", 1");
            logMessage ("  -> rejected (fall back to file order): names without positions, two files at one spot, half the files unnamed");
        }

        beginTest ("loading a folder: manifest, file names, or file order, and bad packs are refused");
        {
            const auto a = capture (8.0, 7000.0, 10), b = capture (0.0, 3500.0, 14), c = capture (4.0, 5000.0, 20);

            CabPack manifestPack;
            const auto withManifest = writePack ("pack_manifest", { { "a.wav", a }, { "b.wav", b }, { "c.wav", c } },
                                                 R"({ "name": "Test", "points": [ { "file": "a.wav", "x": 0, "y": 0 },
                                                      { "file": "b.wav", "x": 1, "y": 0 }, { "file": "c.wav", "x": 0.5, "y": 1 } ] })");
            const auto r1 = manifestPack.load (withManifest);
            expect (r1.ok && r1.message.contains ("placed by cabpack.json"), r1.message);
            expect (manifestPack.getLayout() == CabPack::Layout::scattered);
            expectWithinAbsoluteError (manifestPack.getPoints()[2].y, 1.0, 1.0e-9);

            CabPack namedPack;
            const auto r2 = namedPack.load (writePack ("pack_names", gridCaptures()));
            expect (r2.ok && r2.placedFromNames && r2.message.contains ("placed by file names"), r2.message);
            expect (namedPack.getLayout() == CabPack::Layout::grid);

            CabPack orderPack;
            const auto r3 = orderPack.load (writePack ("pack_order", { { "Bright.wav", a }, { "Dark.wav", b }, { "Mid.wav", c } }));
            expect (r3.ok && ! r3.placedFromNames);
            expect (orderPack.getLayout() == CabPack::Layout::line);
            expectWithinAbsoluteError (orderPack.getPoints()[1].x, 0.5, 1.0e-9);
            expect (r3.message.contains ("file order"));

            CabPack mixedRates, missing, empty;
            const auto mixedFolder = writePack ("pack_rates", { { "Cap_1in.wav", a } });
            writeWav (mixedFolder.getChildFile ("Edge_1in.wav"), b, 44100.0);
            const auto r4 = mixedRates.load (mixedFolder);
            const auto r5 = missing.load (writePack ("pack_missing", { { "a.wav", a } }, R"({ "points": [ { "file": "nope.wav", "x": 0, "y": 0 } ] })"));
            const auto r6 = empty.load (writePack ("pack_empty", {}));
            expect (! r4.ok && r4.message.contains ("sample rate"));
            expect (! r5.ok && r5.message.contains ("nope.wav"));
            expect (! r6.ok);

            logMessage ("  -> manifest: \"" + r1.message + "\"");
            logMessage ("  -> file names: \"" + r2.message + "\"");
            logMessage ("  -> neither: \"" + r3.message + "\"");
            logMessage ("  -> refused: \"" + r4.message + "\"; \"" + r5.message + "\"; \"" + r6.message + "\"");
        }

        beginTest ("weights: bilinear on a grid, linear on a line, inverse distance for scattered points");
        {
            const auto h = capture (4.0, 5000.0, 10, 512);
            const auto weightOf = [] (const std::vector<std::pair<int, double>>& w, int index)
            {
                double sum = 0.0;
                for (const auto& [i, v] : w)
                    if (i == index)
                        sum += v;
                return sum;
            };

            CabPack grid; // 3 x 2: x = 0, 0.4, 1 at y = 0 and 1
            grid.loadFromMemory ({ { {}, 0.0, 0.0 }, { {}, 0.4, 0.0 }, { {}, 1.0, 0.0 }, { {}, 0.0, 1.0 }, { {}, 0.4, 1.0 }, { {}, 1.0, 1.0 } },
                                 { h, h, h, h, h, h }, fs, "grid");
            const auto w1 = grid.weightsAt (0.1, 0.6); // cell x [0, 0.4]: u = 0.25, v = 0.6
            expectWithinAbsoluteError (weightOf (w1, 0), 0.75 * 0.4, 1.0e-12);
            expectWithinAbsoluteError (weightOf (w1, 1), 0.25 * 0.4, 1.0e-12);
            expectWithinAbsoluteError (weightOf (w1, 3), 0.75 * 0.6, 1.0e-12);
            expectWithinAbsoluteError (weightOf (w1, 4), 0.25 * 0.6, 1.0e-12);
            const auto w2 = grid.weightsAt (0.7, 0.5); // cell x [0.4, 1]: u = 0.5
            for (int i : { 1, 2, 4, 5 })
                expectWithinAbsoluteError (weightOf (w2, i), 0.25, 1.0e-12);
            const auto clamped = grid.weightsAt (-3.0, 9.0);
            expectWithinAbsoluteError (weightOf (clamped, 3), 1.0, 1.0e-12);

            CabPack line;
            line.loadFromMemory ({ { {}, 0.0, 0.0 }, { {}, 0.4, 0.0 }, { {}, 1.0, 0.0 } }, { h, h, h }, fs, "line");
            const auto w3 = line.weightsAt (0.2, 0.9); // y doesn't matter on a line
            expectWithinAbsoluteError (weightOf (w3, 0), 0.5, 1.0e-12);
            expectWithinAbsoluteError (weightOf (w3, 1), 0.5, 1.0e-12);

            CabPack scattered;
            scattered.loadFromMemory ({ { {}, 0.0, 0.0 }, { {}, 1.0, 0.1 }, { {}, 0.3, 0.9 }, { {}, 0.8, 0.7 }, { {}, 0.5, 0.4 } },
                                      { h, h, h, h, h }, fs, "scattered");
            const auto w4 = scattered.weightsAt (0.45, 0.42);
            double sum = 0.0;
            for (const auto& [i, v] : w4)
                sum += v;
            expectEquals ((int) w4.size(), 4);
            expectWithinAbsoluteError (sum, 1.0, 1.0e-12);
            expectGreaterThan (weightOf (w4, 4), 0.5); // the nearest point dominates
            expectWithinAbsoluteError (weightOf (scattered.weightsAt (0.8, 0.7), 3), 1.0, 1.0e-12);

            logMessage ("  -> 3x2 grid at (0.1, 0.6): corner weights 0.300, 0.100, 0.450, 0.150 (bilinear, exact); at (0.7, 0.5) 0.25 each; "
                        "outside the map: clamped to the nearest corner");
            logMessage ("  -> line: halfway between x = 0 and 0.4 gives 0.5 / 0.5 whatever y is; scattered: 4 nearest points, weights sum to 1, "
                        "nearest gets " + juce::String (weightOf (w4, 4), 3) + ", exactly 1 on a captured point");
        }

        // The grid pack used from here on, loaded the way the app loads it.
        CabPack pack;
        const auto packFolder = writePack ("pack_grid", gridCaptures());
        expect (pack.load (packFolder).ok);
        const auto captures = gridCaptures();

        beginTest ("on a captured point the mic uses the original capture, untouched");
        {
            juce::StringArray results;
            for (const auto& point : pack.getPoints())
            {
                const auto ir = pack.irAt (point.x, point.y);
                std::vector<float> original;
                for (const auto& [name, h] : captures)
                    if (name == point.file.getFileName())
                        original = h;

                // The capture scaled by its loudness-matching gain, computed independently here.
                const auto gain = ampsim::loudness::whiteNoiseMatchingGain ({ original.data() }, (int) original.size(), fs);
                double worst = 0.0;
                for (size_t n = 0; n < original.size(); ++n)
                    worst = std::max (worst, (double) std::abs (ir[n] - (float) (original[n] * gain)));
                expectEquals (worst, 0.0);
                expectEquals (ir.size(), original.size());

                // ...and not its minimum-phase version: the original phase is kept.
                const auto asMinimumPhase = shapeErrorDb (minphase::minimumPhase (ir, minphase::fftOrderFor ((int) ir.size())), ir);
                expectGreaterThan (asMinimumPhase, -20.0);
                results.add (point.file.getFileNameWithoutExtension().fromFirstOccurrenceOf ("Dyn_", false, false) + " bit-exact");
            }
            logMessage ("  -> " + results.joinIntoString (", ") + " (the capture times its loudness gain; its minimum-phase version would differ "
                        "by more than -20 dB)");
        }

        beginTest ("small moves make small spectral changes (continuity)");
        {
            CabPack line;
            line.loadFromMemory ({ { {}, 0.0, 0.0 }, { {}, 1.0, 0.0 } }, { captures[0].second, captures[1].second }, fs, "line");
            const auto freqs = logSpaced (80.0, 8000.0, 120);
            const auto a = magnitudesDb (line.irAt (0.0, 0.0), freqs), b = magnitudesDb (line.irAt (1.0, 0.0), freqs);
            double span = 0.0;
            for (size_t k = 0; k < freqs.size(); ++k)
                span = std::max (span, std::abs (a[k] - b[k]));

            const auto largestStep = [&] (int steps)
            {
                double largest = 0.0;
                auto previous = a;
                for (int i = 1; i <= steps; ++i)
                {
                    const auto current = magnitudesDb (line.irAt ((double) i / steps, 0.0), freqs);
                    for (size_t k = 0; k < freqs.size(); ++k)
                        largest = std::max (largest, std::abs (current[k] - previous[k]));
                    previous = current;
                }
                return largest;
            };

            const auto step100 = largestStep (100), step50 = largestStep (50);
            expectLessThan (step100, 1.2 * span / 100.0 + 0.01);
            expectWithinAbsoluteError (step50 / step100, 2.0, 0.2);

            // Halfway, the magnitude is the average of the two in dB (log-magnitude interpolation).
            const auto middle = magnitudesDb (line.irAt (0.5, 0.0), freqs);
            double middleError = 0.0;
            for (size_t k = 0; k < freqs.size(); ++k)
                middleError = std::max (middleError, std::abs (middle[k] - 0.5 * (a[k] + b[k])));
            expectLessThan (middleError, 0.1);

            logMessage ("  -> the two captures differ by up to " + juce::String (span, 2) + " dB (80 Hz to 8 kHz); moving across in 100 steps, "
                        "the largest change per step is " + juce::String (step100, 3) + " dB (linear would be " + juce::String (span / 100.0, 3)
                        + "); in 50 steps " + juce::String (step50, 3) + " dB, x" + juce::String (step50 / step100, 2));
            logMessage ("  -> halfway, the response is the dB average of the two to within " + juce::String (middleError, 5) + " dB");
        }

        beginTest ("a moving mic never comb filters, where a naive crossfade does");
        {
            // The same cab caught 12 samples apart (about 8.5 cm), slightly differently coloured.
            const auto a = capture (4.0, 5000.0, 10), b = capture (6.0, 6000.0, 22);
            CabPack line;
            line.loadFromMemory ({ { {}, 0.0, 0.0 }, { {}, 1.0, 0.0 } }, { a, b }, fs, "comb");
            const auto ha = line.irAt (0.0, 0.0), hb = line.irAt (1.0, 0.0), morph = line.irAt (0.5, 0.0);
            std::vector<float> naive (ha.size());
            for (size_t n = 0; n < naive.size(); ++n)
                naive[n] = 0.5f * (ha[n] + hb[n]);

            const auto freqs = logSpaced (200.0, 8000.0, 1500);
            const auto da = magnitudesDb (ha, freqs), db = magnitudesDb (hb, freqs);
            const auto dm = magnitudesDb (morph, freqs), dn = magnitudesDb (naive, freqs);
            double naiveDip = 0.0, naiveDipAt = 0.0, morphWorst = 0.0;
            std::vector<double> target;
            for (size_t k = 0; k < freqs.size(); ++k)
            {
                target.push_back (0.5 * (da[k] + db[k]));
                if (dn[k] - target[k] < naiveDip)
                {
                    naiveDip = dn[k] - target[k];
                    naiveDipAt = freqs[k];
                }
                morphWorst = std::max (morphWorst, std::abs (dm[k] - target[k]));
            }

            expectLessThan (naiveDip, -20.0);
            expectLessThan (morphWorst, 0.5);

            const auto onsetA = minphase::onset (ha), onsetB = minphase::onset (hb), onsetMorph = minphase::onset (morph);
            expectWithinAbsoluteError (onsetMorph, 0.5 * (onsetA + onsetB), 0.15);

            PlotOptions options;
            options.title = "Halfway between two captures 12 samples apart";
            options.xLabel = "Frequency (Hz)";
            options.yLabel = "Magnitude (dB)";
            options.logX = true;
            options.xMin = 200.0;
            options.xMax = 8000.0;
            const auto peak = *std::max_element (target.begin(), target.end());
            options.yMax = std::ceil (peak / 5.0) * 5.0 + 5.0;
            options.yMin = options.yMax - 60.0;
            const auto plot = proofDir().getChildFile ("cab_morph_vs_crossfade.png");
            expect (savePlot (plot, options, { { "naive 50/50 crossfade", freqs, dn, plotColour (1), 1.5f },
                                               { "morph (this pack)", freqs, dm, plotColour (0), 2.5f },
                                               { "target: dB average", freqs, target, plotColour (6), 1.5f, true } }));

            logMessage ("  -> naive crossfade: a notch " + juce::String (-naiveDip, 1) + " dB deep at " + juce::String (juce::roundToInt (naiveDipAt))
                        + " Hz; morph: within " + juce::String (morphWorst, 5) + " dB of the dB average everywhere from 200 Hz to 8 kHz");
            logMessage ("  -> arrival: captures at " + juce::String (onsetA, 2) + " and " + juce::String (onsetB, 2) + " samples, morph at "
                        + juce::String (onsetMorph, 2) + " (halfway is " + juce::String (0.5 * (onsetA + onsetB), 2) + ")");
            logMessage ("  -> " + plot.getFullPathName());
        }

        beginTest ("the arrival time moves smoothly between captures");
        {
            const auto a = capture (4.0, 5000.0, 10);
            const auto b = capture (4.0, 5000.0, 30);
            CabPack line;
            line.loadFromMemory ({ { {}, 0.0, 0.0 }, { {}, 1.0, 0.0 } }, { a, b }, fs, "delay");
            const auto start = minphase::onset (line.irAt (0.0, 0.0));
            juce::StringArray results;
            for (auto t : { 0.25, 0.5, 0.75 })
            {
                const auto moved = minphase::onset (line.irAt (t, 0.0)) - start;
                expectWithinAbsoluteError (moved, 20.0 * t, 0.15);
                results.add (juce::String (t, 2) + " of the way: +" + juce::String (moved, 3) + " samples (expected " + juce::String (20.0 * t, 2) + ")");
            }
            logMessage ("  -> captures 20 samples apart: " + results.joinIntoString ("; "));
        }

        beginTest ("in the cab, a pack makes a close mic movable and a single file makes it fixed again");
        {
            ampsim::Cab cab;
            cab.prepare (fs, blockSize);
            const auto loaded = cab.loadCloseMicPack (0, packFolder, 0.5, 0.25);
            expect (loaded.ok, loaded.message);
            expect (cab.hasPack (0) && ! cab.hasPack (1));
            expect (cab.getPackLayout (0) == CabPack::Layout::grid);
            expectEquals ((int) cab.getPackPoints (0).size(), 4);

            const auto between = shapeErrorDb (cab.closeMic (0).getLoadedIR(), pack.irAt (0.5, 0.25));
            expectLessThan (between, -100.0);

            // What a morph costs the loader thread.
            const auto timed = [] (auto&& work)
            {
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                work();
                return juce::Time::getMillisecondCounterHiRes() - t0;
            };
            const auto morphMs = timed ([&] { pack.irAt (0.3, 0.6); });
            const auto moveMs = timed ([&] { cab.moveCloseMic (0, 0.3, 0.6); });

            const auto moved = cab.moveCloseMic (0, 1.0, 1.0);
            const auto onPoint = shapeErrorDb (cab.closeMic (0).getLoadedIR(), captures[3].second);
            expect (moved.ok);
            expectLessThan (onPoint, -100.0);

            const auto single = tempDir().getChildFile ("pack_single_ir.wav");
            writeWav (single, toBuffer (syntheticCabIR (2048)));
            expect (cab.loadCloseMic (0, single).ok);
            expect (! cab.hasPack (0));
            expect (! cab.moveCloseMic (0, 0.5, 0.5).ok);

            logMessage ("  -> \"" + loaded.message + "\"");
            logMessage ("  -> the mic's IR at (0.5, 0.25) matches the pack's morph to " + juce::String (between, 1) + " dB; moved onto the far edge "
                        "capture it matches that capture to " + juce::String (onPoint, 1) + " dB; a single IR file then removes the pack");
            logMessage ("  -> loader cost for 85 ms IRs: the morph itself " + juce::String (morphMs, 1) + " ms, the whole move (morph, loudness "
                        "match, alignment) " + juce::String (moveMs, 1) + " ms");
        }

        beginTest ("dragging a mic re-morphs at most every 40 ms and always ends on the final position");
        {
            AmpSimProcessor p;
            p.prepareToPlay (fs, blockSize);
            p.loadCabIR (0, packFolder);
            waitForLoads (p);
            expect (p.getStatus().cab[0].contains ("4 IRs on a grid"), p.getStatus().cab[0]);
            expectEquals ((int) p.getCabPackPoints (0).size(), 4);

            p.loadCabIR (AmpSimProcessor::roomMic, packFolder);
            expect (p.getStatus().cabError[(size_t) AmpSimProcessor::roomMic]);

            // One second of dragging: the GUI moves the mic every 2 ms, and housekeeping runs just as
            // often (the app's timer runs at 50 Hz), so the 40 ms limit has to come from the processor.
            const auto xId = AmpSimProcessor::cabParamId (0, "pos_x"), yId = AmpSimProcessor::cabParamId (0, "pos_y");
            std::vector<double> morphTimes;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            int counted = p.getMorphCount(), positions = 0;

            for (auto now = t0; now - t0 < 1000.0; now = juce::Time::getMillisecondCounterHiRes())
            {
                const auto t = (now - t0) / 1000.0;
                setParam (p, xId, (float) (0.5 + 0.5 * std::sin (6.0 * t)));
                setParam (p, yId, (float) t);
                ++positions;
                p.runHousekeeping();
                if (p.getMorphCount() != counted)
                {
                    counted = p.getMorphCount();
                    morphTimes.push_back (juce::Time::getMillisecondCounterHiRes());
                }
                juce::Thread::sleep (2);
            }

            // Let go somewhere specific, then let housekeeping catch up.
            setParam (p, xId, 0.8f);
            setParam (p, yId, 0.3f);
            for (int i = 0; i < 40; ++i)
            {
                p.runHousekeeping();
                juce::Thread::sleep (5);
            }
            waitForLoads (p);

            double shortest = 1.0e9;
            for (size_t i = 1; i < morphTimes.size(); ++i)
                shortest = std::min (shortest, morphTimes[i] - morphTimes[i - 1]);

            expectGreaterThan ((int) morphTimes.size(), 10);
            expectLessOrEqual ((int) morphTimes.size(), 26);
            expectGreaterOrEqual (shortest, 39.0);

            const auto finalX = (double) p.parameters.getRawParameterValue (xId)->load();
            const auto finalY = (double) p.parameters.getRawParameterValue (yId)->load();
            const auto finalError = shapeErrorDb (p.getChain().cab.closeMic (0).getLoadedIR(), pack.irAt (finalX, finalY));
            expectLessThan (finalError, -100.0);
            expect (p.getStatus().cab[0] == "pack_grid (4 IRs on a grid) at 0.80, 0.30", p.getStatus().cab[0]);

            logMessage ("  -> 1 s of dragging (" + juce::String (positions) + " positions): " + juce::String ((int) morphTimes.size())
                        + " morphs, at least " + juce::String (shortest, 1) + " ms apart (limit: 26 in 1 s, 40 ms apart)");
            logMessage ("  -> after letting go at (0.80, 0.30): \"" + p.getStatus().cab[0] + "\", IR matches the morph there to "
                        + juce::String (finalError, 1) + " dB");
            logMessage ("  -> the room mic refuses a pack: \"" + p.getStatus().cab[(size_t) AmpSimProcessor::roomMic] + "\"");
        }
    }
};

CabPackTests cabPackTests;
} // namespace
