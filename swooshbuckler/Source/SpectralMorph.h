#pragma once

#include <juce_dsp/juce_dsp.h>

#include <complex>
#include <memory>
#include <vector>

/**
 * Real-time spectral morph between two mono sources (A and B), in the spirit of
 * Zynaptiq MORPH (whose algorithms are proprietary: this recreates the
 * behaviour from published techniques, see docs/experiments/06-morph.md).
 *
 * STFT: Hann / Hann, hop = N / 4, N = 2048 at 44.1/48 kHz, scaled with the rate
 * (4096 at 96, 8192 at 192 kHz) so the frame is always ~43 ms. Latency = N.
 *
 * Modes (x = 0 is A, x = 1 is B in every mode; y is a second dimension per mode):
 *   Blend : spectral-envelope morph. Each frame is split into a true envelope
 *           (iterated cepstral smoothing) and a whitened fine structure. The
 *           envelopes are interpolated in log magnitude (formants slide), the fine
 *           structures are mixed with a power mean whose exponent y sets
 *           y = 0 "Mix"  : power mix (both pitch structures stay audible)
 *           y = 1 "Fuse" : log/geometric mix (one fused hybrid texture)
 *   Glide : displacement interpolation (audio-transport style, Henderson &
 *           Solomon, DAFx 2019). Each spectrum is cut into peak-centred masses,
 *           the masses are matched by 1-D optimal transport and moved to the
 *           interpolated frequency with phase-locked resynthesis, so partials
 *           glide instead of crossfading. y: how much the in-between spectral
 *           envelope is imposed on the moved partials
 *           y = 0 "Move" : formants travel with the partials (pure transport)
 *           y = 1 "Shape": formants morph in place (envelope re-imposed)
 *   Cross : cross-synthesis / vocoding. x travels A -> hybrid -> B; y chooses the
 *           hybrid in the middle
 *           y = 1 "A thru B" : A's fine structure through B's envelope
 *           y = 0 "B thru A" : B's fine structure through A's envelope
 *           y = 0.5          : the plain diagonal morph
 *
 * Under the hood (no UI needed):
 *  - every parameter is smoothed per frame (~40 ms), mode changes crossfade over
 *    6 frames, so fast x/y moves and mode switches are click-free;
 *  - level matching: each output frame is scaled to the log-interpolated energy
 *    of the inputs, so there is no level dip mid-morph;
 *  - silence handling: each input has a smoothed presence gate (-75..-60 dBFS);
 *    the effective x is pulled towards the input that is present, so a silent A
 *    gives B and vice versa; both silent gives silence;
 *  - amount (automation only, default 1) blends the morph with a plain
 *    equal-power crossfade of the two inputs.
 *
 * Allocation-free and lock-free after prepare(). No JUCE GUI dependency.
 * setParams() and process() are meant to be called from the same (audio) thread.
 */
class SpectralMorph
{
public:
    enum Mode { Blend = 0, Glide, Cross, NumModes };

    struct Params
    {
        float x = 0.5f;      // 0 = source A, 1 = source B
        float y = 0.5f;      // mode axis (see above)
        int mode = Blend;
        float amount = 1.0f; // 0 = plain crossfade, 1 = full morph
    };

    void prepare(double sampleRate, int maxBlock);
    void reset();

    void setParams(const Params& p) { m_target = p; }
    const Params& getParams() const { return m_target; }

    /** a, b may be null (treated as silence). out may alias a or b (each sample is read before it is written). */
    void process(const float* a, const float* b, float* out, int n);

    int getLatencySamples() const { return m_fftSize; }
    int getFftSize() const { return m_fftSize; }

    static const char* modeName(int mode);

private:
    using Cpx = std::complex<float>;

    struct Mass
    {
        int lo = 0, hi = 0, peak = 0;
        float freq = 0.0f;   // instantaneous frequency of the peak, in bins
        float weight = 0.0f; // normalised (sums to 1 over a spectrum)
    };

    void processFrame();
    void analyse(const std::vector<float>& in, std::vector<Cpx>& spec, std::vector<float>& mag,
                 std::vector<float>& phase, std::vector<float>& logMag, float& energy, float& rms);
    void trueEnvelope(const float* logMag, float* env, int iterations);
    void cepstralSmooth(const float* in, float* out);

    void renderMode(int mode, float xe, float y, std::vector<Cpx>& dest);
    void renderBlend(float xe, float p, float envW, float fineW, std::vector<Cpx>& dest);
    void renderGlide(float xe, float y, std::vector<Cpx>& dest);
    int findMasses(const std::vector<float>& mag, const std::vector<float>& phase,
                   const std::vector<float>& prevPhase, std::vector<Mass>& masses);

    double m_sampleRate = 48000.0;
    int m_fftOrder = 11, m_fftSize = 2048, m_hop = 512, m_bins = 1025, m_lifter = 58;
    std::unique_ptr<juce::dsp::FFT> m_fft;

    std::vector<float> m_window, m_inA, m_inB, m_outAcc, m_outFifo, m_work;
    int m_pos = 0;

    std::vector<Cpx> m_specA, m_specB, m_out, m_tmp;
    std::vector<float> m_magA, m_magB, m_phA, m_phB, m_prevPhA, m_prevPhB, m_logA, m_logB, m_envA, m_envB;
    std::vector<float> m_scratch, m_scratch2, m_envT, m_outPhase;
    std::vector<Mass> m_massA, m_massB;

    Params m_target;
    float m_x = 0.5f, m_y = 0.5f, m_amount = 1.0f;
    float m_gateA = 0.0f, m_gateB = 0.0f;

    // Slow loudness trim: compares the emitted output with the (latency-delayed) target level and
    // corrects the residual dip left by phase interaction in overlap-add (mainly Glide).
    float m_targetHistory[4] {};
    int m_historyPos = 0;
    float m_powTarget = 0.0f, m_powOut = 0.0f, m_trim = 1.0f, m_trimCoeff = 0.05f;
    float m_paramCoeff = 0.25f, m_gateAttack = 0.5f, m_gateRelease = 0.1f;
    int m_mode = Blend, m_prevMode = Blend;
    float m_modeFade = 1.0f;
    bool m_first = true;
};
