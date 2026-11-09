#include "LogoSdf.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    bool isRedFill (const juce::FillType& fill)
    {
        if (! fill.isColour())
            return false;
        const auto c = fill.colour;
        const float hue = c.getHue();
        return (hue < 0.06f || hue > 0.94f) && c.getSaturation() > 0.45f && c.getBrightness() > 0.3f;
    }

    void collect (const juce::Drawable& drawable, const juce::AffineTransform& parent, juce::Path& black, juce::Path& red)
    {
        const auto transform = drawable.getTransform().followedBy (parent);

        if (const auto* dp = dynamic_cast<const juce::DrawablePath*> (&drawable))
        {
            if (! dp->getFill().isInvisible())
            {
                auto path = dp->getPath();
                path.applyTransform (transform);
                (isRedFill (dp->getFill()) ? red : black).addPath (path);
            }
        }

        // display="none" elements are kept as invisible children; Drawable::draw() skips them, so do we.
        for (const auto* child : drawable.getChildren())
            if (const auto* d = dynamic_cast<const juce::Drawable*> (child))
                if (d->isVisible())
                    collect (*d, transform, black, red);
    }

    // Felzenszwalb-Huttenlocher 1D squared distance transform of f (0 at sites, large elsewhere).
    void edt1d (const double* f, int n, double* d, int* v, double* z)
    {
        int k = 0;
        v[0] = 0;
        z[0] = -std::numeric_limits<double>::infinity();
        z[1] = std::numeric_limits<double>::infinity();
        for (int q = 1; q < n; ++q)
        {
            double s = ((f[q] + (double) q * q) - (f[v[k]] + (double) v[k] * v[k])) / (2.0 * (q - v[k]));
            while (s <= z[k])
            {
                --k;
                s = ((f[q] + (double) q * q) - (f[v[k]] + (double) v[k] * v[k])) / (2.0 * (q - v[k]));
            }
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = std::numeric_limits<double>::infinity();
        }
        k = 0;
        for (int q = 0; q < n; ++q)
        {
            while (z[k + 1] < (double) q)
                ++k;
            const double dq = (double) (q - v[k]);
            d[q] = dq * dq + f[v[k]];
        }
    }

    // Squared distance (pixels) from each pixel to the nearest site.
    std::vector<double> edt2d (const std::vector<bool>& site, int n)
    {
        constexpr double far = 1.0e10;
        std::vector<double> grid ((size_t) (n * n));
        for (size_t i = 0; i < grid.size(); ++i)
            grid[i] = site[i] ? 0.0 : far;

        std::vector<double> f ((size_t) n), d ((size_t) n), z ((size_t) n + 1);
        std::vector<int> v ((size_t) n);
        for (int x = 0; x < n; ++x)
        {
            for (int y = 0; y < n; ++y)
                f[(size_t) y] = grid[(size_t) (y * n + x)];
            edt1d (f.data(), n, d.data(), v.data(), z.data());
            for (int y = 0; y < n; ++y)
                grid[(size_t) (y * n + x)] = d[(size_t) y];
        }
        for (int y = 0; y < n; ++y)
        {
            std::copy_n (grid.begin() + y * n, n, f.begin());
            edt1d (f.data(), n, d.data(), v.data(), z.data());
            std::copy_n (d.begin(), n, grid.begin() + y * n);
        }
        return grid;
    }

    // Signed distance in pixels (negative inside) of a coverage image thresholded at half.
    std::vector<float> signedDistance (const juce::Image& mask, int n, float& area)
    {
        std::vector<bool> inside ((size_t) (n * n)), outside ((size_t) (n * n));
        const juce::Image::BitmapData data (mask, juce::Image::BitmapData::readOnly);
        size_t count = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                const bool in = data.getPixelColour (x, y).getAlpha() >= 128;
                inside[(size_t) (y * n + x)] = in;
                outside[(size_t) (y * n + x)] = ! in;
                count += in ? 1 : 0;
            }
        area = (float) count / (float) (n * n);

        const auto toInside = edt2d (inside, n);
        const auto toOutside = edt2d (outside, n);
        std::vector<float> sd ((size_t) (n * n));
        for (size_t i = 0; i < sd.size(); ++i)
            sd[i] = inside[i] ? -((float) std::sqrt (toOutside[i]) - 0.5f) : ((float) std::sqrt (toInside[i]) - 0.5f);
        return sd;
    }
}

LogoSdf::LogoSdf()
{
    *this = build (BinaryData::sjlogo_svg, (size_t) BinaryData::sjlogo_svgSize, c_size);
}

const LogoSdf& LogoSdf::shared()
{
    static const LogoSdf logo;
    return logo;
}

LogoSdf LogoSdf::build (const void* svgData, size_t svgSize, int size)
{
    LogoSdf out (0);
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    auto drawable = juce::Drawable::createFromImageData (svgData, svgSize);
    if (drawable == nullptr || size <= 0)
        return out;

    juce::Path black, red;
    collect (*drawable, {}, black, red);
    const auto bounds = black.getBounds().getUnion (red.getBounds());
    if (bounds.isEmpty())
        return out;

    constexpr int c_oversample = 2;
    const int n = size * c_oversample;
    const float margin = 0.06f * (float) n;
    const float scale = ((float) n - 2.0f * margin) / std::max (bounds.getWidth(), bounds.getHeight());
    const auto fit = juce::AffineTransform::translation (-bounds.getCentreX(), -bounds.getCentreY())
                         .scaled (scale)
                         .translated ((float) n * 0.5f, (float) n * 0.5f);

    std::vector<float> fields[2];
    float areas[2] {};
    const juce::Path* groups[2] { &black, &red };
    for (int g = 0; g < 2; ++g)
    {
        juce::Image mask (juce::Image::SingleChannel, n, n, true, juce::SoftwareImageType());
        {
            juce::Graphics gr (mask);
            gr.setColour (juce::Colours::white);
            gr.fillPath (*groups[g], fit);
        }
        fields[g] = signedDistance (mask, n, areas[g]);
    }

    out.grenadeArea = areas[0];
    out.cherryArea = areas[1];
    out.rgba.assign ((size_t) (size * size * 4), 0);
    const float toBox = 1.0f / (float) n;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            // Rows bottom to top: image row 0 is the top of the artwork.
            const int row = size - 1 - y;
            for (int g = 0; g < 2; ++g)
            {
                float sum = 0.0f;
                for (int sy = 0; sy < c_oversample; ++sy)
                    for (int sx = 0; sx < c_oversample; ++sx)
                        sum += fields[g][(size_t) ((row * c_oversample + sy) * n + x * c_oversample + sx)];
                const float d = sum / (float) (c_oversample * c_oversample) * toBox;
                const float enc = juce::jlimit (-1.0f, 1.0f, d / c_range) * 0.5f + 0.5f;
                out.rgba[(size_t) ((y * size + x) * 4 + g)] = (std::uint8_t) juce::roundToInt (enc * 255.0f);
            }
            out.rgba[(size_t) ((y * size + x) * 4 + 2)] = 0;
            out.rgba[(size_t) ((y * size + x) * 4 + 3)] = 255;
        }

    out.buildMilliseconds = juce::Time::getMillisecondCounterHiRes() - t0;
    return out;
}

float LogoSdf::distanceAt (int x, int y, int channel) const
{
    if (rgba.empty())
        return c_range;
    x = juce::jlimit (0, c_size - 1, x);
    y = juce::jlimit (0, c_size - 1, y);
    const float enc = (float) rgba[(size_t) ((y * c_size + x) * 4 + channel)] / 255.0f;
    return (enc - 0.5f) * 2.0f * c_range;
}
