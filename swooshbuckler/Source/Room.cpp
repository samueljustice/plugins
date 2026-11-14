#include "Room.h"

#include <algorithm>
#include <cmath>

namespace room
{
namespace
{
    constexpr float c_halfPi = 1.57079632679f;
    constexpr float c_twoPi = 6.28318530718f;

    using Type = juce::AudioChannelSet::ChannelType;
    using S = Speaker;

    //   name   suffix   role          azimuth elevation      x      y     z   JUCE type                              WAV bit
    const std::array<SpeakerInfo, c_numSpeakers> c_speakers { {
        { "L",   ".L",   Role::Main,    -30.0f,  0.0f,   -1.0f,  1.0f, 0.0f, Type::left,              0x1 },
        { "R",   ".R",   Role::Main,     30.0f,  0.0f,    1.0f,  1.0f, 0.0f, Type::right,             0x2 },
        { "C",   ".C",   Role::Main,      0.0f,  0.0f,    0.0f,  1.0f, 0.0f, Type::centre,            0x4 },
        { "LFE", ".LFE", Role::Lfe,       0.0f,  0.0f,    0.0f,  1.0f, 0.0f, Type::LFE,               0x8 },
        { "Ls",  ".Ls",  Role::Main,   -110.0f,  0.0f,   -1.0f, -1.0f, 0.0f, Type::leftSurround,      0x10 },
        { "Rs",  ".Rs",  Role::Main,    110.0f,  0.0f,    1.0f, -1.0f, 0.0f, Type::rightSurround,     0x20 },
        { "Lss", ".Lss", Role::Main,    -90.0f,  0.0f,   -1.0f,  0.0f, 0.0f, Type::leftSurroundSide,  0x200 },
        { "Rss", ".Rss", Role::Main,     90.0f,  0.0f,    1.0f,  0.0f, 0.0f, Type::rightSurroundSide, 0x400 },
        { "Lsr", ".Lsr", Role::Main,   -135.0f,  0.0f,   -1.0f, -1.0f, 0.0f, Type::leftSurroundRear,  0x10 },
        { "Rsr", ".Rsr", Role::Main,    135.0f,  0.0f,    1.0f, -1.0f, 0.0f, Type::rightSurroundRear, 0x20 },
        { "Ltf", ".Ltf", Role::Height,  -45.0f, 45.0f,   -1.0f,  1.0f, 1.0f, Type::topFrontLeft,      0x1000 },
        { "Rtf", ".Rtf", Role::Height,   45.0f, 45.0f,    1.0f,  1.0f, 1.0f, Type::topFrontRight,     0x4000 },
        { "Ltr", ".Ltr", Role::Height, -135.0f, 45.0f,   -1.0f, -1.0f, 1.0f, Type::topRearLeft,       0x8000 },
        { "Rtr", ".Rtr", Role::Height,  135.0f, 45.0f,    1.0f, -1.0f, 1.0f, Type::topRearRight,      0x20000 },
        { "Ltm", ".Ltm", Role::Height,  -90.0f, 45.0f,   -1.0f,  0.0f, 1.0f, Type::topSideLeft,       0x1000 },
        { "Rtm", ".Rtm", Role::Height,   90.0f, 45.0f,    1.0f,  0.0f, 1.0f, Type::topSideRight,      0x4000 },
    } };


