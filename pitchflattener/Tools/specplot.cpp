// Draws a spectrogram of a WAV as a PNG, with tracked pitch lines on top, so a person can see
// whether a tracker is following the right thing.
//   specplot in.wav out.png [--lines lines.csv] [--lo 80] [--hi 8000] [--width 1400] [--height 600]
//                           [--from seconds] [--to seconds]
// lines.csv rows are tracker,time_seconds,hz; hz of 0 means nothing was tracked at that moment.
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
#include "wav.inc"

// ---------------------------------------------------------------- image

struct Rgb { std::uint8_t r, g, b; };

struct Image
{
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;

    Image(int width, int height) : w(width), h(height), px(static_cast<std::size_t>(width) * height * 3, 0) {}

    void set(int x, int y, Rgb c)
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        auto* p = &px[(static_cast<std::size_t>(y) * w + x) * 3];
        p[0] = c.r; p[1] = c.g; p[2] = c.b;
    }

    // Blends towards a colour, so grid lines stay faint over the spectrogram.
    void blend(int x, int y, Rgb c, float amount)
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        auto* p = &px[(static_cast<std::size_t>(y) * w + x) * 3];
        p[0] = static_cast<std::uint8_t>(p[0] + (c.r - p[0]) * amount);
        p[1] = static_cast<std::uint8_t>(p[1] + (c.g - p[1]) * amount);
        p[2] = static_cast<std::uint8_t>(p[2] + (c.b - p[2]) * amount);
    }

    void fill(int x0, int y0, int x1, int y1, Rgb c)
    {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                set(x, y, c);
    }
};

bool writeChunk(std::ofstream& f, const char* type, const std::vector<std::uint8_t>& body)
{
    const auto be32 = [&](std::uint32_t v) { const char b[4] = { char(v >> 24), char(v >> 16), char(v >> 8), char(v) }; f.write(b, 4); };
    be32(static_cast<std::uint32_t>(body.size()));
    f.write(type, 4);
    f.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(type), 4);
    crc = crc32(crc, body.data(), static_cast<uInt>(body.size()));
    be32(static_cast<std::uint32_t>(crc));
    return static_cast<bool>(f);
}

