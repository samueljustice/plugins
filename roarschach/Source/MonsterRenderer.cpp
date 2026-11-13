#include "MonsterRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace roar
{
    using namespace juce::gl;
    using P = shaders::Program;

    namespace
    {
        // The tier each GPU settled on, for this process, with the size of the view it settled at: a
        // reopened editor (or a recreated context) of about that size or larger starts there instead
        // of at the top, and keeps an upgrade lock-out and any relief. Fixed storage, so the GL
        // thread never allocates for it.
        struct TierMemory
        {
            struct Entry { char key[128] {}; int tier = -1; bool locked = false; double pixels = 0.0; int relief = 0; };
            static std::mutex& lock() { static std::mutex m; return m; }
            static std::array<Entry, 4>& entries() { static std::array<Entry, 4> e; return e; }

            static bool recall (const char* key, int& tier, bool& locked, double& pixels, int& relief)
            {
                const std::lock_guard<std::mutex> g (lock());
                for (auto& e : entries())
                    if (e.tier >= 0 && std::strncmp (e.key, key, sizeof (e.key) - 1) == 0)
                    {
                        tier = e.tier;
                        locked = e.locked;
                        pixels = e.pixels;
                        relief = e.relief;
                        return true;
                    }
                return false;
            }

            static void store (const char* key, int tier, bool locked, double pixels, int relief)
            {
                const std::lock_guard<std::mutex> g (lock());
                Entry* slot = nullptr;
                for (auto& e : entries())
                    if (e.tier >= 0 && std::strncmp (e.key, key, sizeof (e.key) - 1) == 0)
                        slot = &e;
                if (slot == nullptr)
                    for (auto& e : entries())
                        if (e.tier < 0 && slot == nullptr)
                            slot = &e;
                if (slot == nullptr)
                    slot = &entries()[0];
                std::snprintf (slot->key, sizeof (slot->key), "%s", key);
                slot->tier = tier;
                slot->locked = locked;
                slot->pixels = pixels;
                slot->relief = relief;
            }
        };

        constexpr int c_inkReadTexels = 64 * 48;            // the budget's read: the largest buffer's third mip, at any aspect a view has
        constexpr float c_atmoTintShare = 0.22f;           // how much of the stage's own hue the grade keeps
        constexpr float c_atmoHueLean = 3.0f;              // at most this many degrees of hue off the palette's
        constexpr float c_minDt = 1.0f / 120.0f, c_maxDt = 1.0f / 20.0f;

        const char* const c_rNames[] = { "uSrcS", "uDither", "uInkRes", "uInkStep", "uInkRule", "uInkTier" };
        const char* const c_sNames[] = { "uNoise", "uLogoSdf", "uSrc", "uInkTex" };

        float clamp01 (float v) { return std::isfinite (v) ? std::min (1.0f, std::max (0.0f, v)) : 0.0f; }
        float smooth (float a, float b, float x) { const float t = clamp01 ((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
        float mixf (float a, float b, float t) { return a + (b - a) * t; }

        float hashf (float n) { return n - std::floor (n); }

        // hue in degrees (0 for a grey)
        float hueOf (const float c[3])
        {
            const float mx = std::max (c[0], std::max (c[1], c[2])), mn = std::min (c[0], std::min (c[1], c[2])), d = mx - mn;
            if (d < 1.0e-6f)
                return 0.0f;
            const int top = c[0] >= c[1] && c[0] >= c[2] ? 0 : (c[1] >= c[2] ? 1 : 2);
            float h = top == 0 ? std::fmod ((c[1] - c[2]) / d + 6.0f, 6.0f) : top == 1 ? (c[2] - c[0]) / d + 2.0f : (c[0] - c[1]) / d + 4.0f;
            return h * 60.0f;
        }

        // The stage's hue lean on the palette: limited to c_atmoHueLean degrees on the ramp's main tone,
        // its value unchanged (a lean, never a different palette)
        void limitAtmoTint (float tint[3])
        {
            const float ref[3] = { 0.580f, 0.690f, 0.757f };   // #94b0c1
            const float h0 = hueOf (ref);
            const auto luma = [] (const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; };
            for (int i = 0; i < 32; ++i)
            {
                const float c[3] = { ref[0] * tint[0], ref[1] * tint[1], ref[2] * tint[2] };
                float dh = std::abs (hueOf (c) - h0);
                dh = std::min (dh, 360.0f - dh);
                if (dh <= c_atmoHueLean)
                    break;
                for (int k = 0; k < 3; ++k)
                    tint[k] = 1.0f + (tint[k] - 1.0f) * 0.85f;
            }
            const float c[3] = { ref[0] * tint[0], ref[1] * tint[1], ref[2] * tint[2] };
            const float k = luma (ref) / std::max (1.0e-4f, luma (c));
            for (int i = 0; i < 3; ++i)
                tint[i] *= k;
        }

        bool hasExtension (const char* name)
        {
            const auto* ext = reinterpret_cast<const char*> (glGetString (GL_EXTENSIONS));
            return ext != nullptr && std::strstr (ext, name) != nullptr;
        }
    }

    void MonsterFrame::setArray (U u, const float* data, int n)
    {
        const auto& s = c_uniformSpecs[(int) u];
        n = std::min (n, s.comps * s.count);
        std::copy (data, data + n, at (u));
    }

    //==============================================================================================
    bool MonsterRenderer::makeTex (Tex& t, int w, int h, bool halfFloat, bool repeat, const void* rgba8)
    {
        freeTex (t);
        glGenTextures (1, &t.tex);
        glBindTexture (GL_TEXTURE_2D, t.tex);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? (GLint) GL_REPEAT : (GLint) GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? (GLint) GL_REPEAT : (GLint) GL_CLAMP_TO_EDGE);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        if (halfFloat)
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        else
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
        t.w = w;
        t.h = h;

        if (rgba8 != nullptr)   // a source texture: no framebuffer
        {
            glBindTexture (GL_TEXTURE_2D, 0);
            return true;
        }

        glGenFramebuffers (1, &t.fbo);
        glBindFramebuffer (GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        const bool ok = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok)
        {
            glViewport (0, 0, w, h);
            glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
            glClear (GL_COLOR_BUFFER_BIT);
        }
        glBindFramebuffer (GL_FRAMEBUFFER, m_hostFbo);
        glBindTexture (GL_TEXTURE_2D, 0);
        return ok;
    }

    void MonsterRenderer::freeTex (Tex& t)
    {
        if (t.fbo != 0)
            glDeleteFramebuffers (1, &t.fbo);
        if (t.tex != 0)
            glDeleteTextures (1, &t.tex);
        t = Tex();
    }

    bool MonsterRenderer::buildProgram (P which, Prog& prog)
    {
        const auto compileOne = [] (GLenum type, const juce::String& code) -> GLuint
        {
            const GLuint s = glCreateShader (type);
            const GLchar* c = code.toRawUTF8();
            glShaderSource (s, 1, &c, nullptr);
            glCompileShader (s);
            GLint ok = 0;
            glGetShaderiv (s, GL_COMPILE_STATUS, &ok);
            if (! ok)
            {
                GLchar log[2048] {};
                glGetShaderInfoLog (s, (GLsizei) sizeof (log) - 1, nullptr, log);
                DBG ("MonsterRenderer shader: " << log);
                glDeleteShader (s);
                return 0;
            }
            return s;
        };

        const auto vs = compileOne (GL_VERTEX_SHADER, juce::OpenGLHelpers::translateVertexShaderToV3 (shaders::vertex()));
        auto source = shaders::fragment (which, m_textureLod);
        if ((which == P::singlePass || which == P::sim || which == P::composite) && ! m_core && m_textureLod)
            source = "#extension GL_ARB_shader_texture_lod : enable\n" + source;
        const auto fs = compileOne (GL_FRAGMENT_SHADER, juce::OpenGLHelpers::translateFragmentShaderToV3 (juce::String (source)));
        if (vs == 0 || fs == 0)
        {
            if (vs != 0) glDeleteShader (vs);
            if (fs != 0) glDeleteShader (fs);
            return false;
        }

        const GLuint id = glCreateProgram();
        glAttachShader (id, vs);
        glAttachShader (id, fs);
        glBindAttribLocation (id, 0, "aPosition");
        glLinkProgram (id);
        glDeleteShader (vs);
        glDeleteShader (fs);
        GLint linked = 0;
        glGetProgramiv (id, GL_LINK_STATUS, &linked);
        if (! linked)
        {
            DBG ("MonsterRenderer link failed: " << shaders::name (which));
            glDeleteProgram (id);
            return false;
        }

        prog = Prog();
        prog.id = id;
        glUseProgram (id);
        for (int i = 0; i < c_numFrameUniforms; ++i)
            prog.frameLoc[(size_t) i] = glGetUniformLocation (id, c_uniformSpecs[i].name);
        for (int i = 0; i < (int) R::numR; ++i)
            prog.loc[(size_t) i] = glGetUniformLocation (id, c_rNames[i]);
        int unit = 0;
        for (int i = 0; i < (int) S::numS; ++i)
        {
            const GLint loc = glGetUniformLocation (id, c_sNames[i]);
            prog.unit[(size_t) i] = loc >= 0 ? unit : -1;
            if (loc >= 0)
                glUniform1i (loc, unit++);
        }
        glUseProgram (0);
        return true;
    }

    bool MonsterRenderer::probeFloat()
    {
        Tex t;
        if (! makeTex (t, 8, 8, true))
        {
            freeTex (t);
            return false;
        }
        glBindFramebuffer (GL_FRAMEBUFFER, t.fbo);
        glViewport (0, 0, 8, 8);
        glUseProgram (m_progs[(size_t) P::probe].id);
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        float px[8 * 8 * 4] {};
        glReadPixels (0, 0, 8, 8, GL_RGBA, GL_FLOAT, px);
        glUseProgram (0);
        glBindFramebuffer (GL_FRAMEBUFFER, m_hostFbo);
        freeTex (t);
        const float want[4] = { -2.5f, 0.001f, 1000.0f, 0.5f };
        for (int i = 0; i < 64; ++i)
            for (int c = 0; c < 4; ++c)
                if (! (std::abs (px[i * 4 + c] - want[c]) <= 0.01f * std::abs (want[c])))
                    return false;
        return true;
    }

    bool MonsterRenderer::create (bool coreProfile, float aspect, const std::vector<std::uint8_t>& logoRgba, int logoSize,
                                  const std::vector<std::uint8_t>& noiseRgba, int noiseSize)
    {
        release();
        const auto t0 = std::chrono::steady_clock::now();
        m_core = coreProfile;
        m_aspect = std::isfinite (aspect) && aspect > 0.5f ? aspect : 3.27f;
        GLint host = 0;
        glGetIntegerv (GL_FRAMEBUFFER_BINDING, &host);
        m_hostFbo = (unsigned int) host;

        // The quad (attribute 0 for every program).
        static constexpr GLfloat quad[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
        glGenBuffers (1, &m_vbo);
        glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
        glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);
        if (m_core)
        {
            glGenVertexArrays (1, &m_vao);
            glBindVertexArray (m_vao);
            glEnableVertexAttribArray (0);
            glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glBindVertexArray (0);
        }
        glBindBuffer (GL_ARRAY_BUFFER, 0);

        // The single pass first: without it nothing draws.
        m_textureLod = m_core || hasExtension ("GL_ARB_shader_texture_lod");
        if (! buildProgram (P::singlePass, m_progs[(size_t) P::singlePass]))
        {
            release();
            return false;
        }

        // The tiers with the ink buffer need a framebuffer texture with mips, read at a level of
        // detail; half floats only choose the buffer's format.
        GLint units = 0;
        glGetIntegerv (GL_MAX_TEXTURE_IMAGE_UNITS, &units);
        m_stats.maxUnits = units;
        const bool floats = m_core || (hasExtension ("GL_ARB_texture_float")
                                       && (hasExtension ("GL_ARB_color_buffer_float") || hasExtension ("GL_ARB_half_float_pixel")));
        bool ok = units >= 4 && m_textureLod && glActiveTexture != nullptr && glGenFramebuffers != nullptr
               && glCheckFramebufferStatus != nullptr && glGenerateMipmap != nullptr;
        for (const P which : { P::sim, P::composite, P::copy })
            ok = ok && buildProgram (which, m_progs[(size_t) which]);
        if (ok && floats && ! m_inkBytesForced)
            buildProgram (P::probe, m_progs[(size_t) P::probe]);
        // (the single pass upscales its half-resolution frame with the copy program: built even when
        // the buffer's tiers are not available)
        m_q0HalfFailed = glGenFramebuffers == nullptr || glCheckFramebufferStatus == nullptr || glActiveTexture == nullptr;
        if (! m_q0HalfFailed && m_progs[(size_t) P::copy].id == 0)
            m_q0HalfFailed = ! buildProgram (P::copy, m_progs[(size_t) P::copy]);

        if (m_core)
            glBindVertexArray (m_vao);
        else
        {
            glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
            glEnableVertexAttribArray (0);
            glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        }
        m_stats.probeOk = ok && m_progs[(size_t) P::probe].id != 0 && probeFloat();
        m_inkBytes = ! m_stats.probeOk;

        const auto* renderer = reinterpret_cast<const char*> (glGetString (GL_RENDERER));
        const auto* vendor = reinterpret_cast<const char*> (glGetString (GL_VENDOR));
        m_capTier = ok ? QualityGovernor::startTierFor (renderer, vendor) : 0;
        // (the single pass is light enough for a fast GPU at the view's own size)
        m_inkFull = QualityGovernor::startTierFor (renderer, vendor) >= 2;

        if (! noiseRgba.empty() && noiseSize > 0)
        {
            Tex n;
            makeTex (n, noiseSize, noiseSize, false, true, noiseRgba.data());
            m_noiseTex = n.tex;
            if (m_noiseTex != 0 && glGenerateMipmap != nullptr)
            {
                // the creature reads the noise at coarse levels (large eddies, cloud banks)
                glBindTexture (GL_TEXTURE_2D, m_noiseTex);
                glGenerateMipmap (GL_TEXTURE_2D);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glBindTexture (GL_TEXTURE_2D, 0);
            }
        }
        if (! logoRgba.empty() && logoSize > 0)
        {
            Tex l;
            makeTex (l, logoSize, logoSize, false, false, logoRgba.data());
            m_logoTex = l.tex;
        }
        if (! ok)
            m_capTier = 0;
        m_simOk = ok && m_noiseTex != 0;
        if (! m_simOk)
            m_capTier = 0;

        // One GPU timer per frame, if the driver has them.
        m_timers = false;
        if (glGenQueries != nullptr && glBeginQuery != nullptr && glEndQuery != nullptr && glGetQueryObjectuiv != nullptr
            && glGetQueryiv != nullptr)
        {
            GLint bits = 0;
            glGetQueryiv (GL_TIME_ELAPSED, GL_QUERY_COUNTER_BITS, &bits);
            if (bits > 0)
            {
                glGenQueries ((GLsizei) m_queries.size(), m_queries.data());
                m_timers = true;
            }
            while (glGetError() != GL_NO_ERROR) {}
        }
        m_stats.timers = m_timers;

        if (m_core)
            glBindVertexArray (0);
        else
        {
            glDisableVertexAttribArray (0);
            glBindBuffer (GL_ARRAY_BUFFER, 0);
        }
        glBindFramebuffer (GL_FRAMEBUFFER, m_hostFbo);

        // the governor starts where it settled last time on this GPU (this process), not at the top again
        std::snprintf (m_rendererKey, sizeof (m_rendererKey), "%s|%s", renderer != nullptr ? renderer : "", vendor != nullptr ? vendor : "");
        {
            int settled = -1, relief = 0;
            bool locked = false;
            double pixels = 0.0;
            if (TierMemory::recall (m_rendererKey, settled, locked, pixels, relief))
                m_governor.resume (m_capTier, settled, locked, pixels, relief);
            else
                m_governor.reset (m_capTier);
        }
        m_tier = m_forcedTier >= 0 ? std::min (m_forcedTier, m_capTier) : std::min (m_governor.tier, m_capTier);
        if (m_tier >= 1 && ! allocInk (m_tier))
        {
            freeInk();
            m_simOk = false;
            m_capTier = m_tier = 0;
        }
        glBindFramebuffer (GL_FRAMEBUFFER, m_hostFbo);
        m_fadeUnsupported = false;
        m_prevTier = -1;
        m_needsSeed = true;
        m_tierFade = -1.0f;
        m_tierCapture = false;
        m_stats.startTier = m_capTier;
        m_stats.compileMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        m_created = true;
        return true;
    }

    void MonsterRenderer::rememberTier()
    {
        // (the lock with the size of the view it was set at, or it would hold a smaller view down)
        if (m_created && m_forcedTier < 0 && m_rendererKey[0] != 0)
            TierMemory::store (m_rendererKey, m_governor.tier, m_governor.upgradesLocked,
                               m_governor.upgradesLocked && m_governor.lockPixels > 0.0 ? m_governor.lockPixels : m_governor.pixels, m_governor.relief);
    }

    void MonsterRenderer::forgetTiersForTests()
    {
        const std::lock_guard<std::mutex> g (TierMemory::lock());
        for (auto& e : TierMemory::entries())
            e = TierMemory::Entry();
    }

    void MonsterRenderer::release()
    {
        rememberTier();
        if (m_created || m_vbo != 0)
        {
            for (auto& p : m_progs)
            {
                if (p.id != 0)
                    glDeleteProgram (p.id);
                p = Prog();
            }
            for (auto* t : { &m_half, &m_fade, &m_q0 })
                freeTex (*t);
            freeInk();
            if (m_noiseTex != 0) glDeleteTextures (1, &m_noiseTex);
            if (m_logoTex != 0) glDeleteTextures (1, &m_logoTex);
            if (m_vbo != 0) glDeleteBuffers (1, &m_vbo);
            if (m_vao != 0) glDeleteVertexArrays (1, &m_vao);
            if (m_timers)
                glDeleteQueries ((GLsizei) m_queries.size(), m_queries.data());
        }
        m_noiseTex = m_logoTex = m_vbo = m_vao = 0;
        m_queries.fill (0);
        m_queryPending.fill (false);
        m_timers = false;
        m_winW = m_winH = m_pendW = m_pendH = 0;
        m_created = m_simOk = false;
        m_needsSeed = true;
        m_cur = nullptr;
        m_tierFade = -1.0f;   // (a crossfade never outlives its textures)
        m_tierCapture = false;
        m_prevTier = -1;
        m_fadeUnsupported = false;
    }

    //==============================================================================================
    void MonsterRenderer::prepare (MonsterFrame& f)
    {
        const float W = f.get (U::uRes, 0), H = f.get (U::uRes, 1);
        const float aspect = H > 0.0f ? W / H : m_aspect;
        f.set (U::uAspect, aspect);

        // the stage's hue lean: its lit cloud colour, normalised and mostly washed out (a subtle
        // shift within the cool palette)
        {
            const float* lit = f.at (U::uColCloudLit);
            const float l = std::max (1.0e-3f, 0.3f * lit[0] + 0.59f * lit[1] + 0.11f * lit[2]);
            float tint[3];
            for (int c = 0; c < 3; ++c)
                tint[c] = mixf (1.0f, std::min (1.6f, std::max (0.4f, lit[c] / l)), c_atmoTintShare);
            limitAtmoTint (tint);
            f.set (U::uAtmoTint, tint[0], tint[1], tint[2]);
        }

        prepareInk (f);
    }

    // The slow scrolls. Each is kept inside one period and wraps where every use the shaders make of
    // it has come round whole: the noise repeats once a tile, so a scroll the shaders multiply (by
    // 0.7, 0.55, 0.6, 1.7 and their products for the clouds' drifts; by 0.5 and 0.3 for the carry's
    // patches) wraps only after as many tiles as make every one of those a whole number, and the
    // phases that go into a sine wrap at a whole turn. Wrapped at one tile, each wrap moved those
    // lookups by a part of a tile and the cloud jumped in a single frame.
    double MonsterRenderer::inkScrollRate (int i, double k, double jet)
    {
        static constexpr double rates[c_inkScrolls] = { 0.011, 0.017, -0.016, 0.009,      // the eddies (times their pace)
                                                         0.006, 0.002, -0.009, 0.004,     // the far cloud's drift xy, the near cloud's
                                                         0.004,                            // the carry's patches
                                                         0.04,                             // the breath rises (faster with its jet)
                                                         0.31, 0.17,                       // the rift's light: its two slow turns
                                                         0.16 };                           // the spines' sway
        return i < 4 ? rates[i] * k : i == 9 ? rates[i] + 0.10 * jet : rates[i];
    }

    double MonsterRenderer::inkScrollPeriod (int i)
    {
        constexpr double turn = 6.283185307179586;
        return i < 4 ? 1.0 : i < 8 ? 200.0 : i == 8 ? 10.0 : i == 9 ? 1.0 : turn;
    }

    void MonsterRenderer::stepInkScrolls (double dt, double k, double jet)
    {
        for (int i = 0; i < c_inkScrolls; ++i)
        {
            const double period = inkScrollPeriod (i);
            double p = std::fmod (m_inkPhase[i] + inkScrollRate (i, k, jet) * dt, period);
            m_inkPhase[i] = p < 0.0 ? p + period : p;
        }
    }

    void MonsterRenderer::advanceInkScrollsForTests (double seconds)
    {
        stepInkScrolls (seconds, 0.52, 0.0);
    }

    double MonsterRenderer::inkScrollWrapInForTests (int i) const
    {
        const double rate = inkScrollRate (i, 0.52, 0.0), period = inkScrollPeriod (i);
        return rate > 0.0 ? (period - m_inkPhase[i]) / rate : m_inkPhase[i] / -rate;
    }

    // A seed's genome, from the two kept (the blot and the one it glides to); made when a new seed
    // arrives (about a millisecond, no allocation).
    const CreatureAnatomy::Genome& MonsterRenderer::genomeFor (std::uint32_t seed, std::uint32_t keep)
    {
        for (size_t i = 0; i < m_genomes.size(); ++i)
            if (m_genomeValid[i] && m_genomes[i].seed == seed)
                return m_genomes[i];
        const size_t slot = m_genomeValid[0] && m_genomes[0].seed == keep ? 1 : 0;
        m_genomes[slot] = CreatureAnatomy::generate (seed);
        m_genomeValid[slot] = true;
        return m_genomes[slot];
    }

    // The ink creature's uniforms: the anatomy posed for the stage, the characters and the jaw; the
    // flow's gain and slow scrolls; the carry and erosion from the formation; the cloud; the breath.
    void MonsterRenderer::prepareInk (MonsterFrame& f)
    {
        if (m_inkTracesOff)
        {
            // (tests: each module's own reading of the view, as it would be with the module off)
            for (const U u : { U::uPitch, U::uSize, U::uRoar, U::uRoarDrive, U::uSwirlAmt, U::uSwirlNotch, U::uGateClosed, U::uTame,
                               U::uStrain, U::uSub, U::uSubLevel, U::uClean, U::uLiquid, U::uRing, U::uGrowl, U::uEnsWidth,
                               U::uDeess, U::uOvLevel, U::uGravelOn, U::uDetailLow, U::uDetailHigh })
                f.set (u, 0.0f);
            f.set (U::uChamber, 0.0f, 0.0f, 0.0f, 0.0f);
            float* bands = f.at (U::uBands);
            std::fill (bands, bands + 32, 0.0f);
        }
        const float dt = std::isfinite (f.frameDt) ? std::min (0.1f, std::max (0.0f, f.frameDt)) : 0.0f;
        const float sleep = clamp01 (f.get (U::uSleep)), awake = 1.0f - sleep;
        const float stage = clamp01 (f.get (U::uStage));
        const float* chr = f.at (U::uEvoChar);
        const std::array<float, 4> chars { clamp01 (chr[0]), clamp01 (chr[1]), clamp01 (chr[2]), clamp01 (chr[3]) };
        // the roar's heat: the ROAR module working, or simply a loud voice
        const float heat = 1.0f - (1.0f - clamp01 (f.get (U::uRoarDrive) * (0.4f + 0.6f * f.get (U::uRoar))))
                                * (1.0f - 0.8f * clamp01 (f.inputLoud) * awake);
        const float level = clamp01 (f.get (U::uLevel));
        const float glide = clamp01 (f.seedGlide);
        // (the body swells with the roar at once and lets go of it over seconds: a breath taken in
        // the middle of a roar does not shrink it)
        if (heat >= m_inkSwell)
        {
            m_inkSwell += (heat - m_inkSwell) * (1.0f - std::exp (-dt / 0.6f));
            m_inkSwellHold = 1.5f;
        }
        else if ((m_inkSwellHold -= dt) <= 0.0f)
            m_inkSwell += (heat - m_inkSwell) * (1.0f - std::exp (-dt / 3.0f));
        const float swell = clamp01 (m_inkSwell);

        // ---- the anatomy
        const auto& from = genomeFor (f.seedFrom, f.seedTo);
        auto pose = CreatureAnatomy::evaluate (from, stage, chars, f.get (U::uPitch), f.get (U::uSize));
        m_genomeNow = (int) (&from - m_genomes.data());
        if (f.seedTo != f.seedFrom)
        {
            const auto& to = genomeFor (f.seedTo, f.seedFrom);
            const auto b = CreatureAnatomy::evaluate (to, stage, chars, f.get (U::uPitch), f.get (U::uSize));
            pose = CreatureAnatomy::mix (pose, b, glide);
            if (glide >= 0.5f)
                m_genomeNow = (int) (&to - m_genomes.data());
        }
        // the head's motion: up and down, and a breath of scale about the body's centre (mirror-safe)
        {
            const float* head = f.at (U::uHead);
            const float offY = std::min (0.06f, std::max (-0.06f, head[0]));
            const float k = 1.0f + std::min (0.05f, std::max (-0.05f, head[1] + 0.012f * head[3] * (0.35f + sleep)));
            const float cy = pose.centreY;
            const auto moveY = [=] (float y) { return cy + (y - cy) * k + offY; };
            for (auto& b : pose.blob)
                b = { b[0] * k, moveY (b[1]), b[2] * k, b[3] * k };
            for (auto& a : pose.limbA)
                a = { a[0] * k, moveY (a[1]), a[2] * k, moveY (a[3]) };
            for (auto& b : pose.limbB)
                b = { b[0] * k, moveY (b[1]), b[2] * k, b[3] * k };
            for (auto& b : pose.bay)
                b = { b[0] * k, moveY (b[1]), b[2] * k, b[3] };
            pose.maw[0] = moveY (pose.maw[0]);
            pose.centreY = moveY (cy);
        }
        // ---- the modules that change the pose itself (the traces on the field are further down)
        // (eased: a gate opens and shuts between syllables, and the limbs draw back over half a second)
        m_inkGate += (clamp01 (f.get (U::uGateClosed)) * awake - m_inkGate) * (1.0f - std::exp (-dt / 0.5f));
        m_inkTame += (clamp01 (f.get (U::uTame)) * awake - m_inkTame) * (1.0f - std::exp (-dt / 0.35f));
        const float gateShut = m_inkGate, tame = m_inkTame, strain = clamp01 (f.get (U::uStrain)) * awake;
        const float subAmt = clamp01 (f.get (U::uSub)) * clamp01 (f.get (U::uSubLevel) * 2.0f) * awake;
        {
            // CLEAN: a shut gate draws the limbs back towards the core. TAME: surface tension, their reach pulled in.
            // (never less than half their reach, both at once: a blot with no limbs is a round mass)
            const float keep = std::max (0.5f, (1.0f - 0.6f * gateShut) * (1.0f - 0.2f * tame));
            if (keep < 0.999f)
                for (int i = 0; i < CreatureAnatomy::maxLimbs; ++i)
                {
                    auto& a = pose.limbA[(size_t) i];
                    auto& b = pose.limbB[(size_t) i];
                    a[2] = a[0] + (a[2] - a[0]) * keep;
                    a[3] = a[1] + (a[3] - a[1]) * keep;
                    b[0] = a[0] + (b[0] - a[0]) * keep;
                    b[1] = a[1] + (b[1] - a[1]) * keep;
                }
            // SUB: a slow heave of the whole mass at the sub's own phase
            const float heave = 1.0f + 0.045f * subAmt * std::sin (6.2831853f * clamp01 (f.get (U::uSubPhase)));
            const float cy = pose.centreY;
            if (std::abs (heave - 1.0f) > 1.0e-4f)
            {
                for (auto& b : pose.blob)
                    b = { b[0] * heave, cy + (b[1] - cy) * heave, b[2] * heave, b[3] * heave };
                for (auto& a : pose.limbA)
                    a = { a[0] * heave, cy + (a[1] - cy) * heave, a[2] * heave, cy + (a[3] - cy) * heave };
                for (auto& b : pose.limbB)
                    b = { b[0] * heave, cy + (b[1] - cy) * heave, b[2] * heave, b[3] * heave };
            }
            // (CLEAN and TAME: the frayed edge pulled back to a clean one; TAME's limiting darkens the body)
            m_inkTidy += (clamp01 (f.get (U::uClean)) * awake - m_inkTidy) * (1.0f - std::exp (-dt / 0.5f));
            pose.look.feather *= (1.0f - 0.5f * tame) * (1.0f - 0.55f * m_inkTidy) * (1.0f - 0.3f * gateShut);
            pose.look.density *= 1.0f + 0.35f * std::max (0.6f * tame, strain);
        }
        // the weighted jaw opens the rift; the lips' syllables wobble the strands; a shut gate closes it
        const float jaw = std::min (1.0f, std::max (0.0f, f.get (U::uJaw)));
        // (and no rift until there is a body round it: a hole in the first shred on the fold is a ring)
        pose.maw[2] = CreatureAnatomy::opening (jaw, stage) * awake * (1.0f - 0.85f * clamp01 (f.get (U::uGateClosed)))
                    * smooth (0.45f, 0.9f, clamp01 (f.inkFormation));
        pose.maw[3] = clamp01 (pose.maw[3] + 0.35f * std::min (1.0f, std::max (-1.0f, f.get (U::uLip))));
        // ---- kept inside the view: with the jaw hanging open, the head moving and the roar's swell,
        // the blot as posed is scaled about its middle and moved just as far as it needs (eased, so
        // an opening jaw never jolts it)
        {
            float top = -9.0f, bottom = 9.0f;
            // (as it would stand with the jaw hung well open: the fit does not follow the jaw, so the
            // weight of the jaw is never taken back by it; the widest openings may hang into the band
            // at the edge, where the ink gives way to cloud)
            const float open = CreatureAnatomy::opening (0.75f, stage) * awake, mawY = pose.maw[0];
            const auto reach = [&] (float y, float r)
            {
                // (the weighted jaw: what is under the rift hangs lower, what is over it lifts)
                const float moved = y + open * (y < mawY ? -0.65f : 0.35f);
                top = std::max (top, moved + r);
                bottom = std::min (bottom, moved - r);
            };
            for (int i = 0; i < CreatureAnatomy::maxLobes; ++i)
            {
                const auto& b = pose.blob[(size_t) i];
                const auto& r = pose.blobR[(size_t) i];
                if (r[2] > 0.3f)
                    reach (b[1], r[2] * std::sqrt (b[2] * r[1] * b[2] * r[1] + b[3] * r[0] * b[3] * r[0]));
            }
            for (int i = 0; i < CreatureAnatomy::maxLimbs; ++i)
            {
                const auto& a = pose.limbA[(size_t) i];
                const auto& b = pose.limbB[(size_t) i];
                if (b[2] > 0.004f)
                {
                    reach (a[3], 0.5f * (b[2] + b[3]));
                    reach (b[1], b[3]);
                }
            }
            float wantK = 1.0f, wantY = 0.0f;
            if (top > bottom)
            {
                // (room for the carry, the feathered edge and the swell beyond the bare shape)
                const float room = 0.40f - 0.04f * swell;
                const float mid = 0.5f * (top + bottom), half = 0.5f * (top - bottom);
                wantK = std::min (1.0f, room / std::max (0.05f, half));
                const float cy = pose.centreY;
                const float newMid = cy + (mid - cy) * wantK;
                wantY = std::min (0.0f, room - (newMid + half * wantK)) + std::max (0.0f, -room - (newMid - half * wantK));
            }
            const float ease = dt > 0.0f ? 1.0f - std::exp (-dt / 0.3f) : 1.0f;
            m_inkFitK += (wantK - m_inkFitK) * ease;
            m_inkFitY += (wantY - m_inkFitY) * ease;
            const float k = m_inkFitK, offY = m_inkFitY, cy = pose.centreY;
            if (k < 0.999f || std::abs (offY) > 1.0e-4f)
            {
                const auto moveY = [=] (float y) { return cy + (y - cy) * k + offY; };
                for (auto& b : pose.blob)
                    b = { b[0] * k, moveY (b[1]), b[2] * k, b[3] * k };
                for (auto& a : pose.limbA)
                    a = { a[0] * k, moveY (a[1]), a[2] * k, moveY (a[3]) };
                for (auto& b : pose.limbB)
                    b = { b[0] * k, moveY (b[1]), b[2] * k, b[3] * k };
                for (auto& b : pose.bay)
                    b = { b[0] * k, moveY (b[1]), b[2] * k, b[3] };
                pose.maw[0] = moveY (pose.maw[0]);
                pose.maw[1] *= k;
                pose.maw[2] *= k;
                pose.centreY = moveY (cy);
            }
        }
        m_inkPose = pose;
        for (int i = 0; i < CreatureAnatomy::maxLobes; ++i)
        {
            f.setAt (U::uBlob, i, pose.blob[(size_t) i][0], pose.blob[(size_t) i][1], pose.blob[(size_t) i][2], pose.blob[(size_t) i][3]);
            f.setAt (U::uBlobR, i, pose.blobR[(size_t) i][0], pose.blobR[(size_t) i][1], pose.blobR[(size_t) i][2], pose.blobR[(size_t) i][3]);
        }
        for (int i = 0; i < CreatureAnatomy::maxLimbs; ++i)
        {
            f.setAt (U::uLimbA, i, pose.limbA[(size_t) i][0], pose.limbA[(size_t) i][1], pose.limbA[(size_t) i][2], pose.limbA[(size_t) i][3]);
            f.setAt (U::uLimbB, i, pose.limbB[(size_t) i][0], pose.limbB[(size_t) i][1], pose.limbB[(size_t) i][2], pose.limbB[(size_t) i][3]);
        }
        for (int i = 0; i < CreatureAnatomy::maxBays; ++i)
            f.setAt (U::uBay, i, pose.bay[(size_t) i][0], pose.bay[(size_t) i][1], pose.bay[(size_t) i][2], pose.bay[(size_t) i][3]);
        // (while the ink gathers into the maker's mark there is no rift: uEgg)
        const float egg = hasLogo() ? clamp01 (f.get (U::uEgg)) : 0.0f;
        f.set (U::uEgg, egg);
        const float eggOff = 1.0f - smooth (0.0f, 0.4f, egg);
        f.set (U::uMaw, pose.maw[0], pose.maw[1], pose.maw[2] * eggOff, pose.maw[3]);

        // ---- the modules' traces on the shape and the material (see the shaders' moduleTerms). Each
        // follows its module's own activity, which is 0 while the module is off.
        {
            const float F0 = clamp01 (f.inkFormation);
            const auto wrap = [dt] (double& phase, double rate, double period) { phase = std::fmod (phase + rate * (double) dt, period); };
            const float ring = clamp01 (f.get (U::uRing)) * awake;
            const float growl = clamp01 (f.get (U::uGrowl)) * (0.35f + 0.65f * clamp01 (f.get (U::uGrowlMod))) * awake;
            const float shimmer = clamp01 (f.get (U::uLiquid)) * awake;
            // (each at a pace the eye follows: an edge that shook at the growl's own rate would flicker)
            // (and slow enough that all of them at once still move the picture at the pace of the flow)
            wrap (m_inkModPhase[0], 0.8 + 1.2 * (double) clamp01 (f.get (U::uRingFlicker)), 6.283185307179586);
            wrap (m_inkModPhase[1], 1.2 + 1.8 * (double) clamp01 (f.get (U::uGrowl)), 6.283185307179586);
            wrap (m_inkModPhase[2], 0.05 + 0.07 * (double) shimmer, 1.0);
            // (the strata follow the bands over a second: gaps that open and shut, not flicker)
            float bandSum = 0.0f;
            float* bands = f.at (U::uBands);
            const float bandEase = 1.0f - std::exp (-dt / 1.2f);
            for (int i = 0; i < 32; ++i)
            {
                m_inkBands[i] += (clamp01 (bands[i]) - m_inkBands[i]) * bandEase;
                bands[i] = m_inkBands[i];
                bandSum += m_inkBands[i];
            }
            const float strata = clamp01 (bandSum / 32.0f * 4.0f) * awake;
            const float detailLow = clamp01 (f.get (U::uDetailLow)) * awake, detailHigh = clamp01 (f.get (U::uDetailHigh)) * awake;
            const float gravel = clamp01 (f.get (U::uGravelOn)) * awake;
            const float ovLevel = clamp01 (f.get (U::uOvLevel)) * awake;
            const float ens = clamp01 (f.get (U::uEnsWidth)) * awake;
            const float deess = clamp01 (f.get (U::uDeess)) * awake;
            // the trace budget: the shape traces share one soft limit, weighted by how much each is at
            // work, so ten modules together bend the blot and do not turn it to mush
            {
                const float work = 0.075f * detailLow + 0.034f * detailHigh + 0.05f * gravel + 0.03f * strata + 0.045f * ring
                                 + 0.036f * growl + 0.055f * ovLevel * clamp01 (f.get (U::uOvReplace));
                const float want = work > 0.08f ? 0.08f / work : 1.0f;
                m_inkTraceK += (want - m_inkTraceK) * (dt > 0.0f ? 1.0f - std::exp (-dt / 0.5f) : 1.0f);
            }
            const float tk = m_inkTraceK * awake * eggOff;
            f.set (U::uModA, detailLow, detailHigh, gravel, strata);
            f.set (U::uModB, ring, (float) m_inkModPhase[0], 34.0f + 50.0f * clamp01 (f.get (U::uRingBody)), growl);
            f.set (U::uModC, (float) m_inkModPhase[1], ens > 0.0f ? 0.05f + 0.11f * ens : 0.0f, shimmer * F0, (float) m_inkModPhase[2]);
            f.set (U::uModD, ovLevel * clamp01 (f.get (U::uOvReplace)), deess * F0, subAmt * F0, tk);
            // CLEAN: tidiness, the fray at the edge pulled back
            f.set (U::uModE, m_inkTidy * eggOff, (float) m_inkPhase[12], 0.0f, 0.0f);
            // OVERTONE: the spines' roots are found on the pose, across the top of the body
            const float* harm = f.at (U::uOvHarm);
            for (int i = 0; i < 5; ++i)
            {
                // (none near the fold: a pair of spines there, rising over the middle, is a pair of horns;
                // and each a tuft of thin ones, never one long curved one, which is a horn or a tusk)
                const float x = (0.24f + 0.10f * (float) i) * m_inkFitK;
                const float lean = 0.45f + 0.20f * (float) i - 0.10f * (float) (i % 2);
                // (only on a body that has formed: a crown over shreds still gathering hangs in the cloud)
                float len = (0.05f + 0.13f * clamp01 (harm[i])) * clamp01 (ovLevel * 2.0f) * clamp01 (harm[i] * 6.0f) * eggOff * smooth (0.75f, 0.95f, F0);
                // (rooted well inside the mass, where the carried ink always stands: a spine is never
                // left hanging in the cloud over an edge the flow has moved)
                float y = 0.33f;
                if (len > 0.012f)
                    while (y > -0.3f && CreatureAnatomy::sdf (pose, x, y) > -0.05f)
                        y -= 0.012f;
                len = len > 0.012f ? len + 0.04f : 0.0f;
                // (as long as there is room above it: the band at the view's top is cloud, and a spine
                // drawn into it is not seen)
                len = std::min (len, std::max (0.0f, (0.40f - y) * std::sqrt (1.0f + lean * lean)));
                // (eased: the top of the body under a spine may change from one mass to another at once)
                const float ease = dt > 0.0f ? 1.0f - std::exp (-dt / 0.4f) : 1.0f;
                auto& sp = m_inkSpine[i];
                const float wantLen = len > 0.012f && y > -0.3f ? len : 0.0f;
                if (sp[1] < 0.005f && wantLen > 0.0f)
                    sp[0] = y;   // (a spine that is only now growing starts where it stands)
                else if (wantLen > 0.0f)
                    sp[0] += (y - sp[0]) * ease;
                sp[1] += (wantLen - sp[1]) * ease;
                // (each leans its own way, outwards more than in: a crown, never a comb)
                f.setAt (U::uSpine, i, x, sp[0], sp[1], lean);
            }
            // CHAMBER: the view's own ringing and room; the bands leave at a slow pace and return
            const float* ch = f.at (U::uChamber);
            const float echo = clamp01 (ch[0]) * awake, room = clamp01 (ch[1]) * awake;
            wrap (m_inkModPhase[3], 0.22, 1.0);
            f.set (U::uChamber, 0.55f * echo, std::max (room, 0.6f * echo), (float) m_inkModPhase[3], std::max (room, echo));
        }

        // ---- formation: the blot being glided to forms out of the smoke of the one before
        const float F = clamp01 (f.inkFormation) * (1.0f - 0.75f * std::sin (3.14159265f * glide) * (f.seedTo != f.seedFrom ? 1.0f : 0.0f));
        const float un = 1.0f - F;

        // ---- the flow: gain from the level and the roar; the characters change how it moves
        const float cFluid = chars[0], cBeast = chars[1], cEld = chars[2], cDeep = chars[3];
        auto& fl = m_inkFlow;
        // (the level moves with every syllable; the eddies follow it over a third of a second, so
        // a long carry does not shake)
        m_inkLevel += (level * awake - m_inkLevel) * (1.0f - std::exp (-dt / 0.35f));
        m_inkHeat += (heat - m_inkHeat) * (1.0f - std::exp (-dt / 0.35f));
        const float heatE = m_inkHeat;
        fl.gain = CreatureFlow::gainFor (m_inkLevel, heatE) * pose.look.flow * (1.0f - 0.3f * cDeep);
        fl.smallGain = 1.0f + 0.4f * cBeast - 0.3f * cFluid;
        // (eased: the bound pair's strength moves the whole carry, and must not shake with the voice)
        m_inkSwirl += (clamp01 (f.get (U::uSwirlAmt)) - m_inkSwirl) * (1.0f - std::exp (-dt / 0.5f));
        const float swirl = m_inkSwirl;
        // (half gathered, the bound pair turns harder: the ink arrives turning, but never wound into
        // an arc round the pair, which is a drawn curve under whatever is above it)
        fl.vortex = F * (0.02f + 0.03f * heatE + 0.02f * cEld) + 0.12f * swirl * awake + 0.08f * F * un;
        // (wide of the fold: a pair of whorls close either side of it is a pair of sockets)
        fl.vortexRadius = 0.30f;
        // (the notch sweeps at the module's own rate; the pair's place follows it over a second, or
        // the whole carried shape would be thrown about with it)
        m_inkNotch += (clamp01 (f.get (U::uSwirlNotch)) - m_inkNotch) * (1.0f - std::exp (-dt / 1.0f));
        fl.vortexX = 0.44f + 0.24f * m_inkNotch;
        // (and under the rift: a whorl either side of it, level with it or above, is a pair of sockets)
        fl.vortexY = std::min (pose.centreY, pose.maw[0]) - 0.12f;
        const double k = 0.42 + 0.10 * (double) fl.gain / (double) (CreatureFlow::c_restGain * std::max (0.5f, pose.look.flow));
        const auto scroll = [dt] (double& phase, double rate) { phase = phase + rate * (double) dt; phase -= std::floor (phase); };
        // (the jaw's speed is uneven frame to frame: the breath follows it over a fifth of a second)
        m_inkJet += (clamp01 (0.7f * heat + 0.5f * std::abs (f.get (U::uJawVel))) - m_inkJet) * (1.0f - std::exp (-dt / 0.2f));
        const float jet = clamp01 (m_inkJet);
        stepInkScrolls ((double) dt, k, (double) jet);
        fl.o1x = (float) m_inkPhase[0] + pose.look.ox;
        fl.o1y = (float) m_inkPhase[1] + pose.look.oy;
        fl.o2x = (float) m_inkPhase[2] + pose.look.oy + 0.37f;
        fl.o2y = (float) m_inkPhase[3] + pose.look.ox + 0.37f;
        const float r = std::max (1.0e-2f, fl.vortexRadius);
        f.set (U::uFlowK, fl.gain * CreatureFlow::c_scale, fl.smallGain, fl.vortex * 2.3316f / r, 1.0f / (r * r));
        f.set (U::uFlowO, fl.o1x, fl.o1y, fl.o2x, fl.o2y);
        f.set (U::uFlowC, fl.vortexX, fl.vortexY, pose.look.strandGap, pose.centreY);

        // ---- the carry: a distance the flow has carried the shape, as a time at the flow's speed
        const float speed = 0.035f * fl.gain / CreatureFlow::c_restGain;
        const float reach = (0.045f + 0.02f * stage + 0.05f * swell) * pose.look.carry;
        // (all in seconds of the flow at its rest pace, which is what the carry is traced through:
        // how far the shape has been carried does not come and go with the level)
        // (the single pass has no smoke to hang on in the cloud: what it lets go of is carried and
        // eroded at a pace the eye can follow, a few seconds, as the buffer's smoke clears; it gathers
        // as fast as the voice comes)
        // (a formation set outright, not fallen to, is taken as it is)
        if (un <= m_inkUnSingle || dt <= 0.0f || m_inkFPrev < 0.0f || m_inkFPrev - F > 0.1f)
            m_inkUnSingle = un;
        else
            m_inkUnSingle = std::min (un, m_inkUnSingle + dt * (0.10f + 0.25f * m_inkUnSingle * m_inkUnSingle));
        const float unC = m_tier == 0 ? m_inkUnSingle : un;
        // (the mark is held where it is: the carry shortens as the ink gathers into it)
        f.set (U::uCarry, reach / 0.035f * (1.0f - 0.9f * smooth (0.1f, 0.7f, egg)), unC, unC, 2.0f + 2.0f * swell);
        f.set (U::uInkF, F, swell, pose.look.feather, pose.look.density * (1.0f + 0.25f * swell));
        f.set (U::uInkO, pose.look.ox, pose.look.oy, (float) m_inkPhase[8], pose.look.ribbon);

        // ---- the cloud: its own picture asleep, folding with the blot as it forms; banks and gaps
        f.set (U::uCloudK, 0.04f + 0.61f * F, pose.look.bank * m_inkFrontCloud, 0.10f * m_inkFrontCloud, 0.45f * swell);
        f.set (U::uCloudO, (float) m_inkPhase[4], (float) m_inkPhase[5], (float) m_inkPhase[6] + 0.61f, (float) m_inkPhase[7] + 0.61f);
        {
            // the far cloud's two carries, half a cycle apart; each has no share as it starts again
            scroll (m_inkCloudPhase, 1.0 / 9.0);
            const float ph = (float) m_inkCloudPhase, phB = ph < 0.5f ? ph + 0.5f : ph - 0.5f;
            f.set (U::uCloudT, 9.0f * ph, 9.0f * phB, std::abs (2.0f * ph - 1.0f), CreatureFlow::c_restGain / std::max (0.1f, fl.gain));
        }

        // ---- the rift's light: by seed and by moment, from a dark tear to drifting lit breath
        const float ph = pose.look.ox * 40.0f;
        const float moment = m_inkMoment >= 0.0f ? clamp01 (m_inkMoment)
                                                 : smooth (-0.12f, 0.30f, std::sin ((float) m_inkPhase[10] + ph) * std::cos ((float) m_inkPhase[11] + 1.3f * ph));
        const float light = m_inkLight >= 0.0f ? clamp01 (m_inkLight) : pose.look.glow * moment * (0.35f + 0.65f * swell);
        f.set (U::uBreath, light * awake, jet, (float) m_inkPhase[9], level);

        // ---- the pigment rule: how fast free pigment turns into held ink and back, the pull on it
        {
            if (m_inkFPrev < 0.0f || dt <= 0.0f)
                m_inkFRate = 0.0f;
            else
                m_inkFRate += ((F - m_inkFPrev) / dt - m_inkFRate) * (1.0f - std::exp (-dt / 0.15f));
            m_inkFPrev = F;
            const float rising = clamp01 (m_inkFRate / 0.4f), falling = clamp01 (-m_inkFRate / 0.10f);
            // towards the body while it gathers (and a little always, awake: the pull keeps replacing
            // what the flow carries off), away from it while it lets go
            // (and in the roar: the swelling body draws on the cloud round it as it did to form)
            // (only while it swells: a roar held draws no more than the body has room for, and no
            // bank of smoke piles up round it)
            const float roarPull = 0.16f * smooth (0.35f, 0.8f, heat) * F * clamp01 ((heat - m_inkSwell + 0.06f) / 0.08f);
            // the hush: the voice has stopped, and the body starts to let go at once, at its rim and
            // its limbs, while the formation still holds (it gives only after the grace before
            // sleep). A pause between phrases tears the rim a little; the pull mends it when the
            // voice comes back.
            m_inkQuiet = level * awake < 0.08f && F > 0.05f ? std::min (9.0f, m_inkQuiet + dt) : 0.0f;
            const float hushTo = smooth (0.20f, 0.90f, m_inkQuiet) * awake;
            m_inkHush += (hushTo - m_inkHush) * (1.0f - std::exp (-dt / (hushTo > m_inkHush ? 0.25f : 0.12f)));
            const float hush = m_inkHush;
            const float drift = falling > 0.0f ? -0.10f * falling : (0.28f * rising + 0.08f * F + roarPull) * (1.0f - hush) - 0.05f * hush;
            // the release's kick: small eddies that tear what was let go, for two seconds after
            m_inkKick = std::max (std::max (falling, 0.7f * hush), m_inkKick * std::exp (-dt / 2.0f));
            // (only as the body really goes: a formation that dips between two syllables and comes
            // back has let nothing go, and the wisps round a formed body are never drawn as its smoke)
            // (the hush lets go of a share at once: the limbs and the rim tear into smoke from the
            // moment the voice stops; the floor under a long formed body keeps its core until the
            // formation itself gives)
            const float going = std::max (falling * smooth (0.01f, 0.10f, un), 0.30f * hush);
            m_inkFalling = going;
            m_inkGather = drift > 0.0f ? (0.28f * rising + roarPull) / drift : 0.0f;
            // (awake, what the rim sheds is the pigment it was made of; let go, the ink spreads into smoke)
            // (let go, the ink is still ink where it hangs dense: drawn as heavy as it was held, and
            // lighter over the next seconds as it is torn and spread, so it is smoke by six or seven)
            // (the hush draws its smoke as heavy as the formation's giving will: no lighter smoke
            // that darkens again when the body goes)
            m_inkLetGo = std::max (std::max (clamp01 (1.5f * going), hush), m_inkLetGo * std::exp (-dt / (rising > 0.0f ? 0.4f : un < 0.02f ? 1.5f : 4.5f)));
            m_inkSmoke = 1.0f + 1.8f * m_inkLetGo;   // (2.8: held ink is three times as dense as the pigment it was made of)
            // (the floor under the body comes in over seconds once it has formed: the gathering is
            // the pigment's alone)
            m_inkFloor += ((F > 0.9f ? 1.0f : 0.0f) - m_inkFloor) * (1.0f - std::exp (-dt / (F > 0.9f ? 2.0f : 0.3f)));
            const float kick = (0.25f + 0.75f * m_inkKick) * (0.3f + 0.7f * un) + 0.15f * heat;
            // (the spec's release, 0.5 + 2.0 (1 - F) a second; swollen with the roar, the body is slower
            // to let go of what the jaw's closing leaves outside it)
            f.set (U::uHold, 0.6f + 2.4f * F, 0.5f - 0.25f * swell + 2.0f * un, drift, 0.45f + 0.25f * stage + 0.2f * heat);
            // (PITCH: shifted down, its loose pigment sinks faster; shifted up, it rises)
            const float pitchK = std::min (2.0f, std::max (-1.0f, -f.get (U::uPitch) / 12.0f));
            f.set (U::uPig, c_inkAmbient, m_inkBudget, kick, 0.008f + 0.022f * pitchK * awake);
            m_inkState = { F, pose.maw[2], heat, level, f.get (U::uBreath), reach / speed, drift, kick, 0.0f, 0.0f };
        }

        // ---- the sub's onsets push the pigment outwards from the body once each; a clipped peak
        // throws a few beads of pure pigment off the rim (no more often than the flash limiter lets
        // a spark through)
        {
            const auto& af = f.forces;
            float push = std::min (0.45f, 0.12f * af.subPulse * clamp01 (f.get (U::uSub) * 2.0f)) * awake * F;
            // the force budget: what disperses the pigment (the breath's jet, the release's kick, the
            // bound pair, the sub's push) shares one allowance, so with every module at work at once
            // the ink is stirred and not thrown about
            {
                const float kickNow = f.get (U::uPig, 2), jetNow = f.get (U::uBreath, 1);
                const float sum = 0.30f * jetNow + 0.25f * kickNow + 2.2f * fl.vortex + push;
                const float allow = std::min (1.0f, 0.42f / std::max (1.0e-3f, sum));
                push *= allow;
                f.at (U::uPig)[2] = kickNow * allow;
                f.at (U::uBreath)[1] = jetNow * allow;
                m_inkState.kick = kickNow * allow;
            }
            f.set (U::uSubPulse, pose.centreY, push, 0.30f + 0.15f * stage, 0.0f);
            m_inkSparkAge = std::min (9.0f, m_inkSparkAge + dt);
            // (only off a loud voice: the clipper is doing nothing to a quiet one)
            if (af.sparkSeed != m_inkSparkSeed && af.clipSparks > 0.02f && awake > 0.1f && F > 0.5f && f.inputLoud > 0.5f)
            {
                m_inkSparkSeed = af.sparkSeed;
                m_inkSparkAge = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    // a way out of the body's middle, upwards more often than down; the bead leaves
                    // from where that way crosses the rim
                    const float a = 3.14159265f * (0.08f + 0.84f * hashf ((float) af.sparkSeed * 0.6180339f + 0.37f * (float) i));
                    const float dx = std::sin (a), dy = std::cos (a) * (hashf ((float) af.sparkSeed * 0.754877f + 0.61f * (float) i) < 0.7f ? 1.0f : -1.0f);
                    float out = 0.04f;
                    while (out < 1.2f && CreatureAnatomy::sdf (pose, dx * out, pose.centreY + dy * out) < 0.0f)
                        out += 0.02f;
                    m_inkBead[(size_t) i] = { dx * out, pose.centreY + dy * out, dx, dy };
                }
            }
            // (a bead flies a tenth of a view height and is spent in a quarter of a second)
            const float fly = 0.10f * (1.0f - std::exp (-m_inkSparkAge / 0.09f));
            const float amount = m_inkSparkAge < 0.25f ? 30.0f * clamp01 (af.clipSparks * 3.0f + 0.3f) * (1.0f - m_inkSparkAge / 0.25f) * awake : 0.0f;
            for (int i = 0; i < 3; ++i)
            {
                const auto& b = m_inkBead[(size_t) i];
                f.setAt (U::uSparkP, i, b[0] + b[2] * fly, b[1] + b[3] * fly, 0.012f, amount);
            }
            m_inkState.push = push;
            m_inkState.beads = amount;
        }
    }

    //==============================================================================================
    void MonsterRenderer::use (P which, const MonsterFrame& f)
    {
        auto& prog = m_progs[(size_t) which];
        glUseProgram (prog.id);
        m_cur = &prog;
        if (prog.uploaded == m_frame)
            return;
        prog.uploaded = m_frame;
        for (int i = 0; i < c_numFrameUniforms; ++i)
        {
            const GLint loc = prog.frameLoc[(size_t) i];
            if (loc < 0)
                continue;
            const auto& s = c_uniformSpecs[i];
            const float* v = f.at ((U) i);
            switch (s.comps)
            {
                case 1: glUniform1fv (loc, s.count, v); break;
                case 2: glUniform2fv (loc, s.count, v); break;
                case 3: glUniform3fv (loc, s.count, v); break;
                default: glUniform4fv (loc, s.count, v); break;
            }
        }
    }

    void MonsterRenderer::bindRaw (S s, unsigned int tex)
    {
        const int unit = m_cur->unit[(size_t) s];
        if (unit < 0)
            return;
        glActiveTexture ((GLenum) GL_TEXTURE0 + (GLenum) unit);
        glBindTexture (GL_TEXTURE_2D, tex);
    }

    void MonsterRenderer::bind (S s, const Tex& t) { bindRaw (s, t.tex); }

    void MonsterRenderer::setR (R r, float a) { if (m_cur->loc[(size_t) r] >= 0) glUniform1f (m_cur->loc[(size_t) r], a); }
    void MonsterRenderer::setR (R r, float a, float b) { if (m_cur->loc[(size_t) r] >= 0) glUniform2f (m_cur->loc[(size_t) r], a, b); }
    void MonsterRenderer::setR (R r, float a, float b, float c, float d) { if (m_cur->loc[(size_t) r] >= 0) glUniform4f (m_cur->loc[(size_t) r], a, b, c, d); }

    void MonsterRenderer::setScale (R r, const Tex& t, int aw, int ah)
    {
        const float w = (float) std::max (1, t.w), h = (float) std::max (1, t.h);
        setR (r, (float) aw / w, (float) ah / h, ((float) aw - 0.5f) / w, ((float) ah - 0.5f) / h);
    }

    void MonsterRenderer::draw (const Tex* dst, int w, int h, const char* passName)
    {
        if (m_passTimer != nullptr)
            m_passTimer->begin (passName);
        if (dst != nullptr)
        {
            glBindFramebuffer (GL_FRAMEBUFFER, dst->fbo);
            glViewport (0, 0, w, h);
        }
        else
        {
            glBindFramebuffer (GL_FRAMEBUFFER, m_hostFbo);
            glViewport (m_hostVp[0], m_hostVp[1], m_hostVp[2], m_hostVp[3]);
        }
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        ++m_stats.draws;
        if (m_passTimer != nullptr)
            m_passTimer->end();
    }

    //==============================================================================================
    // The single pass's half-resolution target: grown to fit, never shrunk (a live resize must not
    // reallocate every frame; the active rectangle is the frame's own size).
    bool MonsterRenderer::ensureQ0Target (int w, int h, int div)
    {
        if (m_q0HalfFailed)
            return false;
        const int hw = std::max (8, (w + div - 1) / div), hh = std::max (8, (h + div - 1) / div);
        if (m_q0.fbo != 0 && hw <= m_q0.w && hh <= m_q0.h)
            return true;
        if (! makeTex (m_q0, std::max (hw, m_q0.w), std::max (hh, m_q0.h), false))
        {
            freeTex (m_q0);
            m_q0HalfFailed = true;
            return false;
        }
        return true;
    }

    // The single pass: the heaviest shader per pixel, so it draws at half resolution and is upscaled
    // (as the composite is on tier 1): the fallback tier must never cost more than the tiers above it.
    // (A third in relief: see QualityGovernor.) Without a framebuffer object it draws straight to
    // the host at full size.
    void MonsterRenderer::drawSinglePass (const MonsterFrame& f, unsigned int hostFbo, const int vp[4])
    {
        const int div = lowDivisor();
        const bool half = (! isInkFullSize() || div > 2) && ensureQ0Target (vp[2], vp[3], div);
        const int hw = std::max (8, (vp[2] + div - 1) / div), hh = std::max (8, (vp[3] + div - 1) / div);
        const P single = P::singlePass;
        use (single, f);
        bindRaw (S::noise, m_noiseTex);
        bindRaw (S::logo, m_logoTex);
        if (half)
        {
            // its pixel-sized measures follow the target's size (use() uploads the frame's own each frame)
            auto& prog = m_progs[(size_t) single];
            const float k = (float) hh / (float) std::max (1, vp[3]);
            if (const int loc = prog.frameLoc[(size_t) U::uRes]; loc >= 0) glUniform2f (loc, (float) hw, (float) hh);
            if (const int loc = prog.frameLoc[(size_t) U::uPxScale]; loc >= 0) glUniform1f (loc, f.get (U::uPxScale) * k);
            if (const int loc = prog.frameLoc[(size_t) U::uRadius]; loc >= 0) glUniform1f (loc, f.get (U::uRadius) * k);
            prog.uploaded = 0xffffffffu;   // (the next use() uploads the frame's full-size values again)
            glBindFramebuffer (GL_FRAMEBUFFER, m_q0.fbo);
            glViewport (0, 0, hw, hh);
        }
        else
        {
            glBindFramebuffer (GL_FRAMEBUFFER, hostFbo);
            glViewport (vp[0], vp[1], vp[2], vp[3]);
        }
        if (m_passTimer != nullptr) m_passTimer->begin ("single pass");
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        if (m_passTimer != nullptr) m_passTimer->end();
        ++m_stats.draws;
        if (half)
        {
            auto& prog = m_progs[(size_t) P::copy];
            glUseProgram (prog.id);
            m_cur = &prog;
            setScale (R::uSrcS, m_q0, hw, hh);
            setR (R::uDither, 1.0f);   // (the upscale smooths the pass's own dither away: the mist would band)
            bind (S::src, m_q0);
            m_hostFbo = hostFbo;
            std::copy (vp, vp + 4, m_hostVp);
            draw (nullptr, vp[2], vp[3], "single pass upscale");
        }
    }

    //==============================================================================================
    // The ink buffer: half the view (x >= 0), a ping-pong pair with mips; mirrored across the fold
    // by its wrap mode, so filtered reads at the fold are right and the ink's symmetry is exact.
    bool MonsterRenderer::makeInkTex (int index, int w, int h)
    {
        auto& t = m_inkBuf[index];
        if (! makeTex (t, w, h, ! m_inkBytes))
            return false;
        glBindTexture (GL_TEXTURE_2D, t.tex);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint) GL_MIRRORED_REPEAT);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint) GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glGenerateMipmap (GL_TEXTURE_2D);
        glBindTexture (GL_TEXTURE_2D, 0);
        // a small mip holds all the pigment there is in a few hundred texels, read back for the
        // budget (not the smallest: each halving of an odd size drops a row, and by the last level
        // a good part of the buffer is no longer counted)
        m_inkTopLevel = 3;
        m_inkTopW = std::max (1, w >> m_inkTopLevel);
        m_inkTopH = std::max (1, h >> m_inkTopLevel);
        return true;
    }

    bool MonsterRenderer::allocInk (int tier)
    {
        const int w = inkWidthFor (tier), h = std::max (16, (int) std::lround ((float) w / (0.5f * m_aspect)));
        bool ok = makeInkTex (0, w, h) && makeInkTex (1, w, h);
        if (! ok && ! m_inkBytes)
        {
            m_inkBytes = true;   // (half floats that probe but do not complete at this size: bytes)
            ok = makeInkTex (0, w, h) && makeInkTex (1, w, h);
        }
        if (ok && m_inkPbo == 0 && glGetBufferSubData != nullptr)
        {
            glGenBuffers (1, &m_inkPbo);
            glBindBuffer (GL_PIXEL_PACK_BUFFER, m_inkPbo);
            glBufferData (GL_PIXEL_PACK_BUFFER, (GLsizeiptr) (c_inkReadTexels * 4 * sizeof (GLfloat)), nullptr, GL_STREAM_READ);
            glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
        }
        m_inkCur = 0;
        m_inkReadAge = -1;
        m_needsSeed = true;
        return ok;
    }

    void MonsterRenderer::freeInk()
    {
        for (int i = 0; i < 2; ++i)
        {
            freeTex (m_inkBuf[i]);
        }
        if (m_inkPbo != 0)
            glDeleteBuffers (1, &m_inkPbo);
        m_inkPbo = 0;
        m_inkReadAge = -1;
    }

    void MonsterRenderer::inkUniforms (float dt, bool fresh)
    {
        setR (R::uInkRes, (float) m_inkBuf[0].w, (float) m_inkBuf[0].h, 0.5f * m_aspect, dt);
        // (the last: how far the pigment in the view is over what the cloud had, for the budget)
        setR (R::uInkStep, fresh ? 1.0f : 0.0f, m_inkBytes ? 1.0f : 0.0f, hashf ((float) (m_frame % 4093u) * 0.6180339f) * 64.0f,
              std::min (4.0f, std::max (-1.0f, 2.5f * (m_inkTotal - (m_inkTotal > 1.0f ? 1.08f : 0.92f)) * (m_inkTotal > 1.08f || m_inkTotal < 0.92f ? 1.0f : 0.0f))));
        setR (R::uInkRule, m_inkFloor, m_inkFalling, m_inkSmoke, m_inkGather);
        // the cloud by tier: three layers, the far one carried in two phases, and threads, from the second buffer tier up
        setR (R::uInkTier, m_tier >= 2 ? 3.0f : 2.0f, m_tier >= 2 ? 1.0f : 0.0f, m_tier >= 2 ? 1.0f : 0.0f, m_inkLetGo);
    }

    // One step of the buffer (fresh: filled as the shape stands and the cloud's own pigment, no history).
    void MonsterRenderer::inkStep (const MonsterFrame& f, float dt, bool fresh)
    {
        use (P::sim, f);
        // (its pixel-sized measures follow the buffer: the whole view at the buffer's scale)
        auto& prog = m_progs[(size_t) P::sim];
        if (const int loc = prog.frameLoc[(size_t) U::uRes]; loc >= 0)
            glUniform2f (loc, 2.0f * (float) m_inkBuf[0].w, (float) m_inkBuf[0].h);
        inkUniforms (dt, fresh);
        bindRaw (S::logo, m_logoTex);
        bindRaw (S::noise, m_noiseTex);
        bind (S::ink, m_inkBuf[m_inkCur]);
        draw (&m_inkBuf[1 - m_inkCur], m_inkBuf[0].w, m_inkBuf[0].h, "ink step");
        m_inkCur = 1 - m_inkCur;
        bind (S::ink, m_inkBuf[m_inkCur]);
        glGenerateMipmap (GL_TEXTURE_2D);
    }

    // A change of tier: what the buffer holds is drawn into one of the new size.
    void MonsterRenderer::inkResample (int toTier)
    {
        Tex src = m_inkBuf[m_inkCur];
        m_inkBuf[m_inkCur] = Tex();
        const bool seedWas = m_needsSeed;
        const bool ok = allocInk (toTier);
        if (ok && src.tex != 0)
        {
            auto& prog = m_progs[(size_t) P::copy];
            glUseProgram (prog.id);
            m_cur = &prog;
            setR (R::uDither, 0.0f);
            setScale (R::uSrcS, src, src.w, src.h);
            bind (S::src, src);
            draw (&m_inkBuf[0], m_inkBuf[0].w, m_inkBuf[0].h, "ink resample");
            glBindTexture (GL_TEXTURE_2D, m_inkBuf[0].tex);
            glGenerateMipmap (GL_TEXTURE_2D);
            m_needsSeed = seedWas;
        }
        freeTex (src);
    }

    // All the pigment in the buffer, from a small mip of it, through a pixel buffer a few frames
    // later (never a stall); a slow gain on the cloud's own pigment keeps the total near its rest.
    void MonsterRenderer::inkBudgetRead()
    {
        if (m_inkPbo == 0 || glGetBufferSubData == nullptr)
            return;
        if (m_inkReadAge < 0)
        {
            if ((m_frame & 15u) != 0 || m_inkTopW * m_inkTopH > c_inkReadTexels)
                return;
            // (the mip itself, straight from the texture: a framebuffer on a level other than the
            // first reads the first on some drivers)
            glBindTexture (GL_TEXTURE_2D, m_inkBuf[m_inkCur].tex);
            glBindBuffer (GL_PIXEL_PACK_BUFFER, m_inkPbo);
            glPixelStorei (GL_PACK_ALIGNMENT, 1);
            glGetTexImage (GL_TEXTURE_2D, m_inkTopLevel, GL_RGBA, GL_FLOAT, nullptr);
            glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
            m_inkReadAge = 0;
        }
        else if (++m_inkReadAge >= 3)
        {
            GLfloat px[c_inkReadTexels * 4];
            const int n = m_inkTopW * m_inkTopH;
            glBindBuffer (GL_PIXEL_PACK_BUFFER, m_inkPbo);
            glGetBufferSubData (GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr) ((size_t) n * 4 * sizeof (GLfloat)), px);
            glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
            m_inkReadAge = -1;
            double heldSum = 0.0, freeSum = 0.0;
            for (int i = 0; i < n; ++i)
            {
                heldSum += (double) px[i * 4];
                freeSum += (double) px[i * 4 + 1];
            }
            const float k = (m_inkBytes ? 2.5f : 1.0f) / (float) n;
            const float total = (float) (heldSum / (double) c_inkYield + freeSum) * k / c_inkAmbient;
            if (std::isfinite (total) && total >= 0.0f)
            {
                // (a read every half second: the total is followed, never jumped to, and the gain
                // moves over several seconds)
                m_inkTotal += std::min (0.3f, std::max (-0.3f, total - m_inkTotal));
                // (what the body holds was the cloud's: the cloud's own share is what is left)
                const float bodyShare = (float) (heldSum / (double) c_inkYield) * k / c_inkAmbient;
                // (but only part of it: the whole of it taken back from the cloud leaves too little
                // loose pigment round the body for the pull to gather, and the body forms thin. The
                // total is held in the spec's 0.8 to 1.3 of rest, not at rest exactly.)
                const float want = std::min (1.1f, std::max (0.3f, 1.12f - 0.6f * bodyShare));
                m_inkBudget += std::min (0.15f, std::max (-0.15f, want - m_inkBudget));
            }
        }
    }

    // The half-size composite's target (a third in relief) and the tier crossfade's kept frame (a
    // change of size waits until it has held for 200 ms, so a live resize does not reallocate every
    // frame).
    void MonsterRenderer::ensureInkWindowTargets (int w, int h, float dt, int div)
    {
        if (w == m_winW && h == m_winH && div == m_halfDiv)
        {
            m_pendSeconds = 0.0f;
            return;
        }
        if (w != m_pendW || h != m_pendH)
        {
            m_pendW = w;
            m_pendH = h;
            m_pendSeconds = 0.0f;
        }
        m_pendSeconds += dt;
        if (m_winW != 0 && m_pendSeconds < 0.2f && div == m_halfDiv)
            return;
        makeTex (m_half, std::max (8, (w + div - 1) / div), std::max (8, (h + div - 1) / div), false);
        makeTex (m_fade, std::max (8, (w + 1) / 2), std::max (8, (h + 1) / 2), false);
        m_tierFade = -1.0f;
        m_winW = w;
        m_winH = h;
        m_halfDiv = div;
        m_pendSeconds = 0.0f;
    }

    // Tiers 1 to 3 of the ink creature: one step of the buffer, then the composite (at half size and
    // upscaled on tier 1).
    void MonsterRenderer::renderInkTier (const MonsterFrame& f, int tier, unsigned int hostFbo, const int hostViewport[4])
    {
        const int W = hostViewport[2], H = hostViewport[3];
        // a change of tier: the tier being left draws once more and is kept, then the new one fades in under it
        bool captureOld = false;
        const bool canFade = glBlitFramebuffer != nullptr && m_fade.fbo != 0 && ! m_fadeUnsupported;
        if (m_prevTier >= 1 && tier != m_prevTier)
        {
            if (canFade && ! m_tierCapture)
            {
                m_tierCapture = captureOld = true;
                tier = m_prevTier;
            }
            else
            {
                inkResample (tier);
                m_tierCapture = false;
                m_tierFade = canFade ? 0.0f : -1.0f;
            }
        }
        else
            m_tierCapture = false;
        if (m_inkBuf[0].tex == 0 || m_inkBuf[1].tex == 0)
        {
            if (! allocInk (tier))
            {
                // no buffer after all: the single pass from here on
                freeInk();
                m_simOk = false;
                m_capTier = m_tier = 0;
                drawSinglePass (f, hostFbo, hostViewport);
                return;
            }
        }
        else if (m_inkBuf[0].w != inkWidthFor (tier))
            inkResample (tier);
        m_tier = tier;
        m_prevTier = tier;
        ensureInkWindowTargets (W, H, f.frameDt, lowDivisor());

        const int qi = m_queryIndex;
        const bool timing = m_timers && m_passTimer == nullptr && ! m_queryPending[(size_t) qi];
        if (timing)
            glBeginQuery (GL_TIME_ELAPSED, m_queries[(size_t) qi]);

        const float dt = m_fixedStep ? 1.0f / 30.0f : std::min (c_maxDt, std::max (c_minDt, f.frameDt));
        if (m_needsSeed)
        {
            inkStep (f, 1.0f / 30.0f, true);
            m_needsSeed = false;
            m_inkSimDt = 0.0f;
        }
        // (asleep and settled, the buffer steps every other frame)
        m_inkSimDt = std::min (0.1f, m_inkSimDt + dt);
        m_inkSimToggle = ! m_inkSimToggle;
        if (! f.asleep || m_inkSimToggle)
        {
            const int steps = m_inkSimDt > 1.0f / 24.0f ? 2 : 1;
            for (int s = 0; s < steps; ++s)
                inkStep (f, m_inkSimDt / (float) steps, false);
            m_inkSimDt = 0.0f;
        }
        inkBudgetRead();

        // the composite
        const bool half = tier == 1 && m_half.fbo != 0;
        use (P::composite, f);
        if (half)
        {
            auto& prog = m_progs[(size_t) P::composite];
            const float k = (float) m_half.h / (float) std::max (1, H);
            if (const int loc = prog.frameLoc[(size_t) U::uRes]; loc >= 0) glUniform2f (loc, (float) m_half.w, (float) m_half.h);
            if (const int loc = prog.frameLoc[(size_t) U::uPxScale]; loc >= 0) glUniform1f (loc, f.get (U::uPxScale) * k);
            if (const int loc = prog.frameLoc[(size_t) U::uRadius]; loc >= 0) glUniform1f (loc, f.get (U::uRadius) * k);
        }
        inkUniforms (dt, false);
        bindRaw (S::logo, m_logoTex);
        bindRaw (S::noise, m_noiseTex);
        bind (S::ink, m_inkBuf[m_inkCur]);
        draw (half ? &m_half : nullptr, half ? m_half.w : W, half ? m_half.h : H, "ink composite");
        if (half)
        {
            auto& prog = m_progs[(size_t) P::copy];
            glUseProgram (prog.id);
            m_cur = &prog;
            setScale (R::uSrcS, m_half, m_half.w, m_half.h);
            setR (R::uDither, 1.0f);   // (the upscale smooths the composite's dither away: the mist would band)
            bind (S::src, m_half);
            draw (nullptr, W, H, "upscale");
        }

        if (captureOld)
        {
            while (glGetError() != GL_NO_ERROR) {}
            glBindFramebuffer (GL_READ_FRAMEBUFFER, hostFbo);
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m_fade.fbo);
            glBlitFramebuffer (hostViewport[0], hostViewport[1], hostViewport[0] + W, hostViewport[1] + H, 0, 0, m_fade.w, m_fade.h,
                               GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer (GL_FRAMEBUFFER, hostFbo);
            if (glGetError() != GL_NO_ERROR)
                m_fadeUnsupported = true;   // (a multisampled host can't be scaled: tier changes switch outright)
        }
        else if (m_tierFade >= 0.0f && m_tierFade < 0.5f)
        {
            auto& prog = m_progs[(size_t) P::copy];
            glUseProgram (prog.id);
            m_cur = &prog;
            setScale (R::uSrcS, m_fade, m_fade.w, m_fade.h);
            setR (R::uDither, 0.0f);
            bind (S::src, m_fade);
            glEnable (GL_BLEND);
            glBlendColor (0.0f, 0.0f, 0.0f, 1.0f - m_tierFade / 0.5f);
            glBlendFunc (GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
            draw (nullptr, W, H, "tier crossfade");
            glDisable (GL_BLEND);
            m_tierFade += std::min (0.1f, std::max (1.0f / 120.0f, f.frameDt));
        }

        if (timing)
        {
            glEndQuery (GL_TIME_ELAPSED);
            m_queryPending[(size_t) qi] = true;
            m_queryIndex = (qi + 1) % (int) m_queries.size();
        }
    }

    void MonsterRenderer::readTimers()
    {
        if (! m_timers)
            return;
        for (size_t i = 0; i < m_queries.size(); ++i)
        {
            if (! m_queryPending[i])
                continue;
            GLuint available = 0;
            glGetQueryObjectuiv (m_queries[i], GL_QUERY_RESULT_AVAILABLE, &available);
            if (available != 0)
            {
                GLuint ns = 0;
                glGetQueryObjectuiv (m_queries[i], GL_QUERY_RESULT, &ns);
                m_freshGpuMs = (float) ns * 1.0e-6f;
                m_stats.gpuMs = m_freshGpuMs;
                m_queryPending[i] = false;
            }
        }
    }

    //==============================================================================================
    void MonsterRenderer::render (const MonsterFrame& f, unsigned int hostFbo, const int hostViewport[4])
    {
        if (! m_created)
            return;
        const auto t0 = std::chrono::steady_clock::now();
        ++m_frame;
        m_stats.draws = 0;
        m_hostFbo = hostFbo;
        std::copy (hostViewport, hostViewport + 4, m_hostVp);
        const int W = hostViewport[2], H = hostViewport[3];
        if (W <= 0 || H <= 0)
            return;

        // GL state as JUCE leaves it; ours is restored below
        GLboolean blendWas = glIsEnabled (GL_BLEND), scissorWas = glIsEnabled (GL_SCISSOR_TEST);
        glDisable (GL_BLEND);
        glDisable (GL_SCISSOR_TEST);
        glDisable (GL_DEPTH_TEST);
        if (m_core)
            glBindVertexArray (m_vao);
        else
        {
            glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
            glEnableVertexAttribArray (0);
            glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        }

        // the tier: measured cost steps it, never above what this GPU can do
        m_freshGpuMs = -1.0f;
        readTimers();
        const float lastGpu = m_freshGpuMs;   // each reading reaches the governor once
        const int changesBefore = m_governor.changes;
        m_governor.noteLoad ((double) W * (double) H);
        const int governed = m_governor.step (f.interval, f.targetInterval, lastGpu, f.dtClamped);
        if (m_governor.changes != changesBefore)
            rememberTier();
        // (measurement never leaves the buffer's tiers for the single pass, which costs more a pixel
        // than tier 1: the single pass is drawn where the buffer cannot run at all, and for a test)
        int tier = m_forcedTier >= 0 ? std::min (m_forcedTier, m_capTier) : std::min (governed, m_capTier);
        if (! m_simOk)
            tier = 0;

        if (tier == 0)
        {
            m_tier = 0;
            drawSinglePass (f, hostFbo, hostViewport);
        }
        else
            renderInkTier (f, tier, hostFbo, hostViewport);

        // restore what JUCE expects
        for (int i = 7; i >= 0; --i)
        {
            glActiveTexture ((GLenum) GL_TEXTURE0 + (GLenum) i);
            glBindTexture (GL_TEXTURE_2D, 0);
        }
        glUseProgram (0);
        m_cur = nullptr;
        if (m_core)
            glBindVertexArray (0);
        else
        {
            glDisableVertexAttribArray (0);
            glBindBuffer (GL_ARRAY_BUFFER, 0);
        }
        glBindFramebuffer (GL_FRAMEBUFFER, hostFbo);
        glViewport (hostViewport[0], hostViewport[1], hostViewport[2], hostViewport[3]);
        if (blendWas) glEnable (GL_BLEND); else glDisable (GL_BLEND);
        if (scissorWas) glEnable (GL_SCISSOR_TEST);
        m_stats.tier = m_tier;
        m_stats.cpuMs = std::chrono::duration<float, std::milli> (std::chrono::steady_clock::now() - t0).count();
    }

    void MonsterRenderer::collectObjectIds (ObjectIds& ids) const
    {
        for (const auto* t : { &m_half, &m_fade, &m_q0, &m_inkBuf[0], &m_inkBuf[1] })
        {
            if (t->tex != 0) ids.textures.push_back (t->tex);
            if (t->fbo != 0) ids.framebuffers.push_back (t->fbo);
        }
        if (m_noiseTex != 0) ids.textures.push_back (m_noiseTex);
        if (m_logoTex != 0) ids.textures.push_back (m_logoTex);
        for (const auto& p : m_progs)
            if (p.id != 0) ids.programs.push_back (p.id);
        if (m_vbo != 0) ids.buffers.push_back (m_vbo);
        if (m_inkPbo != 0) ids.buffers.push_back (m_inkPbo);
        // (each kind in its own list: a query's name asked after as a buffer's is never alive)
        for (auto q : m_queries)
            if (q != 0) ids.queries.push_back (q);
        if (m_vao != 0) ids.vertexArrays.push_back (m_vao);
        ids.timers = m_timers;
    }

    bool MonsterRenderer::readTarget (Target which, std::vector<float>& rgba, int& w, int& h)
    {
        if (! m_created || m_tier == 0 || which != Target::ink)
            return false;
        const Tex* t = &m_inkBuf[m_inkCur];
        w = t->w;
        h = t->h;
        if (t == nullptr || t->fbo == 0)
            return false;
        rgba.resize ((size_t) w * (size_t) h * 4);
        GLint prev = 0;
        glGetIntegerv (GL_FRAMEBUFFER_BINDING, &prev);
        glBindFramebuffer (GL_FRAMEBUFFER, t->fbo);
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, w, h, GL_RGBA, GL_FLOAT, rgba.data());
        glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) prev);
        if (m_inkBytes)
            for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            {
                rgba[i] *= 2.5f;
                rgba[i + 1] *= 2.5f;
                rgba[i + 2] = rgba[i + 2] * 2.0f - 1.0f;
                rgba[i + 3] *= 2.0f;
            }
        return true;
    }
}
