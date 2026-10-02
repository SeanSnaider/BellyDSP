# DSP Reference

Glossary and sources for every algorithm in the project. When implementing a block, start from the source listed here.

## Glossary

**Sample rate (fs).** Samples per second, usually 48000. Everything with a frequency or a time in it depends on this, so blocks receive it in `prepare()` and never hardcode it.

**Nyquist frequency.** Half the sample rate. The highest frequency a digital signal can represent. Anything above it folds back down as a false lower frequency.

**Aliasing.** That fold-back. Nonlinear processing like clipping creates harmonics above Nyquist, which alias into harsh inharmonic noise. Fixed by oversampling.

**Oversampling.** Temporarily raising the sample rate (upsample, process, lowpass, downsample) so generated harmonics have room above the audible range and get filtered out before coming back down.

**Biquad.** A second-order IIR filter, the workhorse of audio EQ. Five coefficients, two samples of memory. Lowpass, highpass, peaking, and shelving filters are all biquads with different coefficients.

**Transfer function, H(s) and H(z).** The frequency response of a system as a ratio of polynomials. H(s) is the analog (continuous) version, H(z) the digital one.

**Bilinear transform.** A standard way to convert an analog H(s) into a digital H(z), by substituting s = 2fs(z-1)/(z+1). It preserves stability but warps high frequencies slightly (prewarping corrects this at one chosen frequency).

**Impulse response (IR).** A system's output when fed a single-sample click. For a linear system like a speaker cab, the IR completely describes it, and convolving any signal with the IR reproduces that system.

**Convolution.** The operation that applies an IR to a signal. Direct convolution is expensive; FFT convolution is much cheaper.

**Envelope follower.** Tracks the loudness of a signal over time, with separate attack and release speeds. The core of gates, compressors, sag, and ducking.

**LFO.** Low-frequency oscillator, typically 0.1 to 10 Hz. Drives the sweep in chorus, phaser, and flanger.

**Fractional delay.** Delaying by a non-integer number of samples, via interpolation between samples. Needed for any smoothly modulated delay line (chorus, flanger, vibrato).

**Allpass filter.** Passes all frequencies at equal level but shifts their phase. Chains of them make phasers and reverb diffusion.

**Latency.** Delay between input and output. Comes from buffer size plus anything that needs to look ahead (FFT blocks, pitch detection windows).

**Dilated causal convolution.** A 1D convolution that only looks at past samples (causal) and skips samples at a fixed spacing (dilation), so stacking layers with dilations 1, 2, 4, 8... sees a long history cheaply. The backbone of NAM's WaveNet models. In real time, each layer keeps a ring buffer of its past inputs.

**Minimum phase.** Of all filters with the same magnitude response, the minimum-phase one has its energy packed as early as possible. Useful because two minimum-phase IRs can be interpolated without the phase cancellation that makes crossfaded IRs sound hollow.

**Comb filtering.** Summing a signal with a delayed copy of itself, which cancels a regular series of frequencies. Sounds hollow or phasey. The reason mic phase alignment matters.

**Zipper noise.** Clicks and stepping heard when a parameter jumps between values instead of ramping. Fixed with parameter smoothing.

## Sources by block