    // Processing order: ascending JUCE channel type (which puts 7.1.4's rear surrounds after its tops).
    const std::array<Layout, c_numWidths> c_layouts { {
        { Width::Mono,        "Mono",   1,  { S::C }, -1, 0 },
        { Width::Stereo,      "Stereo", 2,  { S::L, S::R }, -1, -1 },
        { Width::Quad,        "Quad",   4,  { S::L, S::R, S::Ls, S::Rs }, -1, -1 },
        { Width::Surround51,  "5.1",    6,  { S::L, S::R, S::C, S::Lfe, S::Ls, S::Rs }, 3, 2 },
        { Width::Surround71,  "7.1",    8,  { S::L, S::R, S::C, S::Lfe, S::Lss, S::Rss, S::Lsr, S::Rsr }, 3, 2 },
        { Width::Surround712, "7.1.2",  10, { S::L, S::R, S::C, S::Lfe, S::Lss, S::Rss, S::Lsr, S::Rsr, S::Ltm, S::Rtm }, 3, 2 },
        { Width::Surround714, "7.1.4",  12, { S::L, S::R, S::C, S::Lfe, S::Lss, S::Rss, S::Ltf, S::Rtf, S::Ltr, S::Rtr, S::Lsr, S::Rsr }, 3, 2 },
    } };

    // The two file orders, as speakers.
    struct OrderList
    {
        std::array<S, c_maxChannels> film, smpte;
    };

    const std::array<OrderList, c_numWidths> c_orders { {
        { { S::C }, { S::C } },
        { { S::L, S::R }, { S::L, S::R } },
        { { S::L, S::R, S::Ls, S::Rs }, { S::L, S::R, S::Ls, S::Rs } },
        { { S::L, S::C, S::R, S::Ls, S::Rs, S::Lfe },
          { S::L, S::R, S::C, S::Lfe, S::Ls, S::Rs } },
        { { S::L, S::C, S::R, S::Lss, S::Rss, S::Lsr, S::Rsr, S::Lfe },
          { S::L, S::R, S::C, S::Lfe, S::Lsr, S::Rsr, S::Lss, S::Rss } },
        { { S::L, S::C, S::R, S::Lss, S::Rss, S::Lsr, S::Rsr, S::Lfe, S::Ltm, S::Rtm },
          { S::L, S::R, S::C, S::Lfe, S::Lsr, S::Rsr, S::Lss, S::Rss, S::Ltm, S::Rtm } },
        { { S::L, S::C, S::R, S::Lss, S::Rss, S::Lsr, S::Rsr, S::Lfe, S::Ltf, S::Rtf, S::Ltr, S::Rtr },
          { S::L, S::R, S::C, S::Lfe, S::Lsr, S::Rsr, S::Lss, S::Rss, S::Ltf, S::Rtf, S::Ltr, S::Rtr } },
    } };

    int index(Width width) noexcept { return std::clamp(static_cast<int>(width), 0, c_numWidths - 1); }

