#pragma once

#include <juce_dsp/juce_dsp.h>
#include <memory>
#include <vector>
#include "FlattenerTypes.h"

namespace pf
{
    /**
     * A frame-wise pitch detector. Each call analyses one frame of
     * frameLength() analysis-rate samples and returns ranked candidates.
     *
     * prepare() (via the factory) allocates; analyse() must not.
     */
    class IPitchDetector
    {
    public:
        virtual ~IPitchDetector() = default;

        /** Number of analysis samples this detector looks at per frame (<= cfg.maxFrameLength). */
        virtual int frameLength() const = 0;

        /** sensitivity: 0..1, higher = more readily voiced. */
        virtual void analyse (const float* frame, float sensitivity, FrameObservation& out) = 0;

        /**
         * Analysis samples by which the frame should be shifted later so that the
         * detector's effective analysis centre lands on the frame centre.
         */
        virtual int timeBias() const { return 0; }

        const DetectorConfig& getConfig() const { return config; }

    protected:
        DetectorConfig config;
    };

    std::unique_ptr<IPitchDetector> createDetector (const DetectorConfig& cfg);

    //==========================================================================
    /** Small helper owning a juce FFT plus a scratch buffer, with known inverse scaling. */
    struct RealFFT
    {
        explicit RealFFT (int order);
        int size() const { return n; }
        /** In-place: data has 2*size() floats; on return holds interleaved complex bins 0..size()-1. */
        void forward (float* data) { fft.performRealOnlyForwardTransform (data); }
        /** In-place: data holds interleaved complex bins; on return data[0..size()) is the real signal * inverseScale. */
        void inverse (float* data) { fft.performRealOnlyInverseTransform (data); }
        /** Factor that inverse(forward(x)) applies to x. */
        float roundTripScale = 1.0f;

        juce::dsp::FFT fft;
        int n;
    };

    //==========================================================================
    /** probabilistic YIN (Mauch & Dixon 2014), frame stage. Candidates go to the shared tracker. */
    class PYinDetector final : public IPitchDetector
    {
    public:
        explicit PYinDetector (const DetectorConfig& cfg);
        int frameLength() const override { return frameLen; }
        int timeBias() const override { return bias; }
        void analyse (const float* frame, float sensitivity, FrameObservation& out) override;

    private:
        int bias = 0;
        void updatePrior (float sensitivity);

        int window = 0, tauMin = 2, tauMax = 0, frameLen = 0;
        std::unique_ptr<RealFFT> fft;
        std::vector<float> a, b;
        std::vector<double> prefix;
        std::vector<float> diff, cmnd;
        std::vector<float> thresholds, thresholdPrior;
        float priorForSensitivity = -1.0f;
    };

    /** McLeod Pitch Method (McLeod & Wyvill 2005): NSDF key maxima. */
    class MpmDetector final : public IPitchDetector
    {
    public:
        explicit MpmDetector (const DetectorConfig& cfg);
        int frameLength() const override { return window; }
        void analyse (const float* frame, float sensitivity, FrameObservation& out) override;

    private:
        int window = 0, tauMin = 2, tauMax = 0;
        std::unique_ptr<RealFFT> fft;
        std::vector<float> buf, nsdf;
    };

    /**
     * Multi-resolution spectral peak tracker for near-sinusoidal, fast-sweeping
     * sources: Gaussian-windowed STFTs at several lengths, quadratic peak
     * interpolation on the log magnitude refined by time-frequency
     * reassignment, salience against the local spectral floor, and a
     * harmonic-support check so the fundamental wins over its overtones.
     */
    class SpectralPeakDetector final : public IPitchDetector
    {
    public:
        explicit SpectralPeakDetector (const DetectorConfig& cfg);
        int frameLength() const override { return frameLen; }
        void analyse (const float* frame, float sensitivity, FrameObservation& out) override;

    private:
        struct Resolution
        {
            int length = 0;
            std::unique_ptr<RealFFT> fft;
            std::vector<float> win, dwin;   // Gaussian window and its time derivative
            std::vector<float> bufH, bufD;  // FFT work buffers
            std::vector<float> logMag, floorDb;
        };

        struct Peak { float hz; float salDb; int res; };

        float harmonicSupport (float hz, int res) const;
        float salienceAt (float hz, int res) const;

        std::vector<Resolution> res;
        int frameLen = 0;
        std::vector<Peak> peaks;
        std::vector<double> prefix;
    };

    /**
     * Spectral Peak and MPM on the same frame, their candidates pooled for the tracker.
     *
     * MPM follows very fast sweeps (yelps) closely but gives up in heavy noise; Spectral Peak survives the noise
     * but smears fast sweeps. Each detector's candidates are weighted by its own voicing confidence, near-equal
     * pitches are merged, and the tracker's Viterbi picks the continuous path through the pool.
     */
    class SpectralMpmDetector final : public IPitchDetector
    {
    public:
        explicit SpectralMpmDetector (const DetectorConfig& cfg);
        int frameLength() const override { return frameLen; }
        void analyse (const float* frame, float sensitivity, FrameObservation& out) override;

    private:
        SpectralPeakDetector spectral;
        MpmDetector mpm;
        int frameLen = 0;
    };

    /**
     * Subharmonic summation (Hermes 1988) on a whitened log-frequency spectrum,
     * for harmonic-rich sources whose fundamental may be weak or missing.
     */
    class HarmonicSumDetector final : public IPitchDetector
    {
    public:
        explicit HarmonicSumDetector (const DetectorConfig& cfg);
        int frameLength() const override { return window; }
        void analyse (const float* frame, float sensitivity, FrameObservation& out) override;

    private:
        float binToHz (float b) const { return logMinHz * std::exp2 (b / (float) binsPerOct); }
        float energyAt (float hz) const;

        static constexpr int binsPerOct = 96;
        static constexpr int numHarmonics = 12;
        int window = 0, fftLen = 0;
        float logMinHz = 20.0f;
        int numLogBins = 0;
        std::unique_ptr<RealFFT> fft;
        std::vector<float> win, buf, mag, logSpec, floorSpec, peaky, shs;
        std::vector<double> prefix;
        std::array<int, numHarmonics> harmonicOffset {};
        int searchLo = 0, searchHi = 0;
    };
}