| Block | Algorithm | Source |
|---|---|---|
| Amps | NAM inference (WaveNet A1/A2, LSTM, and more), `.nam` format. The engine is NeuralAmpModelerCore itself (`third_party/NeuralAmpModelerCore`, read `NAM/wavenet/model.cpp` and `NAM/conv1d.cpp`) | NeuralAmpModelerCore docs (neuralampmodelercore.readthedocs.io), especially the WaveNet computation walkthrough and the .nam file versions page; the neural-amp-modeler Python trainer source; van den Oord et al., "WaveNet: A Generative Model for Raw Audio" (2016) for the dilated causal convolution idea |
| EQ, amp filters | Biquad filter formulas | Robert Bristow-Johnson, "Audio EQ Cookbook" (widely mirrored online, including the W3C Audio EQ Cookbook page) |
| EQ filters | Trapezoidal state-variable filter (TPT SVF) | Andrew Simper, Cytomic technical papers on linear trapezoidal SVFs; Vadim Zavalishin, "The Art of VA Filter Design" (free from Native Instruments) |
| Graphic EQ | Accurate cascade graphic equalizer with band interaction compensation | Juho Liski and Vesa Välimäki, "The Quest for the Best Graphic Equalizer" (DAFx 2017) and Välimäki and Liski, "Accurate Cascade Graphic Equalizer", IEEE Signal Processing Letters 2017 |
| Tone stack | Fender/Marshall FMV tone stack, nodal analysis + bilinear transform | David Yeh and Julius O. Smith, "Discretization of the '59 Fender Bassman Tone Stack", DAFx 2006 |
| Tube stages | Gray-box waveshaping; deeper option: tube triode models | Julius O. Smith's online books at CCRMA (ccrma.stanford.edu/~jos) for filter and waveshaping theory; Norman Koren's triode model for circuit-level work |
| Circuit modeling (optional deep end) | Wave Digital Filters | Kurt Werner's PhD thesis (Stanford, 2016) on WDFs for audio circuits |
| Cab | Uniformly partitioned FFT convolution with zero latency (current: JUCE's `dsp::Convolution`); hybrid direct head + partitioned FFT tail if CPU demands it | William G. Gardner, "Efficient Convolution without Input-Output Delay", JAES 1995; Frank Wefers, "Partitioned Convolution Algorithms for Real-Time Auralization" (2015); JUCE's `juce_Convolution.cpp` for the implementation in use |
| Cab mic morphing | Minimum-phase reconstruction via the real cepstrum; spectral interpolation | Oppenheim and Schafer, "Discrete-Time Signal Processing", chapter on cepstrum analysis and minimum-phase systems; Julius O. Smith, "Spectral Audio Signal Processing" (CCRMA online) |
| Mic alignment | Cross-correlation for time-delay estimation | Any DSP text; computed via FFT for speed |
| Loudness matching | K-weighted, gated integrated loudness (LUFS); white-noise IR matching computed from the K-weighted IR energy (Parseval) | ITU-R BS.1770-4, "Algorithms to measure audio programme loudness and true-peak audio level"; EBU Tech 3341 test signals; libebur128 for the analog-derived K-weighting formulas at any sample rate |
| Gates | Two-threshold (hysteresis) gate with hold, raised-cosine attack, and release in dB per second; adaptive release from a fast and a slow follower; Learn from a noise-floor percentile | Udo Zölzer (ed.), "DAFX: Digital Audio Effects", 2nd ed. (2011), ch. 4 (dynamics); Joshua Reiss and Andrew McPherson, "Audio Effects: Theory, Implementation and Application" (2014), the dynamics chapter; detector, decision, and smoothing split as in Giannoulis, Massberg, and Reiss (JAES 2012). The adaptive mapping and Learn are this project's own, settled in `prototypes/gate.py --study` |
| Compressor | Feed-forward log-domain compressor with soft knee and branching smoothing; feedback (pedal) topology | Dimitrios Giannoulis, Michael Massberg, Joshua Reiss, "Digital Dynamic Range Compressor Design: A Tutorial and Analysis", JAES 60(6), 2012 |
| Fractional delay reads | 4-point, 3rd-order Hermite interpolation | Olli Niemitalo, "Polynomial Interpolators for High-Quality Resampling of Oversampled Audio" (2001) |
| Low and high cuts | Butterworth filters as cascaded second-order sections, Q_k = 1 / (2 sin((2k - 1) pi / 2N)) | Any filter design text, e.g. Zavalishin, "The Art of VA Filter Design", or Smith, "Introduction to Digital Filters" (CCRMA online) |
| Reverb | FDN, Dattorro plate, Freeverb (prototype only) | Jean-Marc Jot and Antoine Chaigne, "Digital Delay Networks for Designing Artificial Reverberators" (AES 1991); Jon Dattorro, "Effect Design, Part 1: Reverberator and Other Filters" (JAES 1997); Julius O. Smith, "Physical Audio Signal Processing" (CCRMA online) for Schroeder/Freeverb and FDN theory |
| Reverb testing | Energy decay curve via Schroeder backward integration | Manfred Schroeder, "New Method of Measuring Reverberation Time" (JASA 1965) |
| Chorus, flanger, delay | Modulated delay lines with interpolation | Udo Zölzer (ed.), "DAFX: Digital Audio Effects" |
| Phaser | Swept allpass chains of TPT one-poles; the delay-free feedback loop solved in closed form | Zölzer, "DAFX"; Vadim Zavalishin, "The Art of VA Filter Design" (ch. 3, one-poles; zero-delay feedback); Sedra and Smith's JFET chapter for the square-law ohmic region behind Classic's sweep |
| Vibe lamp and photocell | Incandescent lamp thermal lag and power law, CdS photocell gamma and asymmetric response | Lamp engineering rules of thumb (power ~ V^1.55, light ~ V^3.4); CdS photocell and Vactrol datasheets (gamma, rise and decay times); `prototypes/vibe.py` has the model and its study |
| Bitcrusher | Mid-tread quantization, sample and hold, TPDF dither | Stanley Lipshitz, Robert Wannamaker, John Vanderkooy, "Quantization and Dither: A Theoretical Survey", JAES 1992 |
| Overdrive, boost | Physically informed distortion pedal models: linear stages from the schematic's transfer functions (bilinear transform), nonlinear stages as per-sample trapezoidal/Newton solutions of the circuit equations | David T. Yeh, "Digital Implementation of Musical Distortion Circuits by Analysis and Simulation", PhD thesis, Stanford (CCRMA), 2009; D. T. Yeh, J. S. Abel, J. O. Smith, "Simplified, Physically-Informed Models of Distortion and Overdrive Guitar Effects Pedals", DAFx-07 (the Tube Screamer's clipping ODE, eq. 22, and tone stage, eq. 24 and Fig. 15); ElectroSmash circuit analyses of the Tube Screamer, Klon, and RAT (component values and parts lists) |
| Op-amp macromodel (RAT) | A differential pair's tanh into the compensation capacitor: slew rate It/Cc and gain-bandwidth It/(2 VT 2 pi Cc) from one mechanism | G. R. Boyle, D. O. Pederson, B. M. Cohn, J. E. Solomon, "Macromodeling of Integrated Circuit Operational Amplifiers", IEEE JSSC 9(6), 1974; National Semiconductor LM108/LM208/LM308 datasheet (compensation, DC gain, output swing) |
| Drive aliasing (next step) | Antiderivative antialiasing for stateful systems | Martin Holters, "Antiderivative Antialiasing for Stateful Systems", DAFx-19 |
| Oversampling | Polyphase IIR halfband filters | Laurent de Soras, HIIR library and its accompanying notes; R. A. Valenzuela, A. G. Constantinides, "Digital Signal Processing Schemes for Efficient Interpolation and Decimation", IEE Proceedings 130(6), 1983 |
| Circuit validation | SPICE-style simulation: modified nodal analysis, trapezoidal companion models, Newton-Raphson with junction voltage limiting (pnjlim) | Laurence W. Nagel, "SPICE2: A Computer Program to Simulate Semiconductor Circuits", UC Berkeley ERL-M520, 1975; the SPICE3 source (devsup.c, DEVpnjlim); ngspice documentation |
| Tuner, harmonizer detection | McLeod Pitch Method (the NSDF, key maxima, parabolic interpolation) for the coarse, octave-safe estimate; band-pass plus interpolated zero crossings for the tuner's fine reading. YIN was the alternative the study compared it with | Philip McLeod and Geoff Wyvill, "A Smarter Way to Find Pitch", ICMC 2005; Alain de Cheveigné and Hideki Kawahara, "YIN, a fundamental frequency estimator for speech and music", JASA 2002; Philip McLeod and Geoff Wyvill, "A Smarter Way to Find Pitch" (ICMC 2005) for the McLeod Pitch Method |
| Pitch shifting | Granular delay-line shifting and PSOLA | Zölzer, "DAFX", time-segment processing chapter (covers both); Eric Moulines and Francis Charpentier, "Pitch-synchronous waveform processing techniques for text-to-speech synthesis using diphones" (Speech Communication 1990) for the original PSOLA |
| Granular splice alignment | Choosing each splice's jump by normalized cross-correlation (waveform similarity) | Werner Verhelst and Marc Roelands, "An overlap-add technique based on waveform similarity (WSOLA) for high quality time-scale modification of speech" (ICASSP 1993) |
| Crossfades between partly correlated signals | Gains normalized by the signals' correlation so the power stays constant | Marco Fink, Martin Holters, and Udo Zölzer, "Signal-matched power-complementary cross-fading and dry-wet mixing" (DAFx 2016) |
| General | Plugin DSP engineering in practice | Will Pirkle, "Designing Audio Effect Plugins in C++" (C++ but the DSP translates directly) |