    float clamp01(float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
    float clamp11(float v) noexcept { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

    // Which processing channel each place in the room is, for the panner: -1 where the width has no such speaker.
    struct PanMap
    {
        int frontL = -1, frontR = -1, centre = -1;
        int sideL = -1, sideR = -1; // the middle row of three (7.1 and wider)
        int rearL = -1, rearR = -1;
        int topFrontL = -1, topFrontR = -1; // 7.1.2's one top pair is held here
        int topRearL = -1, topRearR = -1;
        float bedSpeakers = 0.0f; // main speakers at ear level, not counting the centre
        float topSpeakers = 0.0f;
    };

    struct FileTables
    {
        std::array<std::array<int, c_maxChannels>, c_numFileOrders> channels {};
        std::array<std::array<char, 64>, c_numFileOrders> label {};
        std::uint32_t mask = 0;
    };

    struct Tables
    {
        std::array<PanMap, c_numWidths> pan;
        // [from][to][LfeFold]
        std::array<std::array<std::array<FoldMatrix, 2>, c_numWidths>, c_numWidths> fold;
        std::array<FileTables, c_numWidths> files;
    };

    //------------------------------------------------------------------------------------------------------------------
    // Fold-downs: where one speaker's signal goes in a narrower width, step by step (see the header).
    void route(S speaker, float gain, const Layout& to, LfeFold lfe, std::array<float, c_maxChannels>& column)
    {
        if (const int at = to.indexOf(speaker); at >= 0)
        {
            column[static_cast<size_t>(at)] += gain;
            return;
        }

        const float g = gain * c_foldStep;
        const bool hasTopMiddle = to.indexOf(S::Ltm) >= 0;
        switch (speaker)
        {
            case S::Ltf: route(hasTopMiddle ? S::Ltm : S::L, g, to, lfe, column); break;
            case S::Rtf: route(hasTopMiddle ? S::Rtm : S::R, g, to, lfe, column); break;
            case S::Ltr: route(hasTopMiddle ? S::Ltm : S::Lsr, g, to, lfe, column); break;
            case S::Rtr: route(hasTopMiddle ? S::Rtm : S::Rsr, g, to, lfe, column); break;
            case S::Ltm: route(S::Lss, g, to, lfe, column); break;
            case S::Rtm: route(S::Rss, g, to, lfe, column); break;
            case S::Lss:
            case S::Lsr: route(S::Ls, g, to, lfe, column); break;
            case S::Rss:
            case S::Rsr: route(S::Rs, g, to, lfe, column); break;
            case S::Ls:  route(S::L, g, to, lfe, column); break;
            case S::Rs:  route(S::R, g, to, lfe, column); break;
            case S::C:
                route(S::L, g, to, lfe, column);
                route(S::R, g, to, lfe, column);
                break;
            case S::Lfe:
                if (lfe == LfeFold::IntoFronts)
                {
                    route(S::L, g, to, lfe, column);
                    route(S::R, g, to, lfe, column);
                }
                break;
            case S::L:
            case S::R:
                break; // every width but Mono has them, and Mono is folded from Stereo below
        }
    }

    FoldMatrix makeFold(Width from, Width to, LfeFold lfe)
    {
        const Layout& in = layout(from);
        const Layout& out = layout(to);
        FoldMatrix m;
        m.numIn = in.numChannels;
        m.numOut = out.numChannels;

        if (from == to)
        {
            for (size_t c = 0; c < static_cast<size_t>(in.numChannels); ++c)
                m.gain[c][c] = 1.0f;
            return m;
        }

        // Mono is the Stereo fold averaged.
        const bool toMono = to == Width::Mono;
        const Layout& target = toMono ? layout(Width::Stereo) : out;
        for (size_t c = 0; c < static_cast<size_t>(in.numChannels); ++c)
        {
            std::array<float, c_maxChannels> column {};
            route(in.speakers[c], 1.0f, target, lfe, column);
            if (toMono)
                m.gain[0][c] = 0.5f * (column[0] + column[1]);
            else
                for (size_t r = 0; r < static_cast<size_t>(out.numChannels); ++r)
                    m.gain[r][c] = column[r];
        }
        return m;
    }

    PanMap makePanMap(const Layout& l)
    {
        PanMap m;
        m.frontL = l.indexOf(S::L);
        m.frontR = l.indexOf(S::R);
        m.centre = l.width == Width::Mono ? -1 : l.indexOf(S::C);
        m.sideL = l.indexOf(S::Lss);
        m.sideR = l.indexOf(S::Rss);
        const bool sides = m.sideL >= 0;
        m.rearL = l.indexOf(sides ? S::Lsr : S::Ls);
        m.rearR = l.indexOf(sides ? S::Rsr : S::Rs);
        const bool topMiddle = l.indexOf(S::Ltm) >= 0;
        m.topFrontL = l.indexOf(topMiddle ? S::Ltm : S::Ltf);
        m.topFrontR = l.indexOf(topMiddle ? S::Rtm : S::Rtf);
        m.topRearL = l.indexOf(S::Ltr);
        m.topRearR = l.indexOf(S::Rtr);
        m.bedSpeakers = 2.0f + (sides ? 2.0f : 0.0f) + (m.rearL >= 0 ? 2.0f : 0.0f);
        m.topSpeakers = (m.topFrontL >= 0 ? 2.0f : 0.0f) + (m.topRearL >= 0 ? 2.0f : 0.0f);
        return m;
    }

    FileTables makeFileTables(const Layout& l)
    {
        FileTables t;
        const OrderList& lists = c_orders[static_cast<size_t>(index(l.width))];
        for (size_t o = 0; o < c_numFileOrders; ++o)
        {
            const auto& list = o == 0 ? lists.film : lists.smpte;
            size_t at = 0;
            for (size_t c = 0; c < static_cast<size_t>(l.numChannels); ++c)
            {
                t.channels[o][c] = l.indexOf(list[c]);
                if (c > 0)
                    t.label[o][at++] = ' ';
                for (const char* p = info(list[c]).name; *p != 0; ++p)
                    t.label[o][at++] = *p;
            }
            t.label[o][at] = 0;
        }
        for (size_t c = 0; c < static_cast<size_t>(l.numChannels); ++c)
            t.mask |= info(l.speakers[c]).wavBit;
        return t;
    }

    Tables makeTables()
    {
        Tables t;
        for (size_t w = 0; w < c_numWidths; ++w)
        {
            const Layout& l = c_layouts[w];
            t.pan[w] = makePanMap(l);
            t.files[w] = makeFileTables(l);
            for (size_t to = 0; to < c_numWidths; ++to)
                for (size_t f = 0; f < 2; ++f)
                    if (to <= w)
                        t.fold[w][to][f] = makeFold(l.width, c_layouts[to].width, f == 0 ? LfeFold::Drop : LfeFold::IntoFronts);
        }
        return t;
    }

    // Built once when the program loads (it reads only the constant tables above, which the compiler lays down
    // before any code runs), so nothing is built, locked or allocated on first use.
    const Tables c_tables = makeTables();

    inline void put(Gains& gains, int channel, float power) noexcept
    {
        if (channel >= 0)
            gains[static_cast<size_t>(channel)] = std::sqrt(power);
    }
}

//======================================================================================================================
const SpeakerInfo& info(Speaker speaker) noexcept
{
    return c_speakers[static_cast<size_t>(std::clamp(static_cast<int>(speaker), 0, c_numSpeakers - 1))];
}

Speaker mirrored(Speaker speaker) noexcept
{
    switch (speaker)
    {
        case S::L:   return S::R;
        case S::R:   return S::L;
        case S::Ls:  return S::Rs;
        case S::Rs:  return S::Ls;
        case S::Lss: return S::Rss;
        case S::Rss: return S::Lss;
        case S::Lsr: return S::Rsr;
        case S::Rsr: return S::Lsr;
        case S::Ltf: return S::Rtf;
        case S::Rtf: return S::Ltf;
        case S::Ltr: return S::Rtr;
        case S::Rtr: return S::Ltr;
        case S::Ltm: return S::Rtm;
        case S::Rtm: return S::Ltm;
        case S::C:
        case S::Lfe: break;
    }
    return speaker;
}

int Layout::indexOf(Speaker speaker) const noexcept
{
    for (int c = 0; c < numChannels; ++c)
        if (speakers[static_cast<size_t>(c)] == speaker)
            return c;
    return -1;
}

const Layout& layout(Width width) noexcept { return c_layouts[static_cast<size_t>(index(width))]; }

juce::AudioChannelSet channelSet(Width width)
{
    using Set = juce::AudioChannelSet;
    switch (width)
    {
        case Width::Mono:        return Set::mono();
        case Width::Stereo:      return Set::stereo();
        case Width::Quad:        return Set::quadraphonic();
        case Width::Surround51:  return Set::create5point1();
        case Width::Surround71:  return Set::create7point1();
        case Width::Surround712: return Set::create7point1point2();
        case Width::Surround714: return Set::create7point1point4();
    }
    return Set::stereo();
}

std::optional<Width> widthFor(const juce::AudioChannelSet& set)
{
    for (const Layout& l : c_layouts)
        if (set.size() == l.numChannels && set == channelSet(l.width))
            return l.width;
    return std::nullopt;
}

//======================================================================================================================
void pan(Width width, Position position, const PanSettings& settings, Gains& gains) noexcept
{
    gains.fill(0.0f);
    if (width == Width::Mono)
    {
        gains[0] = 1.0f;
        return;
    }

    const PanMap& m = c_tables.pan[static_cast<size_t>(index(width))];
    const float x = clamp11(position.x);
    const float y = clamp11(position.y);
    const float spread = clamp01(settings.spread);
    const float point = 1.0f - spread;

    // Across: left and right share the power as cos^2 / sin^2 of the angle across the room.
    const float sx = std::sin(c_halfPi * x);
    const float right = 0.5f * (1.0f + sx);
    const float left = 0.5f * (1.0f - sx); // written out, not 1 - right, so that left and right mirror exactly

    // Height: only where there are tops.
    float top = 0.0f;
    if (m.topSpeakers > 0.0f)
    {
        const float sz = std::sin(c_halfPi * clamp01(position.z) * clamp01(settings.height));
        top = sz * sz;
    }
    const float bed = 1.0f - top;

    // Depth: two rows share it across the whole room; three rows share it between neighbours.
    const float sy = std::sin(c_halfPi * y);
    const float frontOfTwo = 0.5f * (1.0f + sy);
    const float rearOfTwo = 0.5f * (1.0f - sy);
    float front = frontOfTwo, side = 0.0f, rear = rearOfTwo;
    if (m.sideL >= 0)
    {
        const float away = sy * sy; // how far the power has left the side row
        side = 1.0f - away;
        front = y > 0.0f ? away : 0.0f;
        rear = y > 0.0f ? 0.0f : away;
    }
    else if (m.rearL < 0)
    {
        front = 1.0f; // Stereo: one row
        rear = 0.0f;
    }

    // The front row: a phantom centre (left / right alone) mixed by power with left - centre - right.
    const float centre = m.centre >= 0 ? clamp01(settings.centre) : 0.0f;
    const float off = sx * sx; // in the three-speaker law: how far the power has left the centre speaker
    const float frontC = centre * (1.0f - off);
    const float frontL = (1.0f - centre) * left + centre * (x < 0.0f ? off : 0.0f);
    const float frontR = (1.0f - centre) * right + centre * (x < 0.0f ? 0.0f : off);

    // Spread: towards every speaker of the layer equally (the centre speaker as far as Centre lets it).
    const float even = spread / (m.bedSpeakers + centre);
    const float bedPoint = bed * point;
    const float bedEven = bed * even;
    put(gains, m.frontL, bedPoint * front * frontL + bedEven);
    put(gains, m.frontR, bedPoint * front * frontR + bedEven);
    put(gains, m.centre, bedPoint * front * frontC + bedEven * centre);
    put(gains, m.sideL, bedPoint * side * left + bedEven);
    put(gains, m.sideR, bedPoint * side * right + bedEven);
    put(gains, m.rearL, bedPoint * rear * left + bedEven);
    put(gains, m.rearR, bedPoint * rear * right + bedEven);

    if (m.topSpeakers > 0.0f)
    {
        const float topPoint = top * point;
        const float topEven = top * spread / m.topSpeakers;
        const float topFront = m.topRearL >= 0 ? frontOfTwo : 1.0f;
        put(gains, m.topFrontL, topPoint * topFront * left + topEven);
        put(gains, m.topFrontR, topPoint * topFront * right + topEven);
        const float topRear = m.topRearL >= 0 ? rearOfTwo : 0.0f;
        put(gains, m.topRearL, topPoint * topRear * left + topEven);
        put(gains, m.topRearR, topPoint * topRear * right + topEven);
    }
}

//======================================================================================================================
const char* pathName(Path path) noexcept
{
    switch (path)
    {
        case Path::Across:   return "Across";
        case Path::Through:  return "Through";
        case Path::Overhead: return "Overhead";
        case Path::Around:   return "Around";
    }
    return "";
}

Flight flight(Path path, float progress, float closeness, float direction, float side) noexcept
{
    const float p = clamp01(progress);
    const float close = clamp01(closeness);
    const float dir = direction < 0.0f ? -1.0f : 1.0f;
    const float sd = side < 0.0f ? -1.0f : 1.0f;
    const float u = 2.0f * p - 1.0f;

    Position at;
    float arch = 0.0f; // how high above the listener the pass really is (Overhead only)
    switch (path)
    {
        case Path::Across:
            at = { dir * u, 1.0f - 0.75f * close, 0.0f };
            break;
        case Path::Through:
            at = { sd * (0.6f - 0.48f * close), -dir * u, 0.0f };
            break;
        case Path::Overhead:
        {
            const float c = std::cos(c_halfPi * u);
            at = { sd * 0.3f * (1.0f - close), -dir * u, c * c };
            arch = (0.8f - 0.55f * close) * std::sqrt(1.0f - u * u);
            break;
        }
        case Path::Around:
        {
            const float r = 1.0f - 0.55f * close;
            const float a = dir * c_twoPi * p + (sd < 0.0f ? 2.0f * c_halfPi : 0.0f);
            at = { r * std::sin(a), r * std::cos(a), 0.0f };
            break;
        }
    }

    return { at, std::sqrt(at.x * at.x + at.y * at.y + arch * arch) };
}

//======================================================================================================================
bool canFold(Width from, Width to) noexcept { return index(to) <= index(from); }

const FoldMatrix& fold(Width from, Width to, LfeFold lfe) noexcept
{
    const auto in = static_cast<size_t>(index(from));
    const auto out = canFold(from, to) ? static_cast<size_t>(index(to)) : in;
    return c_tables.fold[in][out][lfe == LfeFold::Drop ? 0 : 1];
}

float foldWorstCaseGain(const FoldMatrix& matrix) noexcept
{
    float worst = 0.0f;
    for (size_t r = 0; r < static_cast<size_t>(matrix.numOut); ++r)
    {
        float sum = 0.0f;
        for (size_t c = 0; c < static_cast<size_t>(matrix.numIn); ++c)
            sum += std::abs(matrix.gain[r][c]);
        worst = std::max(worst, sum);
    }
    return worst;
}

void applyFold(const FoldMatrix& matrix, const float* const* in, float* const* out, int numSamples) noexcept
{
    for (size_t r = 0; r < static_cast<size_t>(matrix.numOut); ++r)
    {
        float* const dst = out[r];
        std::fill(dst, dst + numSamples, 0.0f);
        for (size_t c = 0; c < static_cast<size_t>(matrix.numIn); ++c)
        {
            const float g = matrix.gain[r][c];
            if (g == 0.0f)
                continue;
            const float* const src = in[c];
            for (int i = 0; i < numSamples; ++i)
                dst[i] += g * src[i];
        }
    }
}

//======================================================================================================================
const char* fileOrderName(FileOrder order) noexcept
{
    return order == FileOrder::ProTools ? "Pro Tools (Film)" : "SMPTE / WAV";
}

const std::array<int, c_maxChannels>& fileChannels(Width width, FileOrder order) noexcept
{
    return c_tables.files[static_cast<size_t>(index(width))].channels[order == FileOrder::ProTools ? 0 : 1];
}

const char* orderLabel(Width width, FileOrder order) noexcept
{
    return c_tables.files[static_cast<size_t>(index(width))].label[order == FileOrder::ProTools ? 0 : 1].data();
}

std::uint32_t wavChannelMask(Width width) noexcept { return c_tables.files[static_cast<size_t>(index(width))].mask; }

const char* multiMonoSuffix(Width width, int processingChannel) noexcept
{
    const Layout& l = layout(width);
    if (width == Width::Mono || processingChannel < 0 || processingChannel >= l.numChannels)
        return "";
    return info(l.speakers[static_cast<size_t>(processingChannel)]).suffix;
}
}