bool savePng(const std::string& path, const Image& img)
{
    // Every row starts with filter byte 0 (none); zlib does the real compression.
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(img.h) * (img.w * 3 + 1));
    for (int y = 0; y < img.h; ++y)
    {
        raw.push_back(0);
        const auto* row = &img.px[static_cast<std::size_t>(y) * img.w * 3];
        raw.insert(raw.end(), row, row + img.w * 3);
    }
    uLongf packedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> packed(packedSize);
    if (compress2(packed.data(), &packedSize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
        return false;
    packed.resize(packedSize);

    std::ofstream f(path, std::ios::binary);
    if (! f)
        return false;
    f.write("\x89PNG\r\n\x1a\n", 8);
    std::vector<std::uint8_t> ihdr(13, 0);
    const auto put = [&](int at, std::uint32_t v) { ihdr[at] = v >> 24; ihdr[at + 1] = v >> 16; ihdr[at + 2] = v >> 8; ihdr[at + 3] = v; };
    put(0, static_cast<std::uint32_t>(img.w));
    put(4, static_cast<std::uint32_t>(img.h));
    ihdr[8] = 8;  // bits per channel
    ihdr[9] = 2;  // RGB
    return writeChunk(f, "IHDR", ihdr) && writeChunk(f, "IDAT", packed) && writeChunk(f, "IEND", {});
}

// ---------------------------------------------------------------- tiny 5x7 font

struct Glyph { char c; const char* rows[7]; };

const Glyph glyphs[] = {
    { '0', { "01110", "10001", "10011", "10101", "11001", "10001", "01110" } },
    { '1', { "00100", "01100", "00100", "00100", "00100", "00100", "01110" } },
    { '2', { "01110", "10001", "00001", "00010", "00100", "01000", "11111" } },
    { '3', { "11111", "00010", "00100", "00010", "00001", "10001", "01110" } },
    { '4', { "00010", "00110", "01010", "10010", "11111", "00010", "00010" } },
    { '5', { "11111", "10000", "11110", "00001", "00001", "10001", "01110" } },
    { '6', { "00110", "01000", "10000", "11110", "10001", "10001", "01110" } },
    { '7', { "11111", "00001", "00010", "00100", "01000", "01000", "01000" } },
    { '8', { "01110", "10001", "10001", "01110", "10001", "10001", "01110" } },
    { '9', { "01110", "10001", "10001", "01111", "00001", "00010", "01100" } },
    { 'A', { "01110", "10001", "10001", "11111", "10001", "10001", "10001" } },
    { 'B', { "11110", "10001", "10001", "11110", "10001", "10001", "11110" } },
    { 'C', { "01110", "10001", "10000", "10000", "10000", "10001", "01110" } },
    { 'D', { "11100", "10010", "10001", "10001", "10001", "10010", "11100" } },
    { 'E', { "11111", "10000", "10000", "11110", "10000", "10000", "11111" } },
    { 'F', { "11111", "10000", "10000", "11110", "10000", "10000", "10000" } },
    { 'G', { "01110", "10001", "10000", "10111", "10001", "10001", "01111" } },
    { 'H', { "10001", "10001", "10001", "11111", "10001", "10001", "10001" } },
    { 'I', { "01110", "00100", "00100", "00100", "00100", "00100", "01110" } },
    { 'J', { "00111", "00010", "00010", "00010", "00010", "10010", "01100" } },
    { 'K', { "10001", "10010", "10100", "11000", "10100", "10010", "10001" } },
    { 'L', { "10000", "10000", "10000", "10000", "10000", "10000", "11111" } },
    { 'M', { "10001", "11011", "10101", "10101", "10001", "10001", "10001" } },
    { 'N', { "10001", "10001", "11001", "10101", "10011", "10001", "10001" } },
    { 'O', { "01110", "10001", "10001", "10001", "10001", "10001", "01110" } },
    { 'P', { "11110", "10001", "10001", "11110", "10000", "10000", "10000" } },
    { 'Q', { "01110", "10001", "10001", "10001", "10101", "10010", "01101" } },
    { 'R', { "11110", "10001", "10001", "11110", "10100", "10010", "10001" } },
    { 'S', { "01111", "10000", "10000", "01110", "00001", "00001", "11110" } },
    { 'T', { "11111", "00100", "00100", "00100", "00100", "00100", "00100" } },
    { 'U', { "10001", "10001", "10001", "10001", "10001", "10001", "01110" } },
    { 'V', { "10001", "10001", "10001", "10001", "10001", "01010", "00100" } },
    { 'W', { "10001", "10001", "10001", "10101", "10101", "10101", "01010" } },
    { 'X', { "10001", "10001", "01010", "00100", "01010", "10001", "10001" } },
    { 'Y', { "10001", "10001", "10001", "01010", "00100", "00100", "00100" } },
    { 'Z', { "11111", "00001", "00010", "00100", "01000", "10000", "11111" } },
    { '.', { "00000", "00000", "00000", "00000", "00000", "01100", "01100" } },
    { '-', { "00000", "00000", "00000", "11111", "00000", "00000", "00000" } },
    { '_', { "00000", "00000", "00000", "00000", "00000", "00000", "11111" } },
    { ':', { "00000", "01100", "01100", "00000", "01100", "01100", "00000" } },
    { '/', { "00001", "00010", "00010", "00100", "01000", "01000", "10000" } },
    { '(', { "00010", "00100", "01000", "01000", "01000", "00100", "00010" } },
    { ')', { "01000", "00100", "00010", "00010", "00010", "00100", "01000" } },
    { '+', { "00000", "00100", "00100", "11111", "00100", "00100", "00000" } },
};

const Glyph* findGlyph(char c)
{
    // Lower case is drawn as upper case: one alphabet keeps the font small.
    if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 'a' + 'A');
    for (const auto& g : glyphs)
        if (g.c == c)
            return &g;
    return nullptr;  // space and anything unknown leave a gap
}

int textWidth(const std::string& s, int scale) { return static_cast<int>(s.size()) * 6 * scale - (s.empty() ? 0 : scale); }
int textHeight(int scale) { return 7 * scale; }

