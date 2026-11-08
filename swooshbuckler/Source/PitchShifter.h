#pragma once

#include <juce_dsp/juce_dsp.h>

#include <complex>
#include <memory>
#include <vector>

/**
 * Real-time mono pitch shifter: changes the pitch, keeps the duration. One per track,
 * used when the track's VARI switch is off (VARI on is plain varispeed, pitch and speed
 * together, and does not come through here).
 *
 * STFT phase vocoder: Hann / Hann, hop = N / 4, N = 2048 at 44.1/48 kHz, scaled with the
 * rate like SpectralMorph (4096 at 88.2/96, 8192 at 176.4/192 kHz) so a frame is always
 * ~43 ms. Latency = N samples (2048 at 48 kHz, 4096 at 96 kHz), whatever the ratio.
 *
 * How the shift is done (Laroche & Dolson, "New phase-vocoder techniques for real-time
 * pitch shifting", 1999):
 *  - each analysis spectrum is cut into peak regions (a local maximum and the bins down to
 *    the dips either side of it);
 *  - the true frequency of every peak is measured from its phase advance since the last
 *    frame, and its whole region is moved to that frequency times the ratio (bin k goes to
 *    about k * ratio; regions that land on each other add up, anything past Nyquist is
 *    dropped);
 *  - every bin of a region is turned by the same phase rotation, which advances by exactly
 *    what the moved partial needs each hop. That is identity phase locking: the bins round
 *    a peak keep their analysis phase relationships, so tones stay clean instead of phasey.
 *    The rotation of a peak is handed on to whichever peak owns its bin in the next frame.
 *
 * Attacks stay sharp: a jump in spectral flux marks an onset; while that onset is inside
 * the frame (4 hops) the rotations are reset to the analysis phases, aligned so that all
 * the moved regions are in phase at the onset sample. A click comes out as a click.
 *
 * Ratio 1 (within 1e-4) is a bypass: the output is the input through a plain delay of
 * exactly N samples, so an unshifted track is untouched. The vocoder keeps running quietly
 * underneath; on the way back to 1 its rotations are eased back to zero (which makes its
 * output identical to the delay) before the bypass takes over, and every switch is a
 * one-hop crossfade, so nothing clicks. The ratio itself is smoothed per frame (~30 ms)
 * so pitch moves glide.
 *
 * Allocation-free and lock-free after prepare(). No JUCE GUI dependency.
 */
class PitchShifter
{
public:
    /** Allocates. Message thread / prepareToPlay. */
    void prepare(double sampleRate);

    /** Clears all state, no allocation. */
    void reset();

    /** As reset(), starting at ratio (so it begins in bypass at 1, or shifting otherwise), with its frame boundary
        hopPhase (0..1) of a hop in. Several shifters given different phases do their FFT work in different blocks
        instead of all at once. The latency is the same whatever the phase. */
    void reset(float ratio, float hopPhase);

    /** N: constant for a given sample rate, independent of the ratio. */
    int getLatencySamples() const { return m_fftSize; }

    /**
     * ratio = frequency ratio (2 = up an octave, 0.5 = down an octave), clamped to 0.25..4.
     * in and out may be the same pointer; numSamples may be anything >= 0.
     */
    void process(const float* in, float* out, int numSamples, float ratio);

private:
    using Cpx = std::complex<float>;

    void processFrame();

    double m_sampleRate = 48000.0;
    int m_fftOrder = 11, m_fftSize = 2048, m_hop = 512, m_bins = 1025;
    std::unique_ptr<juce::dsp::FFT> m_fft;

    std::vector<float> m_window, m_in, m_outAcc, m_outFifo, m_work, m_delay;
    int m_pos = 0, m_delayPos = 0;

    std::vector<Cpx> m_spec, m_prevSpec, m_out;
    std::vector<float> m_mag, m_prevMag, m_theta, m_prevTheta;
    std::vector<int> m_owner, m_prevOwner, m_peaks, m_regionLo, m_regionHi;

    float m_targetRatio = 1.0f;   // last ratio handed to process()
    float m_ratio = 1.0f;         // smoothed, per frame
    float m_ratioCoeff = 0.3f;    // one-pole per frame, in log ratio
    int m_settle = 0;             // frames spent at exactly ratio 1 (for the bypass hand-over)
    int m_transientPos = -1;      // onset position inside the current frame, -1 when none
    float m_mix = 0.0f;           // 0 = delay (bypass), 1 = vocoder
    float m_mixTarget = 0.0f;
    float m_mixStep = 1.0f / 512.0f;
};
