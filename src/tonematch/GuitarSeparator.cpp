// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Sean Snaider

#include "GuitarSeparator.h"

#include "AudioFileInput.h"

#include "../platform/AppInfo.h"

#include <juce_cryptography/juce_cryptography.h>

#include <model.hpp>

#include <cmath>
#include <mutex>
#include <new>
#include <thread>

#if JUCE_MAC
 #include <mach/mach.h>
#endif
#if JUCE_MAC || JUCE_LINUX
 #include <sys/resource.h>
#endif

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

juce::String megabytes (juce::int64 bytes)
{
    return juce::String ((double) bytes / 1.0e6, 1) + " MB";
}

/// This process's peak memory so far, for the log (0 where it isn't known).
juce::int64 peakMemoryMB()
{
   #if JUCE_MAC || JUCE_LINUX
    rusage r {};
    getrusage (RUSAGE_SELF, &r);
   #if JUCE_MAC
    return (juce::int64) r.ru_maxrss / (1024 * 1024); // bytes on macOS
   #else
    return (juce::int64) r.ru_maxrss / 1024; // kilobytes on Linux
   #endif
   #else
    return 0;
   #endif
}

/// The standard reason phrase for the statuses a download is likely to meet.
juce::String reasonFor (int status)
{
    switch (status)
    {
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 410: return "Gone";
        case 416: return "Range Not Satisfiable";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return {};
    }
}

/// Waits ms, or less if cancelled. True if cancelled.
bool waitOrCancel (int ms, const std::atomic<bool>& cancel)
{
    for (int waited = 0; waited < ms; waited += 50)
    {
        if (cancel.load())
            return true;
        juce::Thread::sleep (50);
    }
    return cancel.load();
}

std::mutex& logLock()
{
    static std::mutex m;
    return m;
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

// ---- The log ----------------------------------------------------------------------------------------------------

void GuitarSeparator::log (const juce::String& message) const
{
    const std::lock_guard<std::mutex> l (logLock());
    const auto file = logFile();
    if (! file.getParentDirectory().createDirectory())
        return;
    if (file.getSize() > maxLogBytes)
    {
        const auto old = file.getSiblingFile ("separation-log.old.txt");
        old.deleteFile();
        file.moveFileTo (old);
    }
    juce::FileOutputStream out (file); // appends
    if (out.openedOk())
        out << juce::Time::getCurrentTime().toISO8601 (true) << "  " << message << "\n";
}

// ---- Installing the weights --------------------------------------------------------------------------------

juce::String GuitarSeparator::install (const juce::String& url, const std::atomic<bool>& cancel, const ProgressFn& progress)
{
    const auto logName = logFile().getFullPathName();
    auto fail = [&] (const juce::String& message) {
        log ("Install failed: " + message);
        return message + " (details: " + logName + ")";
    };

    log ("Installing the separation model from " + url + " into " + folder.getFullPathName());
    if (! folder.createDirectory())
        return fail ("couldn't create the folder " + folder.getFullPathName());

    const auto part = folder.getChildFile ("5c90dfd2.safetensors.part");
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    if (const auto e = download (url, part, cancel, progress); e.isNotEmpty())
    {
        if (e == "Cancelled")
        {
            part.deleteFile();
            log ("Install cancelled");
            return e;
        }
        // A partial download is kept, so the next try resumes it.
        return fail (e);
    }
    log ("Downloaded " + megabytes (part.getSize()) + " in " + juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) + " s");

    if (part.getSize() != weightsBytes)
    {
        const auto got = part.getSize();
        part.deleteFile();
        return fail ("the download was " + juce::String (got) + " bytes, not " + juce::String (weightsBytes) + "; nothing installed");
    }
    if (progress)
        progress (0.92, "Checking the download");
    if (const auto sha = sha256Of (part); sha != weightsSha256)
    {
        part.deleteFile();
        log ("SHA-256 " + sha + ", expected " + weightsSha256);
        return fail ("the downloaded model is damaged (its SHA-256 doesn't match the pinned one), so it was deleted; try again");
    }

    if (progress)
        progress (0.95, "Converting the model");
    const auto converted = folder.getChildFile ("htdemucs_6s.demucscpp.bin.part");
    if (const auto e = convert (part, converted); e.isNotEmpty())
    {
        part.deleteFile();
        converted.deleteFile();
        return fail ("couldn't convert the model: " + e);
    }
    part.deleteFile();
    modelFile().deleteFile();
    if (! converted.moveFileTo (modelFile()))
    {
        converted.deleteFile();
        return fail ("couldn't move the converted model to " + modelFile().getFullPathName());
    }
    log ("Installed " + modelFile().getFullPathName() + " (" + megabytes (modelFile().getSize()) + ") in "
         + juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) + " s");
    if (progress)
        progress (1.0, "Separation model installed");
    return {};
}

