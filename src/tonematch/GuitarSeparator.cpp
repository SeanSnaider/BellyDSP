// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "GuitarSeparator.h"

#include "AudioFileInput.h"

#include "../platform/AppInfo.h"

#include <juce_cryptography/juce_cryptography.h>

#include <model.hpp>

#include <mutex>
#include <thread>

namespace ampsim::tonematch
{

namespace
{
/// Thrown from demucs.cpp's progress callback to stop an inference that's been cancelled. demucs.cpp
/// calls the callback between layers and holds everything in RAII containers, so unwinding through it
/// is clean.
struct Cancelled
{
};

constexpr int magicSixSource = 0x646d6336; // "dmc6": demucs.cpp's tag for the six-source model

std::vector<float> resample (const std::vector<float>& x, double from, double to)
{
    if (std::abs (from - to) < 0.5 || x.empty())
        return x;
    // AudioFileInput::resampleTo48k resamples sampleRate -> 48 kHz; scaling the rates keeps the ratio.
    auto y = AudioFileInput::resampleTo48k (x, from * 48000.0 / to);
    return y;
}
} // namespace

GuitarSeparator::GuitarSeparator (juce::File f) : folder (std::move (f)) {}
GuitarSeparator::~GuitarSeparator() = default;

juce::File GuitarSeparator::defaultFolder()
{
    return platform::userDataFolder().getChildFile ("Separation");
}

juce::String GuitarSeparator::sha256Of (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk())
        return {};
    return juce::SHA256 (in).toHexString();
}

// ---- Installing the weights --------------------------------------------------------------------------------

juce::String GuitarSeparator::install (const juce::String& url, const std::atomic<bool>& cancel, const ProgressFn& progress)
{
    if (! folder.createDirectory())
        return "Can't create " + folder.getFullPathName();

    const auto part = folder.getChildFile ("5c90dfd2.safetensors.part");
    part.deleteFile();
    {
        auto in = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                         .withConnectionTimeoutMs (20000)
                                                         .withNumRedirectsToFollow (5));
        if (in == nullptr)
            return "Couldn't connect to download the separation model (" + url + ")";

        juce::FileOutputStream out (part);
        if (! out.openedOk())
            return "Can't write " + part.getFullPathName();

        const auto total = in->getTotalLength() > 0 ? in->getTotalLength() : weightsBytes;
        std::vector<char> buffer (1 << 16);
        juce::int64 done = 0;
        for (;;)
        {
            if (cancel.load())
            {
                out.flush();
                part.deleteFile();
                return "Cancelled";
            }
            const auto n = in->read (buffer.data(), (int) buffer.size());
            if (n <= 0)
                break;
            out.write (buffer.data(), (size_t) n);
            done += n;
            if (progress)
                progress (0.9 * (double) done / (double) total, "Downloading the separation model (" + juce::String (done / 1000000) + " of "
                                                                    + juce::String (weightsBytes / 1000000) + " MB)");
        }
        out.flush();
    }

    if (part.getSize() != weightsBytes)
    {
        const auto got = part.getSize();
        part.deleteFile();
        return "The download was " + juce::String (got) + " bytes, not " + juce::String (weightsBytes) + "; nothing installed";
    }
    if (progress)
        progress (0.92, "Checking the download");
    if (sha256Of (part) != weightsSha256)
    {
        part.deleteFile();
        return "The download's SHA-256 doesn't match the pinned one; nothing installed";
    }

    if (progress)
        progress (0.95, "Converting the model");
    const auto converted = folder.getChildFile ("htdemucs_6s.demucscpp.bin.part");
    if (const auto e = convert (part, converted); e.isNotEmpty())
    {
        part.deleteFile();
        converted.deleteFile();
        return e;
    }
    part.deleteFile();
    if (! converted.moveFileTo (modelFile()))
        return "Can't move the converted model into place";
    if (progress)
        progress (1.0, "Separation model installed");
    return {};
}

