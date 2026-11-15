#include "TimeHighway.h"
#include "Theme.h"

namespace
{
    // The floor is in perspective: equal steps in time get shorter towards the horizon.
    // position = (1 - 1 / (1 + depth * age / span)) / (1 - 1 / (1 + depth)), so position(span) = 1.
    constexpr float c_depth = 2.6f;

    const char* const c_vertex = R"(
        attribute vec2 position;
        varying vec2 uv;
        void main()
        {
            uv = position * 0.5 + 0.5;
            gl_Position = vec4(position, 0.0, 1.0);
        }
    )";

    const char* const c_fragment = R"(
        varying vec2 uv;
        uniform vec2 size;          // pixels
        uniform float seconds;
        uniform float flow;         // star travel, runs with the reverser
        uniform float span;         // seconds of history on the floor
        uniform float peaks[128];
        uniform vec4 heads;         // age0, gain0, age1, gain1
        uniform vec3 accent;        // read heads
        uniform vec3 sand;          // NOW
        uniform vec3 paper;         // the input's level
        uniform float horizon;

        uniform float depth;
        const float lane = 0.36;

        float roadHalf(float level) { return 0.10 + 1.15 * level; }

        float hash(float n) { return fract(sin(n * 127.1) * 43758.5453); }

        float peakAt(float t)       // t: 0 = now, 1 = span ago
        {
            float f = clamp(t, 0.0, 1.0) * 127.0;
            int i = int(floor(f));
            int j = i < 127 ? i + 1 : 127;
            return mix(peaks[i], peaks[j], f - float(i));
        }

        float line(float v, float width)   // 1 on integer values of v
        {
            float d = abs(fract(v + 0.5) - 0.5);
            float w = max(fwidth(v), 1.0e-4) * width;
            return 1.0 - smoothstep(0.0, w, d);
        }

        void main()
        {
            float aspect = size.x / size.y;
            float px = (uv.x - 0.5) * 2.0 * aspect;
            vec3 colour = vec3(0.030, 0.027, 0.025);

            if (uv.y < horizon)
            {
                // Floor. s: 0 at the near edge, 1 at the horizon.
                float s = uv.y / horizon;
                float far = 1.0 / (1.0 - s * (1.0 - 1.0 / (1.0 + depth)));   // 1 near, 1 + depth at the horizon
                float t = (far - 1.0) / depth;                               // 0 = now, 1 = span ago
                float age = t * span;
                float wx = px * far * lane;                                  // across the road, in road units
                float fog = 1.0 - 0.82 * s * s;

                // Grid: a line across every second, and lines running into the distance.
                float seconds_ = line(age, 1.4);
                float lanes = line(wx * 0.5, 1.2);
                colour += vec3(0.105, 0.098, 0.088) * (seconds_ * 0.9 + lanes * 0.45) * fog;

                // The road: as wide as the input was loud.
                float level = peakAt(t);
                float half_ = roadHalf(level);
                float edge = abs(abs(wx) - half_);
                float inside = 1.0 - smoothstep(half_ - 0.02, half_ + 0.02, abs(wx));
                float rim = 1.0 - smoothstep(0.0, max(fwidth(wx) * 1.6, 0.012), edge);
                colour += paper * (inside * (0.085 + 0.10 * level) + rim * 0.60) * fog;

                // Dashes down the middle, running the way the sound is being played.
                float dash = step(0.5, fract(age * 2.0 - flow * 1.6)) * (1.0 - smoothstep(0.012, 0.03, abs(wx)));
                colour += accent * dash * 0.22 * fog;
                colour += paper * 0.05 * exp(-edge * 5.0) * fog;

                // Read heads: a gate of light across the road, with a wake on the side it came from.
                for (int h = 0; h < 2; ++h)
                {
                    float headAge = h == 0 ? heads.x : heads.z;
                    float gain = h == 0 ? heads.y : heads.w;
                    if (gain > 0.002 && headAge <= span * 1.02)
                    {
                        float d = (age - headAge) / span;
                        float reach = 1.0 - smoothstep(half_ + 0.25, half_ + 0.9, abs(wx));
                        float flicker = 0.88 + 0.12 * sin(seconds * 31.0 + wx * 9.0 + float(h) * 2.0);
                        float gate = exp(-abs(d) * 260.0);
                        float halo = exp(-abs(d) * 38.0) * 0.30;
                        float wake = d < 0.0 ? exp(d * 9.0) * 0.20 : 0.0;
                        colour += accent * gain * (gate * 1.25 * flicker + halo + wake * inside) * (0.35 + 0.65 * reach) * (0.55 + 0.45 * fog);
                        colour += vec3(1.0) * gain * gate * 0.35 * reach * flicker;
                    }
                }

                // NOW: where new audio comes in.
                colour += sand * (exp(-uv.y * size.y / 1.6) * 0.95 + exp(-uv.y * size.y / 9.0) * 0.16);
            }
            else
            {
                // Sky: stars streaming out of the vanishing point (into the past while reversing).
                vec2 v = vec2(px, (uv.y - horizon) * 2.0);
                float angle = atan(v.y, v.x);
                float radius = length(v);
                float ray = floor(angle * 57.0);
                float star = step(0.80, hash(ray));
                float along = fract(radius * (0.25 + 0.35 * hash(ray + 3.0)) - flow * (0.25 + 0.5 * hash(ray + 7.0)) + hash(ray + 11.0));
                float streak = star * smoothstep(0.0, 0.08, along) * (1.0 - smoothstep(0.08, 0.30, along));
                colour += mix(paper, accent, hash(ray + 5.0)) * streak * 0.55 * smoothstep(0.02, 0.5, radius);
            }

            // Each read head carries a gate: two posts of light standing at the road's edges.
            for (int h = 0; h < 2; ++h)
            {
                float headAge = h == 0 ? heads.x : heads.z;
                float gain = h == 0 ? heads.y : heads.w;
                if (gain > 0.002 && headAge <= span * 1.02)
                {
                    float t = headAge / span;
                    float far = 1.0 + depth * t;
                    float y0 = (1.0 - 1.0 / far) / (1.0 - 1.0 / (1.0 + depth)) * horizon;
                    float halfScreen = (roadHalf(peakAt(t)) + 0.12) / (far * lane);
                    float tall = 0.62 / far;                                    // shorter with distance
                    float up = (uv.y - y0) / tall;
                    if (up > 0.0 && up < 1.0)
                    {
                        float post = exp(-abs(abs(px) - halfScreen) * size.y * 0.5 / (1.0 + 2.0 / far) );
                        float veil = (1.0 - smoothstep(halfScreen - 0.02, halfScreen, abs(px))) * 0.10 * (1.0 - up);
                        colour += accent * gain * (post * (1.0 - up * up) * 1.1 + veil);
                        colour += vec3(1.0) * gain * post * post * (1.0 - up) * 0.5;
                    }
                }
            }

            // Horizon glow.
            float glow = max(heads.y, heads.w);
            colour += accent * exp(-abs(uv.y - horizon) * size.y / 7.0) * (0.10 + 0.30 * glow);
            colour += accent * exp(-length(vec2(px * 0.5, (uv.y - horizon) * 3.0)) * 3.0) * (0.06 + 0.16 * glow);

            // Tube finish: faint scanlines and darker corners.
            colour *= 0.94 + 0.06 * sin(uv.y * size.y * 3.14159);
            vec2 c = uv - 0.5;
            colour *= 1.0 - dot(c, c) * 0.55;

            gl_FragColor = vec4(colour, 1.0);
        }
    )";

    void setColour(juce::OpenGLShaderProgram& program, const char* name, juce::Colour colour)
    {
        program.setUniform(name, colour.getFloatRed(), colour.getFloatGreen(), colour.getFloatBlue());
    }
}