juce::String GuitarSeparator::download (const juce::String& url, const juce::File& part, const std::atomic<bool>& cancel, const ProgressFn& progress)
{
    // A download over HTTP(S) is tried up to downloadAttempts times. Each try asks for what's still missing
    // (Range: bytes=<have>-) and appends it, so a connection that drops 40 MB in loses nothing. A server
    // that ignores the Range header answers 200 with the whole file, which then replaces the partial one.
    // Every try's outcome goes in the log: the HTTP status, the redirected host, how many bytes came.
    // (JUCE's macOS stream reports a failed connection, a timeout, a TLS or DNS error alike, as no stream
    // at all, so "no response" is as specific as that case can be.)
    const juce::URL source (url);
    const auto isHttp = url.startsWithIgnoreCase ("http://") || url.startsWithIgnoreCase ("https://");
    const auto host = isHttp ? source.getDomain() : juce::String ("a local file");
    const auto attempts = isHttp ? juce::jmax (1, downloadAttempts) : 1;
    auto delay = retryDelayMs;
    juce::String lastProblem;

    for (int attempt = 1; attempt <= attempts; ++attempt)
    {
        if (cancel.load())
            return "Cancelled";
        if (attempt > 1)
        {
            log ("  waiting " + juce::String (delay) + " ms before try " + juce::String (attempt));
            if (waitOrCancel (delay, cancel))
                return "Cancelled";
            delay *= 2;
        }

        auto have = part.existsAsFile() ? part.getSize() : (juce::int64) 0;
        if (have > weightsBytes)
        {
            part.deleteFile();
            have = 0;
        }
        if (! isHttp) // a local file is read whole
        {
            part.deleteFile();
            have = 0;
        }

        int status = 0;
        juce::StringPairArray headers;
        const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withConnectionTimeoutMs (downloadTimeoutMs)
                                 .withNumRedirectsToFollow (10)
                                 .withStatusCode (&status)
                                 .withResponseHeaders (&headers)
                                 .withExtraHeaders (have > 0 ? "Range: bytes=" + juce::String (have) + "-" : juce::String());

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto in = source.createInputStream (options);
        const auto waited = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        juce::String tryText = "  try " + juce::String (attempt) + " of " + juce::String (attempts) + (have > 0 ? ", resuming at " + megabytes (have) : juce::String()) + ": ";

        if (in == nullptr)
        {
            lastProblem = status > 0 ? host + " answered HTTP " + juce::String (status) + " " + reasonFor (status)
                                     : "no response from " + host + " (offline, blocked, or no answer within " + juce::String (downloadTimeoutMs / 1000) + " s)";
            log (tryText + lastProblem + " after " + juce::String (waited, 1) + " s");
            continue;
        }

        if (isHttp && status != 200 && status != 206)
        {
            lastProblem = host + " answered HTTP " + juce::String (status) + (reasonFor (status).isNotEmpty() ? " " + reasonFor (status) : juce::String());
            log (tryText + lastProblem);
            const auto worthRetrying = status == 408 || status == 429 || status >= 500 || status == 416;
            if (status == 416)
                part.deleteFile(); // the partial file isn't a prefix of what's there: start again
            if (! worthRetrying)
                return "couldn't download the separation model: " + lastProblem;
            continue;
        }

        // 206 must continue exactly where the partial file ends; anything else starts from scratch.
        const auto contentRange = headers.getValue ("Content-Range", {});
        const auto resumes = status == 206 && have > 0 && contentRange.startsWithIgnoreCase ("bytes " + juce::String (have) + "-");
        if (! resumes)
        {
            part.deleteFile();
            have = 0;
        }
        log (tryText + (isHttp ? "HTTP " + juce::String (status) : juce::String ("local file")) + " after " + juce::String (waited, 1) + " s, "
             + (resumes ? "resuming (" + contentRange + ")" : "from the start") + ", length " + juce::String (in->getTotalLength()));

        juce::FileOutputStream out (part); // appends to what's there
        if (! out.openedOk())
            return "couldn't save the download to " + part.getFullPathName() + " (" + out.getStatus().getErrorMessage() + ")";

        std::vector<char> buffer (1 << 16);
        auto done = have;
        for (;;)
        {
            if (cancel.load())
                return "Cancelled";
            const auto n = in->read (buffer.data(), (int) buffer.size());
            if (n <= 0)
                break;
            if (! out.write (buffer.data(), (size_t) n))
                return "couldn't save the download to " + part.getFullPathName() + " (is the disk full?)";
            done += n;
            if (progress)
                progress (0.9 * juce::jmin (1.0, (double) done / (double) weightsBytes),
                          "Downloading the separation model (" + juce::String (done / 1000000) + " of " + juce::String (weightsBytes / 1000000) + " MB)");
        }
        out.flush();
        if (out.getStatus().failed())
            return "couldn't save the download to " + part.getFullPathName() + " (" + out.getStatus().getErrorMessage() + ")";

        const auto size = part.getSize();
        if (size == weightsBytes || ! isHttp)
            return {};
        lastProblem = size < weightsBytes ? "the connection dropped after " + megabytes (size) + " of " + megabytes (weightsBytes)
                                          : "the server sent " + juce::String (size) + " bytes, more than the model's " + juce::String (weightsBytes);
        log (tryText + lastProblem);
    }
    return "couldn't download the separation model after " + juce::String (attempts) + (attempts == 1 ? " try: " : " tries: ") + lastProblem;
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
        return "the separation model isn't installed";
    auto m = std::make_unique<demucscpp::demucs_model>();
    bool loaded = false;
    try
    {
        loaded = demucscpp::load_demucs_model (modelFile().getFullPathName().toStdString(), m.get());
    }
    catch (const std::bad_alloc&)
    {
        log ("Loading the model ran out of memory");
        return "not enough memory to load the separation model (about 300 MB)";
    }
    if (! loaded || m->is_4sources)
    {
        log ("The model file " + modelFile().getFullPathName() + " (" + megabytes (modelFile().getSize()) + ") didn't load"
             + (m->is_4sources ? " (it has four sources, not six)" : "") + "; deleted, so the next try downloads it again");
        modelFile().deleteFile();
        return "the installed separation model was damaged; it was deleted, and the next try downloads it again";
    }
    model = std::move (m);
    return {};
}