juce::String GuitarSeparator::convert (const juce::File& safetensors, const juce::File& out)
{
    // safetensors: an 8-byte little-endian header length, a JSON header naming each tensor's dtype, shape,
    // and [begin, end) byte offsets into the data that follows, then the data.
    juce::FileInputStream in (safetensors);
    if (! in.openedOk())
        return "Can't read " + safetensors.getFullPathName();
    const auto headerLength = (juce::int64) in.readInt64();
    if (headerLength <= 0 || headerLength > 10'000'000)
        return "Not a safetensors file";
    juce::MemoryBlock headerBytes;
    in.readIntoMemoryBlock (headerBytes, (ssize_t) headerLength);
    const auto header = juce::JSON::parse (headerBytes.toString());
    const auto* tensors = header.getDynamicObject();
    if (tensors == nullptr)
        return "The model's header doesn't parse";
    const auto dataStart = 8 + headerLength;

    out.deleteFile();
    juce::FileOutputStream o (out);
    if (! o.openedOk())
        return "Can't write " + out.getFullPathName();
    o.writeInt (magicSixSource);

    int written = 0;
    for (const auto& [name, info] : tensors->getProperties())
    {
        if (name.toString() == "__metadata__")
            continue;
        if (info["dtype"].toString() != "F16")
            return "Unexpected tensor type " + info["dtype"].toString() + " for " + name.toString();

        // The shape squeezed (dimensions of 1 dropped), as the conversion script's numpy squeeze() does.
        std::vector<int> dims;
        juce::int64 elements = 1;
        if (const auto* shape = info["shape"].getArray())
            for (const auto& d : *shape)
            {
                elements *= (juce::int64) d;
                if ((int) d != 1)
                    dims.push_back ((int) d);
            }
        const auto* offsets = info["data_offsets"].getArray();
        if (offsets == nullptr || offsets->size() != 2)
            return "No data offsets for " + name.toString();
        const auto begin = (juce::int64) (*offsets)[0], end = (juce::int64) (*offsets)[1];
        if (end - begin != elements * 2)
            return "Size mismatch for " + name.toString();

        const auto utf8 = name.toString().toStdString();
        o.writeInt ((int) dims.size());
        o.writeInt ((int) utf8.size());
        for (auto d : dims)
            o.writeInt (d);
        o.write (utf8.data(), utf8.size());

        juce::MemoryBlock data;
        in.setPosition (dataStart + begin);
        if (in.readIntoMemoryBlock (data, (ssize_t) (end - begin)) != (size_t) (end - begin))
            return "The model file is truncated";
        o.write (data.getData(), data.getSize());
        ++written;
    }
    o.flush();
    return written > 0 ? juce::String() : "The model file holds no tensors";
}

// ---- Running it -----------------------------------------------------------------------------------------------

juce::String GuitarSeparator::load()
{
    if (model != nullptr)
        return {};
    if (! isInstalled())
        return "The separation model isn't installed";
    auto m = std::make_unique<demucscpp::demucs_model>();
    if (! demucscpp::load_demucs_model (modelFile().getFullPathName().toStdString(), m.get()))
        return "The separation model didn't load (" + modelFile().getFullPathName() + ")";
    if (m->is_4sources)
        return "The separation model has four sources, not six";
    model = std::move (m);
    return {};
}