float TimeHighway::floorPositionForAge(float ageSeconds, float spanSeconds)
{
    const float t = juce::jlimit(0.0f, 1.0f, ageSeconds / std::max(0.01f, spanSeconds));
    return (1.0f - 1.0f / (1.0f + c_depth * t)) / (1.0f - 1.0f / (1.0f + c_depth));
}

bool TimeHighway::Renderer::create(juce::OpenGLContext& context)
{
    using namespace juce::gl;

    auto program = std::make_unique<juce::OpenGLShaderProgram>(context);
    if (! program->addVertexShader(juce::OpenGLHelpers::translateVertexShaderToV3(c_vertex))
        || ! program->addFragmentShader(juce::OpenGLHelpers::translateFragmentShaderToV3(c_fragment))
        || ! program->link())
    {
        DBG("TimeHighway shader: " << program->getLastError());
        return false;
    }

    const float corners[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    glGenBuffers(1, &m_quad);
    glBindBuffer(GL_ARRAY_BUFFER, m_quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    m_program = std::move(program);
    return true;
}

void TimeHighway::Renderer::release()
{
    if (m_quad != 0)
        juce::gl::glDeleteBuffers(1, &m_quad);

    m_quad = 0;
    m_program.reset();
}

void TimeHighway::Renderer::draw(juce::OpenGLContext&, juce::Rectangle<int> viewport, const Frame& frame, double seconds)
{
    using namespace juce::gl;
    using namespace Theme::Colours;

    if (m_program == nullptr || viewport.isEmpty())
        return;

    // The stars run out of the vanishing point while reversing, and drift back slowly when not.
    const float dt = static_cast<float>(juce::jlimit(0.0, 0.1, seconds - m_lastSeconds));
    m_lastSeconds = seconds;
    m_flow = std::fmod(m_flow + dt * (frame.reversing ? 0.9f : -0.12f) + 64.0f, 64.0f);

    glViewport(viewport.getX(), viewport.getY(), viewport.getWidth(), viewport.getHeight());
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);

    m_program->use();
    m_program->setUniform("size", static_cast<float>(viewport.getWidth()), static_cast<float>(viewport.getHeight()));
    m_program->setUniform("seconds", static_cast<float>(std::fmod(seconds, 600.0)));
    m_program->setUniform("flow", m_flow);
    m_program->setUniform("span", frame.spanSeconds);
    m_program->setUniform("peaks", frame.peaks.data(), numPeaks);
    m_program->setUniform("heads", frame.headAge[0], frame.headGain[0], frame.headAge[1], frame.headGain[1]);
    m_program->setUniform("horizon", horizon);
    m_program->setUniform("depth", c_depth);
    setColour(*m_program, "accent", frame.reversing ? reverser : textDim);
    setColour(*m_program, "sand", sand);
    setColour(*m_program, "paper", text);

    const auto position = static_cast<GLuint>(glGetAttribLocation(m_program->getProgramID(), "position"));
    glBindBuffer(GL_ARRAY_BUFFER, m_quad);
    glEnableVertexAttribArray(position);
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(position);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