void drawText(Image& img, int x, int y, const std::string& s, Rgb c, int scale)
{
    for (char ch : s)
    {
        if (const Glyph* g = findGlyph(ch))
            for (int r = 0; r < 7; ++r)
                for (int k = 0; k < 5; ++k)
                    if (g->rows[r][k] == '1')
                        img.fill(x + k * scale, y + r * scale, x + (k + 1) * scale, y + (r + 1) * scale, c);
        x += 6 * scale;
    }
}

// ---------------------------------------------------------------- colour map

// Key points of a perceptually ordered black-purple-orange-yellow map, interpolated linearly.
Rgb colourFor(float v)
{
    static const float keys[][3] = {
        { 0, 0, 4 }, { 40, 11, 84 }, { 101, 21, 110 }, { 159, 42, 99 },
        { 212, 72, 66 }, { 245, 125, 21 }, { 250, 193, 39 }, { 252, 255, 164 },
    };
    const int n = static_cast<int>(std::size(keys)) - 1;
    v = std::clamp(v, 0.0f, 1.0f) * n;
    const int i = std::min(static_cast<int>(v), n - 1);
    const float t = v - i;
    const auto mix = [&](int ch) { return static_cast<std::uint8_t>(std::lround(keys[i][ch] + (keys[i + 1][ch] - keys[i][ch]) * t)); };
    return { mix(0), mix(1), mix(2) };
}

// ---------------------------------------------------------------- FFT

void fft(std::vector<std::complex<float>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const std::complex<float> wl(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (std::size_t i = 0; i < n; i += len)
        {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// ---------------------------------------------------------------- pitch lines

struct Point { double t, hz; };

struct Track
{
    std::string name;
    std::vector<Point> points;
};

bool parseNumber(const std::string& s, double& out)
{
    const char* begin = s.c_str();
    while (*begin == ' ' || *begin == '\t')
        ++begin;
    char* end = nullptr;
    out = std::strtod(begin, &end);
    if (end == begin)
        return false;
    while (*end == ' ' || *end == '\t' || *end == '\r')
        ++end;
    return *end == '\0' && std::isfinite(out);
}

std::vector<Track> loadLines(const std::string& path)
{
    std::vector<Track> tracks;
    std::ifstream f(path);
    if (! f)
    {
        std::fprintf(stderr, "specplot: cannot read %s\n", path.c_str());
        std::exit(1);
    }
    std::string line;
    while (std::getline(f, line))
    {
        std::vector<std::string> fields;
        std::stringstream ss(line);
        for (std::string field; std::getline(ss, field, ',');)
            fields.push_back(field);
        double t = 0.0, hz = 0.0;
        // The header row (and anything else odd) fails the number check and is skipped.
        if (fields.size() < 3 || ! parseNumber(fields[2], hz) || ! parseNumber(fields[1], t))
            continue;
        auto name = fields[0];
        while (! name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        while (! name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());
        auto it = std::find_if(tracks.begin(), tracks.end(), [&](const Track& tr) { return tr.name == name; });
        if (it == tracks.end())
        {
            tracks.push_back({ name, {} });
            it = tracks.end() - 1;
        }
        it->points.push_back({ t, hz });
    }
    for (auto& tr : tracks)
        std::stable_sort(tr.points.begin(), tr.points.end(), [](const Point& a, const Point& b) { return a.t < b.t; });
    return tracks;
}

// A square brush of the given size, clipped to the plot rectangle.
struct Brush
{
    Image& img;
    int x0, y0, x1, y1;

    void dab(int x, int y, int size, Rgb c)
    {
        const int lo = -(size - 1) / 2;
        for (int dy = lo; dy < lo + size; ++dy)
            for (int dx = lo; dx < lo + size; ++dx)
                if (x + dx >= x0 && x + dx < x1 && y + dy >= y0 && y + dy < y1)
                    img.set(x + dx, y + dy, c);
    }

    void line(double ax, double ay, double bx, double by, int size, Rgb c)
    {
        const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(bx - ax), std::abs(by - ay)))));
        for (int i = 0; i <= steps; ++i)
        {
            const double t = static_cast<double>(i) / steps;
            dab(static_cast<int>(std::lround(ax + (bx - ax) * t)), static_cast<int>(std::lround(ay + (by - ay) * t)), size, c);
        }
    }
};

