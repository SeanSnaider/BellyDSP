#include "Overdrive.h"

namespace ampsim
{

Overdrive::Overdrive()
{
    // In Mode order.
    engine.addCircuit (std::make_unique<drive::MidDriveCircuit>());
    engine.addCircuit (std::make_unique<drive::DistortionCircuit>());
    engine.addCircuit (std::make_unique<drive::TransparentCircuit>());
    engine.addCircuit (std::make_unique<drive::FuzzCircuit>());
}

void Overdrive::setSettings (const Settings& s)
{
    DriveEngine::Settings e;
    e.circuit = (int) s.mode;
    e.drive = s.drive;
    e.tone = s.tone;
    e.tightHz = s.tightHz;
    e.mix = s.mix;
    e.levelDb = s.levelDb;
    e.oversampling = s.oversampling;
    e.voltsAtFullScale = s.voltsAtFullScale;
    engine.setSettings (e);
}

void Overdrive::prepare (double sampleRate, int maxBlockSize) { engine.prepare (sampleRate, maxBlockSize); }

void Overdrive::process (juce::dsp::AudioBlock<float> block, const BlockContext&)
{
    engine.process (block.getChannelPointer (0), (int) block.getNumSamples());
}

void Overdrive::reset() { engine.reset(); }

} // namespace ampsim