std::vector<float> GuitarSeparator::separate (const std::vector<float>& x, const std::atomic<bool>& cancel, const ProgressFn& progress,
                                              juce::String& error, int threads)
{
    if (const auto e = load(); e.isNotEmpty())
    {
        error = e;
        return {};
    }
    if (x.empty())
    {
        error = "Nothing to separate";
        return {};
    }

    const auto x441 = resample (x, 48000.0, modelSampleRate);
    const auto total = (int) x441.size();

    // Parts of 10 s, each with 0.75 s of context on both sides, crossfaded back together with triangular
    // weights normalized by their sum (the scheme of demucs.cpp's multithreaded driver,
    // cli-apps/threaded_inference.hpp, written for this use), run by a few workers in parallel. Short parts
    // because demucs.cpp's time grows faster than the length (measured: a 60 s part took 532 s, six 10 s
    // ones a fraction of that), and a few workers because each holds about 3 GB while it runs: the default
    // leaves 6 GB of the machine's memory free, at most 4 workers and half the cores.
    if (threads <= 0)
    {
        const auto gb = juce::SystemStats::getMemorySizeInMegabytes() / 1024;
        threads = juce::jlimit (1, juce::jmax (1, juce::jmin (4, (int) std::thread::hardware_concurrency() / 2)), (gb - 6) / 3);
    }
    const auto overlap = (int) (0.75 * modelSampleRate);
    const auto partLength = (int) (partSeconds * modelSampleRate);
    const auto numParts = juce::jmax (1, (total + partLength - 1) / partLength);
    threads = juce::jlimit (1, numParts, threads);

    std::vector<std::vector<float>> guitars ((size_t) numParts);
    std::vector<int> starts ((size_t) numParts), lengths ((size_t) numParts);
    std::vector<std::atomic<float>> partProgress ((size_t) numParts);
    std::atomic<bool> failed { false };
    std::atomic<int> nextPart { 0 };
    std::mutex reportLock; // workers only: progress comes from one of them at a time

    auto runPart = [&] (int t) {
        const auto core = t * partLength;
        const auto a = juce::jmax (0, core - overlap), b = juce::jmin (total, core + partLength + overlap);
        starts[(size_t) t] = a;
        lengths[(size_t) t] = b - a;
        Eigen::MatrixXf audio (2, b - a);
        for (int i = a; i < b; ++i)
            audio (0, i - a) = audio (1, i - a) = x441[(size_t) i];
        auto callback = [&, t] (float p, const std::string&) {
            partProgress[(size_t) t] = p;
            if (cancel.load())
                throw Cancelled {};
            if (progress)
            {
                float sum = 0.0f;
                for (auto& v : partProgress)
                    sum += v.load();
                const std::lock_guard<std::mutex> l (reportLock);
                progress (juce::jlimit (0.0, 0.97, (double) sum / numParts), "Separating the guitar");
            }
        };
        try
        {
            const auto out = demucscpp::demucs_inference (*model, audio, callback);
            auto& g = guitars[(size_t) t];
            g.resize ((size_t) (b - a));
            for (int i = 0; i < b - a; ++i)
                g[(size_t) i] = 0.5f * (out (guitarSource, 0, i) + out (guitarSource, 1, i));
            partProgress[(size_t) t] = 1.0f;
        }
        catch (...)
        {
            failed = true; // Cancelled, or anything demucs.cpp throws
        }
    };

    std::vector<std::thread> pool;
    for (int w = 0; w < threads; ++w)
        pool.emplace_back ([&] {
            for (int t = nextPart++; t < numParts && ! failed && ! cancel.load(); t = nextPart++)
                runPart (t);
        });
    for (auto& th : pool)
        th.join();

    if (cancel.load())
    {
        error = "Cancelled";
        return {};
    }
    if (failed)
    {
        error = "The separation failed";
        return {};
    }

    std::vector<double> sum ((size_t) total, 0.0), weight ((size_t) total, 0.0);
    for (int t = 0; t < numParts; ++t)
    {
        const auto n = lengths[(size_t) t];
        for (int i = 0; i < n; ++i)
        {
            const auto w = (double) std::min (i + 1, n - i); // triangular
            sum[(size_t) (starts[(size_t) t] + i)] += w * guitars[(size_t) t][(size_t) i];
            weight[(size_t) (starts[(size_t) t] + i)] += w;
        }
    }
    std::vector<float> guitar441 ((size_t) total);
    for (size_t i = 0; i < guitar441.size(); ++i)
        guitar441[i] = (float) (sum[i] / std::max (weight[i], 1.0e-12));

    auto out = resample (guitar441, modelSampleRate, 48000.0);
    out.resize (x.size(), 0.0f);
    if (progress)
        progress (1.0, "Separated");
    return out;
}

} // namespace ampsim::tonematch