// ---------------------------------------------------------------- main

[[noreturn]] void usage()
{
    std::fprintf(stderr, "usage: specplot in.wav out.png [--lines lines.csv] [--lo 80] [--hi 8000] [--width 1400] "
                         "[--height 600] [--from seconds] [--to seconds]\n");
    std::exit(2);
}

std::string formatSeconds(double t, double step)
{
    // Enough decimals to tell neighbouring ticks apart, no more.
    const int decimals = step >= 1.0 ? 0 : step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, std::abs(t) < step * 1e-6 ? 0.0 : t);  // no "-0.0"
    return buf;
}

std::string formatHz(double hz)
{
    char buf[32];
    if (hz >= 1000.0)
        std::snprintf(buf, sizeof buf, "%gK", hz / 1000.0);
    else
        std::snprintf(buf, sizeof buf, "%g", hz);
    return buf;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        usage();
    const std::string inPath = argv[1], outPath = argv[2];
    std::string linesPath;
    double lo = 80.0, hi = 8000.0, from = 0.0, to = -1.0;
    int width = 1400, height = 600;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (i + 1 >= argc)
            usage();
        const char* v = argv[++i];
        if (a == "--lines") linesPath = v;
        else if (a == "--lo") lo = std::atof(v);
        else if (a == "--hi") hi = std::atof(v);
        else if (a == "--width") width = std::atoi(v);
        else if (a == "--height") height = std::atoi(v);
        else if (a == "--from") from = std::atof(v);
        else if (a == "--to") to = std::atof(v);
        else usage();
    }

    Wav wav;
    if (! load(inPath, wav) || wav.channels.empty() || wav.channels[0].empty())
    {
        std::fprintf(stderr, "specplot: cannot read %s as a WAV\n", inPath.c_str());
        return 1;
    }
    const std::vector<float> mono = wav.sum();
    const double rate = wav.rate, duration = static_cast<double>(mono.size()) / rate;
    if (to < 0.0 || to > duration) to = duration;
    from = std::clamp(from, 0.0, duration);
    hi = std::min(hi, rate * 0.5);
    if (width < 200 || height < 120 || lo <= 0.0 || hi <= lo || to <= from)
    {
        std::fprintf(stderr, "specplot: bad size, frequency range or time range\n");
        return 2;
    }
    const double span = to - from;

    // Layout: frequency labels on the left, time labels underneath.
    const int scale = width >= 1000 && height >= 400 ? 2 : 1;
    const int left = textWidth("20K", scale) + 8 * scale, right = 6 * scale;
    const int top = 4 * scale, bottom = textHeight(scale) + 8 * scale;
    const int plotW = width - left - right, plotH = height - top - bottom;
    Image img(width, height);
    img.fill(0, 0, width, height, { 16, 16, 20 });

    // Short spans need short windows to show fast sweeps; long files can afford finer frequency detail.
    // About 10 ms under 1 s, rising smoothly to about 40 ms at 30 s and beyond.
    const double windowSeconds = span <= 1.0 ? 0.010 : std::min(0.040, 0.010 * std::pow(span, std::log(4.0) / std::log(30.0)));
    const int window = std::max(32, static_cast<int>(std::lround(windowSeconds * rate)));
    int fftSize = 1;
    while (fftSize < window * 4)  // zero-pad four times so the frequency axis interpolates smoothly
        fftSize <<= 1;
    const int bins = fftSize / 2 + 1;
    std::vector<float> hann(static_cast<std::size_t>(window));
    for (int i = 0; i < window; ++i)
        hann[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * (i + 0.5) / window));

    // At least one frame per pixel column; when a column covers more than a quarter window, take
    // several frames within it and keep the loudest, so short events are not skipped.
    const double columnSeconds = span / plotW;
    const int framesPerColumn = std::max(1, static_cast<int>(std::ceil(columnSeconds / (windowSeconds / 4.0))));

    // Each pixel row covers a band of frequencies on the log axis.
    std::vector<double> rowLo(static_cast<std::size_t>(plotH)), rowHi(static_cast<std::size_t>(plotH)), rowMid(static_cast<std::size_t>(plotH));
    const double logLo = std::log(lo), logHi = std::log(hi);
    const auto rowToHz = [&](double y) { return std::exp(logHi - (y / plotH) * (logHi - logLo)); };
    for (int y = 0; y < plotH; ++y)
    {
        // In fractional FFT bins.
        rowHi[static_cast<std::size_t>(y)] = rowToHz(y) * fftSize / rate;
        rowLo[static_cast<std::size_t>(y)] = rowToHz(y + 1) * fftSize / rate;
        rowMid[static_cast<std::size_t>(y)] = rowToHz(y + 0.5) * fftSize / rate;
    }

    std::vector<float> db(static_cast<std::size_t>(plotW) * plotH, -300.0f);
    std::vector<std::complex<float>> buf(static_cast<std::size_t>(fftSize));
    std::vector<float> spectrum(static_cast<std::size_t>(bins));
    float peak = -300.0f;
    for (int x = 0; x < plotW; ++x)
    {
        std::vector<float> column(static_cast<std::size_t>(bins), 1e-20f);
        for (int k = 0; k < framesPerColumn; ++k)
        {
            const double centre = from + (x + (k + 0.5) / framesPerColumn) * columnSeconds;
            const long start = std::lround(centre * rate) - window / 2;
            std::fill(buf.begin(), buf.end(), std::complex<float>(0.0f, 0.0f));
            for (int i = 0; i < window; ++i)
            {
                const long s = start + i;
                if (s >= 0 && s < static_cast<long>(mono.size()))
                    buf[static_cast<std::size_t>(i)] = mono[static_cast<std::size_t>(s)] * hann[static_cast<std::size_t>(i)];
            }
            fft(buf);
            for (int b = 0; b < bins; ++b)
                column[static_cast<std::size_t>(b)] = std::max(column[static_cast<std::size_t>(b)], std::norm(buf[static_cast<std::size_t>(b)]));
        }
        for (int b = 0; b < bins; ++b)
            spectrum[static_cast<std::size_t>(b)] = 10.0f * std::log10(column[static_cast<std::size_t>(b)]);

        for (int y = 0; y < plotH; ++y)
        {
            const double bLo = rowLo[static_cast<std::size_t>(y)], bHi = rowHi[static_cast<std::size_t>(y)];
            float v;
            if (bHi - bLo >= 1.0)
            {
                // Many bins in this row: the loudest one, so narrow harmonics stay visible.
                const int b0 = std::clamp(static_cast<int>(std::ceil(bLo)), 0, bins - 1);
                const int b1 = std::clamp(static_cast<int>(std::floor(bHi)), b0, bins - 1);
                v = *std::max_element(spectrum.begin() + b0, spectrum.begin() + b1 + 1);
            }
            else
            {
                // Less than a bin per row: interpolate between neighbouring bins in dB.
                const double b = std::clamp(rowMid[static_cast<std::size_t>(y)], 0.0, static_cast<double>(bins - 2));
                const int i = static_cast<int>(b);
                const float t = static_cast<float>(b - i);
                v = spectrum[static_cast<std::size_t>(i)] * (1.0f - t) + spectrum[static_cast<std::size_t>(i) + 1] * t;
            }
            db[static_cast<std::size_t>(y) * plotW + x] = v;
            peak = std::max(peak, v);
        }
    }

    const float range = 70.0f;
    for (int y = 0; y < plotH; ++y)
        for (int x = 0; x < plotW; ++x)
            img.set(left + x, top + y, colourFor((db[static_cast<std::size_t>(y) * plotW + x] - (peak - range)) / range));

    // Grid lines and labels.
    const Rgb labelColour { 210, 210, 215 }, gridColour { 255, 255, 255 };
    const auto hzToY = [&](double hz) { return top + (logHi - std::log(hz)) / (logHi - logLo) * plotH; };
    for (double hz : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        if (hz < lo || hz > hi)
            continue;
        const int y = static_cast<int>(std::lround(hzToY(hz)));
        for (int x = left; x < left + plotW; ++x)
            img.blend(x, y, gridColour, 0.22f);
        img.fill(left - 3 * scale, y, left, y + 1, labelColour);
        const std::string s = formatHz(hz);
        const int ty = std::clamp(y - textHeight(scale) / 2, 0, height - textHeight(scale));
        drawText(img, left - 4 * scale - textWidth(s, scale), ty, s, labelColour, scale);
    }

    // A time step that leaves room for each label.
    const double labelRoom = textWidth("00.00", scale) * 2.0;
    double step = 60.0;
    for (double s : { 0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0 })
        if (s / span * plotW >= labelRoom)
        {
            step = s;
            break;
        }
    const auto timeToX = [&](double t) { return left + (t - from) / span * plotW; };
    for (double t = std::ceil(from / step - 1e-9) * step; t <= to + 1e-9; t += step)
    {
        const int x = static_cast<int>(std::lround(timeToX(t)));
        for (int y = top; y < top + plotH; ++y)
            img.blend(x, y, gridColour, 0.22f);
        img.fill(x, top + plotH, x + 1, top + plotH + 3 * scale, labelColour);
        const std::string s = formatSeconds(t, step);
        const int tx = std::clamp(x - textWidth(s, scale) / 2, 0, width - textWidth(s, scale));
        drawText(img, tx, top + plotH + 5 * scale, s, labelColour, scale);
    }
    // Seconds, tucked into the corner the ticks leave free (frequency labels carry their own K).
    drawText(img, 2 * scale, height - textHeight(scale) - 2 * scale, "S", labelColour, scale);

    // Pitch lines: a dark halo first for every tracker, then the colours, so no line hides another's edge.
    if (! linesPath.empty())
    {
        const auto tracks = loadLines(linesPath);
        const Rgb colours[] = { { 0, 255, 255 }, { 255, 0, 255 }, { 255, 255, 0 }, { 255, 255, 255 },
                                { 0, 255, 0 }, { 255, 128, 0 } };
        Brush brush { img, left, top, left + plotW, top + plotH };
        for (int pass = 0; pass < 2; ++pass)
            for (std::size_t n = 0; n < tracks.size(); ++n)
            {
                const Rgb c = pass == 0 ? Rgb { 0, 0, 0 } : colours[n % std::size(colours)];
                const int size = pass == 0 ? 4 : 2;
                const auto& pts = tracks[n].points;
                for (std::size_t i = 0; i < pts.size(); ++i)
                {
                    if (pts[i].hz <= 0.0)
                        continue;
                    const double x = timeToX(pts[i].t), y = hzToY(pts[i].hz);
                    const bool joinsNext = i + 1 < pts.size() && pts[i + 1].hz > 0.0 && pts[i + 1].t - pts[i].t <= 0.030;
                    if (joinsNext)
                        brush.line(x, y, timeToX(pts[i + 1].t), hzToY(pts[i + 1].hz), size, c);
                    else if (! (i > 0 && pts[i - 1].hz > 0.0 && pts[i].t - pts[i - 1].t <= 0.030))
                        // A lone point: a slightly bigger dot, or a 2-pixel speck would be lost.
                        brush.dab(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), size + 2, c);
                }
            }

        // Legend in the top right corner of the plot.
        if (! tracks.empty())
        {
            int longest = 0;
            for (const auto& tr : tracks)
                longest = std::max(longest, textWidth(tr.name, scale));
            const int rowH = textHeight(scale) + 4 * scale, swatch = 10 * scale, pad = 4 * scale;
            const int boxW = pad * 3 + swatch + longest, boxH = pad * 2 + rowH * static_cast<int>(tracks.size()) - 4 * scale;
            const int bx = left + plotW - boxW - 6 * scale, by = top + 6 * scale;
            img.fill(bx, by, bx + boxW, by + boxH, { 0, 0, 0 });
            for (std::size_t n = 0; n < tracks.size(); ++n)
            {
                const int y = by + pad + static_cast<int>(n) * rowH;
                img.fill(bx + pad, y + scale, bx + pad + swatch, y + textHeight(scale) - scale, colours[n % std::size(colours)]);
                drawText(img, bx + pad * 2 + swatch, y, tracks[n].name, labelColour, scale);
            }
        }
    }

    if (! savePng(outPath, img))
    {
        std::fprintf(stderr, "specplot: cannot write %s\n", outPath.c_str());
        return 1;
    }
    return 0;
}