juce::int64 GuitarSeparator::physicalMemoryMB()
{
    return (juce::int64) juce::SystemStats::getMemorySizeInMegabytes();
}

juce::int64 GuitarSeparator::availableMemoryMB()
{
   #if JUCE_MAC
    // What the kernel can hand out without swapping: free pages, plus inactive, purgeable, and file-backed
    // (cache) pages, which it reclaims on demand. (Activity Monitor's "Memory Used" is roughly physical
    // memory minus this.)
    vm_statistics64_data_t vm {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    if (host_page_size (mach_host_self(), &pageSize) == KERN_SUCCESS
        && host_statistics64 (mach_host_self(), HOST_VM_INFO64, (host_info64_t) &vm, &count) == KERN_SUCCESS)
    {
        const auto pages = (juce::int64) vm.free_count + vm.inactive_count + vm.purgeable_count + vm.external_page_count;
        return juce::jmin (physicalMemoryMB(), pages * (juce::int64) pageSize / (1024 * 1024));
    }
   #endif
    return physicalMemoryMB();
}

int GuitarSeparator::chooseWorkers (juce::int64 physicalMB, juce::int64 availableMB, int cores)
{
    // Each worker needs about workerMegabytes. The budget is memoryBudget of the machine (so a separation
    // never takes most of it, whatever else is open), and never more than is available now less a
    // reserve for the app and the system. At least one worker always runs: if even that doesn't fit, it
    // runs anyway and a failed allocation is reported (separate() catches it).
    const auto byBudget = (juce::int64) (memoryBudget * (double) physicalMB) / workerMegabytes;
    const auto byAvailable = (availableMB - reserveMegabytes) / workerMegabytes;
    const auto byCores = juce::jmax (1, cores / 2);
    return (int) juce::jlimit ((juce::int64) 1, (juce::int64) juce::jmin (maxWorkers, byCores), juce::jmin (byBudget, byAvailable));
}

std::vector<float> GuitarSeparator::separate (const std::vector<float>& left, const std::vector<float>& right, const std::atomic<bool>& cancel,
                                              const ProgressFn& progress, juce::String& error, int workers)
{
    const auto logName = logFile().getFullPathName();
    if (left.empty())
    {
        error = "nothing to separate";
        return {};
    }
    if (const auto e = load(); e.isNotEmpty())
    {
        log ("Separation failed: " + e);
        error = e + " (details: " + logName + ")";
        return {};
    }

    const auto physical = physicalMemoryMB(), available = availableMemoryMB();
    const auto cores = (int) std::thread::hardware_concurrency();
    const auto chosen = workers > 0 ? workers : chooseWorkers (physical, available, cores);
    log ("Separating " + juce::String ((double) left.size() / 48000.0, 1) + " s (" + (right.empty() ? "mono" : "stereo") + "), "
         + "up to " + juce::String (chosen) + (workers > 0 ? " workers (asked for)" : " workers (" + juce::String (physical) + " MB physical, " + juce::String (available)
                                                                          + " MB available, " + juce::String (cores) + " cores)"));

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto left441 = resample (left, 48000.0, modelSampleRate);
    const auto right441 = right.empty() ? std::vector<float>() : resample (right, 48000.0, modelSampleRate);

    bool outOfMemory = false;
    auto out = run (left441, right441, cancel, progress, error, chosen, outOfMemory);
    if (out.empty() && outOfMemory && chosen > 1 && ! cancel.load())
    {
        // Out of memory with several workers: once more with one, which needs about a third as much.
        log ("  out of memory with " + juce::String (chosen) + " workers; trying again with 1");
        error.clear();
        outOfMemory = false;
        out = run (left441, right441, cancel, progress, error, 1, outOfMemory);
    }

    const auto seconds = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    if (out.empty())
    {
        if (outOfMemory)
            error = "not enough memory to separate: each worker needs about " + juce::String (workerMegabytes / 1000.0, 1)
                    + " GB, and even one didn't fit. Close other apps and try again";
        log (error == "Cancelled" ? juce::String ("Separation cancelled") : "Separation failed: " + error);
        if (error != "Cancelled")
            error << " (details: " << logName << ")";
        return {};
    }
    log ("  separated in " + juce::String (seconds, 1) + " s (" + juce::String (60.0 * seconds / ((double) left.size() / 48000.0), 1)
         + " s per minute of audio), " + juce::String (lastWorkers) + (lastWorkers == 1 ? " worker" : " workers") + ", peak memory of the process so far " + juce::String (peakMemoryMB()) + " MB");

    auto out48 = resample (out, modelSampleRate, 48000.0);
    out48.resize (left.size(), 0.0f);
    if (progress)
        progress (1.0, "Separated");
    return out48;
}

std::vector<float> GuitarSeparator::run (const std::vector<float>& left441, const std::vector<float>& right441, const std::atomic<bool>& cancel,
                                         const ProgressFn& progress, juce::String& error, int workers, bool& outOfMemory)
{
    const auto total = (int) left441.size();

    // Parts of 10 s, each with 0.75 s of context on both sides, crossfaded back together with triangular
    // weights normalized by their sum (the scheme of demucs.cpp's multithreaded driver,
    // cli-apps/threaded_inference.hpp, written for this use), run by a few workers in parallel. Short parts
    // because demucs.cpp's time grows faster than the length (measured: a 60 s part took 532 s, six 10 s
    // ones a fraction of that); a few workers because each holds about workerMegabytes while it runs.
    const auto overlap = (int) (0.75 * modelSampleRate);
    const auto partLength = (int) (partSeconds * modelSampleRate);
    const auto numParts = juce::jmax (1, (total + partLength - 1) / partLength);
    workers = juce::jlimit (1, numParts, workers);
    lastWorkers = workers;

    std::vector<std::vector<float>> guitars ((size_t) numParts);
    std::vector<int> starts ((size_t) numParts), lengths ((size_t) numParts);
    std::vector<std::atomic<float>> partProgress ((size_t) numParts);
    std::atomic<bool> failed { false }, ranOutOfMemory { false };
    std::atomic<int> nextPart { 0 }, running { 0 }, silentParts { 0 }, nonFinite { 0 };
    std::mutex reportLock; // workers only: progress and the first error come from one of them at a time
    juce::String firstError;
    const auto stage = "Separating the guitar (" + juce::String (numParts) + (numParts == 1 ? " part" : " parts")
                       + (workers > 1 ? ", " + juce::String (workers) + " at a time)" : juce::String (")"));

    auto runPart = [&] (int t) {
        const auto core = t * partLength;
        const auto a = juce::jmax (0, core - overlap), b = juce::jmin (total, core + partLength + overlap);
        starts[(size_t) t] = a;
        lengths[(size_t) t] = b - a;
        auto& g = guitars[(size_t) t];
        g.assign ((size_t) (b - a), 0.0f);

        // Demucs normalizes its input by the standard deviation over time of the channels' mean
        // (demucs_inference: (x - mean) / std). Digital silence has none, and the division makes every
        // output NaN, which the crossfade would then spread into the neighbouring parts. So a part
        // that's silent (or exactly constant) isn't run: its guitar is silence.
        double sum = 0.0, sumSquares = 0.0;
        for (int i = a; i < b; ++i)
        {
            const auto m = right441.empty() ? (double) left441[(size_t) i] : 0.5 * ((double) left441[(size_t) i] + right441[(size_t) i]);
            sum += m;
            sumSquares += m * m;
        }
        const auto n = (double) (b - a);
        const auto variance = sumSquares / n - (sum / n) * (sum / n);
        if (! (variance > 1.0e-12)) // also catches a NaN input
        {
            ++silentParts;
            partProgress[(size_t) t] = 1.0f;
            return;
        }

        auto callback = [&, t] (float p, const std::string&) {
            partProgress[(size_t) t] = p;
            if (cancel.load())
                throw Cancelled {};
            if (progress)
            {
                float done = 0.0f;
                for (auto& v : partProgress)
                    done += v.load();
                const std::lock_guard<std::mutex> l (reportLock);
                progress (juce::jlimit (0.0, 0.97, (double) done / numParts), stage);
            }
        };
        try
        {
            const auto concurrent = ++running;
            struct Leave { std::atomic<int>& r; ~Leave() { --r; } } leave { running };
            if (simulateOutOfMemoryAtWorkers > 0 && concurrent >= simulateOutOfMemoryAtWorkers)
                throw std::bad_alloc();

            Eigen::MatrixXf audio (2, b - a);
            for (int i = a; i < b; ++i)
            {
                audio (0, i - a) = left441[(size_t) i];
                audio (1, i - a) = right441.empty() ? left441[(size_t) i] : right441[(size_t) i];
            }
            const auto out = demucscpp::demucs_inference (*model, audio, callback);
            for (int i = 0; i < b - a; ++i)
            {
                const auto v = 0.5f * (out (guitarSource, 0, i) + out (guitarSource, 1, i));
                if (std::isfinite (v))
                    g[(size_t) i] = v;
                else
                    ++nonFinite;
            }
            partProgress[(size_t) t] = 1.0f;
        }
        catch (const Cancelled&)
        {
            failed = true;
        }
        catch (const std::bad_alloc&)
        {
            ranOutOfMemory = true;
            failed = true;
        }
        catch (const std::exception& e)
        {
            const std::lock_guard<std::mutex> l (reportLock);
            if (firstError.isEmpty())
                firstError = "the separation stopped with an error in part " + juce::String (t + 1) + ": " + juce::String (e.what());
            failed = true;
        }
        catch (...)
        {
            const std::lock_guard<std::mutex> l (reportLock);
            if (firstError.isEmpty())
                firstError = "the separation stopped with an unknown error in part " + juce::String (t + 1);
            failed = true;
        }
    };

    std::vector<std::thread> pool;
    try
    {
        for (int w = 0; w < workers; ++w)
            pool.emplace_back ([&] {
                for (int t = nextPart++; t < numParts && ! failed && ! cancel.load(); t = nextPart++)
                    runPart (t);
            });
    }
    catch (const std::system_error& e)
    {
        failed = true;
        firstError = "couldn't start the separation's workers (" + juce::String (e.what()) + ")";
    }
    for (auto& th : pool)
        th.join();

    if (silentParts > 0)
        log ("  " + juce::String (silentParts.load()) + " of " + juce::String (numParts) + " parts were digital silence (not run, their guitar is silence)");
    if (nonFinite > 0)
        log ("  " + juce::String (nonFinite.load()) + " non-finite output samples set to 0");

    if (cancel.load())
    {
        error = "Cancelled";
        return {};
    }
    if (failed)
    {
        outOfMemory = ranOutOfMemory.load();
        error = outOfMemory ? "ran out of memory with " + juce::String (workers) + (workers == 1 ? " worker" : " workers")
                            : (firstError.isNotEmpty() ? firstError : juce::String ("the separation failed"));
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
    return guitar441;
}

} // namespace ampsim::tonematch
