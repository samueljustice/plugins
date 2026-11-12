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
        // The tier each GPU settled on, for this process: a reopened editor (or a recreated context)
        // starts there instead of at the top, and keeps an upgrade lock-out. Fixed storage, so the
        // GL thread never allocates for it.
        struct TierMemory
        {
            struct Entry { char key[128] {}; int tier = -1; bool locked = false; };
            static std::mutex& lock() { static std::mutex m; return m; }
            static std::array<Entry, 4>& entries() { static std::array<Entry, 4> e; return e; }

            static bool recall (const char* key, int& tier, bool& locked)
            {
                const std::lock_guard<std::mutex> g (lock());
                for (auto& e : entries())
                    if (e.tier >= 0 && std::strncmp (e.key, key, sizeof (e.key) - 1) == 0)
                    {
                        tier = e.tier;
                        locked = e.locked;
                        return true;
                    }
                return false;
            }

            static void store (const char* key, int tier, bool locked)
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
            }
        };

        constexpr int c_inkReadTexels = 64 * 48;            // the budget's read: the largest buffer's third mip, at any aspect a view has
        constexpr int c_q3SimW = 256;
        constexpr float c_refRows = 78.0f;                  // the speeds below are tuned on a 78-row sim
        constexpr float c_atmoTintShare = 0.22f;           // how much of the stage's own hue the grade keeps
        constexpr float c_atmoHueLean = 3.0f;              // at most this many degrees of hue off the palette's
        constexpr float c_minDt = 1.0f / 120.0f, c_maxDt = 1.0f / 20.0f;

        const char* const c_rNames[] = { "uOutRes", "uSimTexel", "uSimRes", "uSimS", "uDt", "uFirst", "uDiss", "uSeedMode",
                                         "uWarm", "uDyeS", "uCFS", "uLightS", "uBloomS", "uSrcS", "uSrcTexel", "uDyeTexel",
                                         "uDyeRes", "uCFTexel", "uDyeOn", "uSteps", "uUpGain", "uDivK", "uDither", "uInkRes", "uInkStep", "uInkRule", "uInkTier" };
        const char* const c_sNames[] = { "uDye", "uV", "uE", "uCF", "uLight", "uNoise", "uBloom", "uLogoSdf", "uWc", "uP", "uDv", "uSrc", "uInkTex" };

        float clamp01 (float v) { return std::isfinite (v) ? std::min (1.0f, std::max (0.0f, v)) : 0.0f; }
        float smooth (float a, float b, float x) { const float t = clamp01 ((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
        float mixf (float a, float b, float t) { return a + (b - a) * t; }

        float hgNorm (float g, float c) { return (1.0f - g * g) / std::pow (std::max (1.0e-4f, 1.0f + g * g - 2.0f * g * c), 1.5f); }

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
    const MonsterRenderer::TierSpec& MonsterRenderer::spec (int tier)
    {
        //                               simW dye cf sun emit bloom jacobi cubic corr  half
        static const TierSpec q3 { 256, 2, 2, 6, 4, 3, 12, true, true, false };
        static const TierSpec q2 { 208, 2, 3, 5, 3, 3, 8, false, true, false };
        static const TierSpec q1 { 160, 2, 4, 3, 2, 2, 4, false, false, true };
        return tier >= 3 ? q3 : (tier == 2 ? q2 : q1);
    }

    void MonsterRenderer::sizesFor (int tier, int& simW, int& simH, int& dyeW, int& dyeH) const
    {
        const auto& s = spec (tier);
        simW = s.simW;
        simH = std::max (8, (int) std::lround ((float) m_simH * (float) s.simW / (float) c_q3SimW));
        dyeW = simW * s.dyeMul;
        dyeH = simH * s.dyeMul;
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
        if ((which == P::inkSinglePass || which == P::inkSim || which == P::inkComposite) && ! m_core && m_textureLod)
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
        m_simH = std::min (96, std::max (64, (int) std::lround ((float) c_q3SimW / m_aspect)));
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
        const P single = m_ink ? P::inkSinglePass : P::singlePass;
        if (! buildProgram (single, m_progs[(size_t) single]))
        {
            release();
            return false;
        }

        // Capabilities for the fluid tiers.
        GLint units = 0;
        glGetIntegerv (GL_MAX_TEXTURE_IMAGE_UNITS, &units);
        m_stats.maxUnits = units;
        const bool floats = m_core || (hasExtension ("GL_ARB_texture_float")
                                       && (hasExtension ("GL_ARB_color_buffer_float") || hasExtension ("GL_ARB_half_float_pixel")));
        bool ok = floats && units >= 8 && glActiveTexture != nullptr && glGenFramebuffers != nullptr && glCheckFramebufferStatus != nullptr;
        if (m_ink)
        {
            // the ink creature's tiers need a framebuffer texture with mips, read at a level of
            // detail; half floats only choose the buffer's format. None of the programs or targets
            // of the creature before it are made.
            ok = units >= 4 && m_textureLod && glActiveTexture != nullptr && glGenFramebuffers != nullptr
              && glCheckFramebufferStatus != nullptr && glGenerateMipmap != nullptr;
            for (const P which : { P::inkSim, P::inkComposite, P::copy })
                ok = ok && buildProgram (which, m_progs[(size_t) which]);
            if (ok && floats && ! m_inkBytesForced)
                buildProgram (P::probe, m_progs[(size_t) P::probe]);
        }
        else
        {
            for (int i = 0; ok && i < (int) P::numPrograms; ++i)
                if (i != (int) P::singlePass && i != (int) P::inkSinglePass && i != (int) P::inkSim && i != (int) P::inkComposite)
                    ok = buildProgram ((P) i, m_progs[(size_t) i]);
        }
        // (the single pass upscales its half-resolution frame with the copy program: built even when
        // the fluid tiers are not available)
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
        if (m_ink)
        {
            m_stats.probeOk = ok && m_progs[(size_t) P::probe].id != 0 && probeFloat();
            m_inkBytes = ! m_stats.probeOk;
        }
        else
        {
            m_stats.probeOk = ok && probeFloat();
            ok = m_stats.probeOk;
        }

        const auto* renderer = reinterpret_cast<const char*> (glGetString (GL_RENDERER));
        const auto* vendor = reinterpret_cast<const char*> (glGetString (GL_VENDOR));
        m_capTier = ok ? QualityGovernor::startTierFor (renderer, vendor) : 0;
        // (the ink creature's one pass is light enough for a fast GPU at the view's own size)
        m_inkFull = m_ink && QualityGovernor::startTierFor (renderer, vendor) >= 2;

        if (ok)
            ok = allocSim();
        if (! noiseRgba.empty() && noiseSize > 0)
        {
            Tex n;
            makeTex (n, noiseSize, noiseSize, false, true, noiseRgba.data());
            m_noiseTex = n.tex;
            if (m_ink && m_noiseTex != 0 && glGenerateMipmap != nullptr)
            {
                // the ink creature reads the noise at coarse levels (large eddies, cloud banks)
                glBindTexture (GL_TEXTURE_2D, m_noiseTex);
                glGenerateMipmap (GL_TEXTURE_2D);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glBindTexture (GL_TEXTURE_2D, 0);
            }
        }
        if (! logoRgba.empty() && logoSize > 0 && ! m_ink)
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
            int settled = -1;
            bool locked = false;
            if (TierMemory::recall (m_rendererKey, settled, locked))
                m_governor.resume (m_capTier, settled, locked);
            else
                m_governor.reset (m_capTier);
        }
        m_tier = m_forcedTier >= 0 ? std::min (m_forcedTier, m_capTier) : std::min (m_governor.tier, m_capTier);
        if (m_ink && m_tier >= 1 && ! allocInk (m_tier))
        {
            freeInk();
            m_simOk = false;
            m_capTier = m_tier = 0;
        }
        glBindFramebuffer (GL_FRAMEBUFFER, m_hostFbo);
        m_fadeUnsupported = m_fluidFreed = false;
        m_divDt = 0.0f;
        m_prevTier = -1;
        m_needsSeed = true;
        m_firstEmitter = true;
        m_fadeQ0 = m_tierFade = -1.0f;
        m_tierCapture = false;
        if (m_tier == 0 && m_capTier > 0)
            m_fadeQ0 = 0.5f;   // (settled on the single pass last time: no crossfade, the fluid's memory goes back)
        m_stats.startTier = m_capTier;
        m_stats.compileMs = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        m_created = true;
        return true;
    }

    bool MonsterRenderer::allocSim()
    {
        if (m_ink)
            return true;   // (the ink buffer is made for the tier that draws: allocInk)
        const int simW = c_q3SimW, simH = m_simH;
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i)
            ok = makeTex (m_E[i], simW, simH, true) && makeTex (m_V[i], simW, simH, true) && makeTex (m_P[i], simW, simH, true)
              && makeTex (m_dye[i], simW * 2, simH * 2, true);
        return ok && makeTex (m_Wc, simW, simH, true) && makeTex (m_Dv, simW, simH, true) && makeTex (m_light, simW * 2, simH * 2, true);
    }

    // Settled on the single pass: the fluid's sim and window targets go back to the driver (they are
    // made again, and the fluid reseeded, if the governor ever climbs back out).
    void MonsterRenderer::freeFluid()
    {
        for (auto* t : { &m_E[0], &m_E[1], &m_V[0], &m_V[1], &m_P[0], &m_P[1], &m_dye[0], &m_dye[1], &m_Wc, &m_Dv, &m_light,
                         &m_cf, &m_bloom[0], &m_bloom[1], &m_bloom[2], &m_bloom[3], &m_half, &m_fade })
            freeTex (*t);
        freeInk();
        m_winW = m_winH = m_pendW = m_pendH = 0;
        m_tierFade = -1.0f;
        m_tierCapture = false;
        m_prevTier = -1;
        m_fluidFreed = true;
    }

    void MonsterRenderer::rememberTier()
    {
        if (m_created && m_forcedTier < 0 && m_rendererKey[0] != 0)
            TierMemory::store (m_rendererKey, m_governor.tier, m_governor.upgradesLocked);
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
            for (auto* t : { &m_E[0], &m_E[1], &m_V[0], &m_V[1], &m_P[0], &m_P[1], &m_dye[0], &m_dye[1], &m_Wc, &m_Dv, &m_light,
                             &m_cf, &m_bloom[0], &m_bloom[1], &m_bloom[2], &m_bloom[3], &m_half, &m_fade, &m_q0 })
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
        m_fadeQ0 = m_tierFade = -1.0f;   // (a crossfade never outlives its textures)
        m_tierCapture = false;
        m_prevTier = -1;
        m_fadeUnsupported = m_fluidFreed = false;
    }

    //==============================================================================================
    void MonsterRenderer::prepare (MonsterFrame& f)
    {
        const float W = f.get (U::uRes, 0), H = f.get (U::uRes, 1);
        const float aspect = H > 0.0f ? W / H : m_aspect;
        f.set (U::uAspect, aspect);
        const auto& af = f.forces;

        int simW, simH, dyeW, dyeH;
        sizesFor (std::max (1, m_tier), simW, simH, dyeW, dyeH);
        const float rows = (float) simH;
        const float k = rows / c_refRows;   // texel-based forces follow the sim's resolution

        const float sleep = clamp01 (f.get (U::uSleep)), awake = 1.0f - sleep;
        const float jaw = std::max (0.0f, f.get (U::uJaw));
        const float* head = f.at (U::uHead);
        const float* evoHead = f.at (U::uEvoHead);
        const float* eyes = f.at (U::uEvoEyes);
        const float* maw = f.at (U::uEvoMaw);
        const float* ink = f.at (U::uEvoInk);
        const float* pal = f.at (U::uEvoPal);
        const float* form = f.at (U::uForm);
        const float* cloud = f.at (U::uCloud);
        // the character leans (fluid, beast, eldritch, deep; 0 neutral, up to 1) move the flow itself:
        // fluid runs smooth and laminar, beast churns, eldritch winds tight, deep is slow and heavy
        const float* chr = f.at (U::uEvoChar);
        const float cFluid = std::max (0.0f, chr[0]), cBeast = std::max (0.0f, chr[1]);
        const float cEld = std::max (0.0f, chr[2]), cDeep = std::max (0.0f, chr[3]);
        const float F = clamp01 (form[0]), Ffast = clamp01 (form[2]);
        const float Fe = smooth (0.0f, 0.35f, F), Fm = smooth (0.15f, 0.6f, F), Fx = smooth (0.5f, 1.0f, F);
        const float egg = clamp01 (f.get (U::uEgg));
        const float keep = 1.0f - egg;
        const float cran = evoHead[0], jawM = evoHead[2];
        const float size = f.get (U::uSize);
        const float sizeJ = size + jawM;
        const float heat = clamp01 (f.get (U::uRoarDrive) * (0.4f + 0.6f * f.get (U::uRoar)));
        const float level = f.get (U::uLevel);
        const float roarHold = f.get (U::uRoarHold);

        // the head transform and the mouth, as the shader computes them
        const float scale = 1.0f + std::min (2.0f, std::max (-1.0f, -f.get (U::uPitch) / 12.0f)) * 0.065f
                          + 0.014f * head[3] * (0.35f + sleep) + jaw * 0.025f * awake + head[1];
        const float offY = 0.025f + head[0];
        const auto py = [scale, offY] (float hy) { return hy * scale + offY; };
        const float eyeOuter = 0.135f * cloud[1] + 0.056f * mixf (1.0f, eyes[3], 0.5f);
        const float mw = std::min ((0.15f + 0.08f * size) * maw[0], eyeOuter * 0.92f) * (1.0f - 0.10f * jaw * awake);
        float open = std::max ((0.006f + std::pow (std::min (jaw, 1.05f), 1.6f) * (0.12f + 0.05f * sizeJ)) * awake, 0.003f);
        open = std::max (open * (1.0f - 0.85f * f.get (U::uGateClosed)), 0.003f);
        const float mY = -0.075f, mouthMid = mY - open * 0.28f, chinDrop = open * 0.85f;
        const float chinY = -0.29f - chinDrop - 0.03f * jawM;

        // the vortices: four mirrored round the face, two free, and the swirl's pair at the notch
        {
            std::array<std::array<float, 4>, 6> vort {};
            const float swirlAct = clamp01 (af.swirl);
            const float strength = mixf (1.0f, 0.5f, sleep) * (1.0f + 0.6f * clamp01 (roarHold / 0.45f + swirlAct))
                                 * (1.0f + 0.5f * cEld + 0.3f * cBeast - 0.3f * cDeep - 0.15f * cFluid);
            AudioForces::vortices (f.clock, aspect, strength, vort);
            for (int i = 0; i < 6; ++i)
                f.setAt (U::uVort, i, vort[(size_t) i][0], vort[(size_t) i][1], vort[(size_t) i][2], vort[(size_t) i][3]);
            const float sy = py (mixf (chinY, 0.36f * cran, clamp01 (f.get (U::uSwirlNotch))));
            const float ss = (10.0f + 15.0f * f.swirlKnob) * swirlAct / c_refRows * awake;
            f.setAt (U::uVort, 6, 0.32f, sy, 12.0f / c_refRows, ss);
            f.setAt (U::uVort, 7, -0.32f, sy, 12.0f / c_refRows, -ss);
        }

        // the weather: two octaves drifting slowly (integrated, so a change of speed never jumps)
        const float ambK = mixf (1.0f, 0.5f, sleep) * (1.0f + 0.5f * clamp01 (4.0f * std::abs (f.formationRate)))
                         * (1.0f - 0.15f * clamp01 (-f.get (U::uPitch) / 12.0f))
                         * (1.0f + 0.35f * cFluid - 0.35f * cDeep);
        {
            const double dt = std::min ((double) f.frameDt, 0.1);
            m_amb[0] = std::fmod (m_amb[0] + dt * 0.011 * ambK, 4096.0);
            m_amb[1] = std::fmod (m_amb[1] + dt * 0.004 * ambK, 4096.0);
            m_amb[2] = std::fmod (m_amb[2] - dt * 0.017 * ambK, 4096.0);
            m_amb[3] = std::fmod (m_amb[3] + dt * 0.009 * ambK, 4096.0);
            f.set (U::uAmb, (float) m_amb[0], (float) m_amb[1], (float) m_amb[2] + 13.7f, (float) m_amb[3] + 5.1f);
        }
        const float g = clamp01 (af.growl * f.get (U::uGrowl) * 2.0f);
        float eps = std::min (0.6f, 0.20f + 0.25f * g + 0.15f * heat + 0.10f * clamp01 (af.swirl));
        eps *= std::min (2.0f, std::max (0.45f, 1.0f + 0.9f * cBeast + 0.5f * cEld - 0.55f * cFluid - 0.35f * cDeep));
        eps = std::min (0.75f, mixf (eps, 0.12f, sleep));
        f.set (U::uSimK, 0.075f * ambK, 0.5f * (1.0f + 1.5f * g), eps * 9.0f * k, 0.6f);
        const float inky = cloud[0];
        f.set (U::uBuoy, 12.0f * k, (1.5f * inky + 1.0f * f.get (U::uLiquid)) * k,
               0.0054f * (0.3f + 0.7f * Fx), (0.5f + 1.5f * ink[1] / 1.6f) * k * F);
        const float fr = std::min (1.5f, std::max (-1.5f, f.formationRate / 0.25f));
        // (asleep the cloud is not drawn in onto what is left of it: it would pile into a smooth disc)
        // (and not while the ink flows into the logo and back: there the distance is capped, its slope is
        // noise, and the pull gathers the sky into dense, hard-edged pale discs)
        const float gatherK = (1.0f - 0.9f * sleep) * (1.0f - smooth (0.0f, 0.25f, egg));
        f.set (U::uGather, F * gatherK, 0.0f, 12.0f * fr * k * gatherK, py (-0.02f));
        f.set (U::uFormK, smooth (0.5f, 0.9f, F));
        const float fall = clamp01 (-f.formationRate / 0.25f);
        f.set (U::uGatherD, rows * ((0.06f * F + 0.20f * Ffast) * (1.0f - fall) * gatherK - 0.5f * 0.26f * fall), F, 0.0f, 1.0f);
        f.set (U::uInk, ink[0], ink[1], 3.0f / (2.0f * rows) + 0.15f * (1.0f - F), 4.5f);

        const float fcY = -0.13f - chinDrop * 0.6f - 0.03f * jawM;
        f.set (U::uFrill, py (fcY), (0.17f + 0.04f * sizeJ) * scale, scale, f.get (U::uGravelOn) * awake * k * keep);
        f.set (U::uEns, f.get (U::uEnsWidth) * k * awake, py (-0.05f), 0.0f, 0.0f);

        // the audio's pushes
        const float roarAmt = clamp01 (af.roar) * clamp01 (f.roarKnob);
        const float jet = (40.0f + 80.0f * heat) * roarAmt * k * awake;
        const float c35 = 0.819f, s35 = 0.574f;
        const float cornerY = py (mY - open * 0.3f);
        f.setAt (U::uSplat, 0, mw * scale, cornerY, 0.10f, jet);
        f.setAt (U::uSplatDir, 0, c35, -s35, 0.8f * heat * roarAmt, 0.0f);
        f.setAt (U::uSplat, 1, -mw * scale, cornerY, 0.10f, jet);
        f.setAt (U::uSplatDir, 1, -c35, -s35, 0.8f * heat * roarAmt, 0.0f);
        f.setAt (U::uSplat, 2, 0.0f, py (mouthMid), 0.10f, 0.5f * jet);
        f.setAt (U::uSplatDir, 2, 0.0f, -1.0f, 0.5f * heat * roarAmt, 0.0f);
        f.setAt (U::uSplat, 3, 0.0f, py (mouthMid - 0.04f), 0.08f, 25.0f * af.puff * k * awake);
        f.setAt (U::uSplatDir, 3, 0.0f, -1.0f, 0.0f, 0.3f * af.puff * awake);
        for (int i = 0; i < 2; ++i)
        {
            const float a = 6.2832f * hashf ((float) af.sparkSeed * 0.6180339f + 0.37f * (float) i);
            const float side = i == 0 ? 1.0f : -1.0f;
            const float hx = side * (0.18f + 0.15f * std::abs (std::cos (a))), hy = 0.04f + 0.30f * std::sin (a);
            const float nx = side * std::abs (std::cos (a)), ny = std::sin (a);
            f.setAt (U::uSplat, 4 + i, hx * scale, py (hy), 0.026f, 900.0f * af.clipSparks * k * awake);
            f.setAt (U::uSplatDir, 4 + i, nx, ny, 15.0f * af.clipSparks * awake, 0.0f);
        }
        {
            const float lq = f.get (U::uLiquid);
            const float drip = smooth (0.03f, 0.45f, lq) * awake * F;
            const float phase = hashf ((float) f.clock * 0.33f);
            const float yb = py (-0.27f - chinDrop - 0.05f - 0.25f * phase);
            f.setAt (U::uSplat, 6, 0.25f * scale, yb, 0.03f, 8.0f * drip * k);
            f.setAt (U::uSplatDir, 6, 0.0f, -1.0f, 0.0f, -0.5f * drip * (1.0f - phase));
            f.setAt (U::uSplat, 7, -0.25f * scale, yb, 0.03f, 8.0f * drip * k);
            f.setAt (U::uSplatDir, 7, 0.0f, -1.0f, 0.0f, -0.5f * drip * (1.0f - phase));
        }

        // divergence: the sub's onsets ring out from the throat; the limiter throbs at the temples
        f.setAt (U::uDiv, 0, 0.0f, py (mouthMid), 14.0f / c_refRows, af.subPulse * clamp01 (f.get (U::uSub) * 2.0f) * awake);
        const float throb = (f.get (U::uStrainPulse) - 0.5f) * 1.2f * clamp01 (f.get (U::uStrain)) * awake;
        f.setAt (U::uDiv, 1, 0.225f * scale, py (0.165f * cran), 0.05f, throb);
        f.setAt (U::uDiv, 2, -0.225f * scale, py (0.165f * cran), 0.05f, throb);

        // light: from above, leaning a little one way then the other over minutes (so neither side of
        // the blot is the lit one); the two-lobe phase for it
        {
            const float a = 1.5708f + 0.22f * (float) std::sin (6.283185307 * f.clock / 140.0);
            const float c = -0.2f;
            const float phase = 2.2f * (0.7f * hgNorm (0.75f, c) + 0.3f * hgNorm (-0.25f, c));
            f.set (U::uLightDir, std::cos (a), std::sin (a), phase, 1.0f);
        }

        // the eyes and the throat as emitters (the light pass carries them through the ink)
        const float glowK = pal[2];
        const float eI = ((0.75f + 1.7f * heat + 0.5f * level) * awake + 0.12f * sleep) * mixf (0.75f, 1.0f, std::min (glowK, 1.0f))
                       * (0.8f + 0.2f * glowK);
        const float eyeOpen = awake * (1.0f - clamp01 (f.get (U::uBlink)));
        const float eyeI = eI * (0.3f + 0.7f * eyeOpen) * Fe * keep;
        f.setAt (U::uEmit, 0, 0.135f * cloud[1] * scale, py (0.083f), 0.034f * scale * mixf (1.0f, eyes[3], 0.5f), 0.55f * eyeI);
        const auto bright = [Fe] (float x) { return smooth (0.15f, 0.4f, x) * Fe; };
        f.setAt (U::uEmit, 1, 0.075f * cloud[1] * scale, py (0.205f * cran), 0.020f * scale, 0.45f * eyeI * bright (eyes[0]));
        f.setAt (U::uEmit, 2, 0.215f * cloud[1] * scale, py (0.005f), 0.018f * scale, 0.45f * eyeI * bright (eyes[1]));
        f.setAt (U::uEmit, 3, 0.0f, py (0.25f * cran), 0.022f * scale, 0.25f * eyeI * bright (eyes[2]));   // (x = 0: counted on both sides)
        const float subPulse = f.get (U::uSub) * f.get (U::uSubLevel) * (0.5f + 0.5f * std::sin (f.get (U::uSubPhase) * 6.2832f * 3.0f));
        const float throatI = (heat * (0.4f + 0.6f * f.roarKnob) + 0.3f * subPulse + 0.25f * level + 0.15f) * awake * Fm * keep;
        const float throatR = (mw * 0.6f + open) * scale * (1.0f + 0.3f * clamp01 (roarHold / 0.45f));
        f.set (U::uThroat, 0.0f, py (mouthMid), std::max (0.02f, throatR), 0.8f * throatI);
        f.set (U::uFogD, 0.0f, clamp01 (eI * 0.5f) * Fe * keep, clamp01 (roarHold / 0.45f) * awake, 0.0f);
        f.set (U::uEyeP, 0.135f * cloud[1] * scale, py (0.083f), py (mouthMid), std::max (0.03f, mw * 0.75f * scale));

        // colours for the glows
        const float* out = f.at (U::uColOutput);
        const float* roarC = f.at (U::uColRoar);
        const float* voc = f.at (U::uColVocoder);
        const float hk = clamp01 (heat * 1.3f);
        float eyeCol[3] = { mixf (out[0], roarC[0], hk), mixf (out[1], roarC[1], hk), mixf (out[2], roarC[2], hk) };
        {
            // the glow's hue leans with the character (pale gold, ember red, cold violet-teal, deep
            // blue), less so while it roars
            const float lean[4][3] = { { 1.0f, 0.86f, 0.52f }, { 1.0f, 0.38f, 0.12f }, { 0.62f, 0.55f, 1.0f }, { 0.35f, 0.70f, 1.0f } };
            const float w[4] = { cFluid, cBeast, cEld, cDeep };
            for (int i = 0; i < 4; ++i)
            {
                const float t = 0.55f * clamp01 (w[i]) * (1.0f - 0.6f * hk);
                for (int c = 0; c < 3; ++c)
                    eyeCol[c] = mixf (eyeCol[c], lean[i][c] * std::max (eyeCol[0], std::max (eyeCol[1], eyeCol[2])), t);
            }
        }
        f.set (U::uEyeCol, eyeCol[0], eyeCol[1], eyeCol[2]);
        f.set (U::uEmitCol, eyeCol[0], eyeCol[1], eyeCol[2]);
        float meanBand = 0.0f;
        for (int i = 0; i < 32; i += 4)
            meanBand += f.get (U::uBands, i) * 0.125f;
        const float tk = 0.35f + 0.65f * heat;
        const float base[3] = { 0.20f, 0.015f, 0.03f };
        float thr[3];
        const float vk = (f.get (U::uGravelOn) > 0.0f ? 1.0f : 0.0f) * std::min (0.7f, meanBand * 1.5f);
        for (int c = 0; c < 3; ++c)
            thr[c] = mixf (mixf (base[c], roarC[c], tk), voc[c], vk);
        f.set (U::uThroatCol, thr[0], thr[1], thr[2]);

        f.set (U::uBloomK, 0.7f, 0.6f, 0.0f, 0.0f);
        f.set (U::uComp, 1.0f, 1.0f / rows, 3.0f, 1.7f);
        const float grainFrame = std::floor ((float) std::fmod (f.clock * 24.0, 100000.0));
        f.set (U::uGrain, hashf (grainFrame * 0.6180339f) * 256.0f, hashf (grainFrame * 0.7548776f) * 256.0f, 1.0f,
               0.35f + 0.25f * std::min (glowK, 1.5f));
        f.set (U::uSpark, (float) (af.sparkSeed % 997), clamp01 (af.clipSparks), 0.0f, 0.0f);

        // the atmosphere: the top mist layer's opacity and drift, and the stage's hue lean (its lit
        // cloud colour, normalised and mostly washed out: a subtle shift within the cool palette)
        {
            const float mistA = m_mist.step (f.frameDt, sleep, heat, level, f.clock);
            f.set (U::uAtmo, mistA, (float) m_mist.drift[0], (float) m_mist.drift[1], 1.0f);
            const float* lit = f.at (U::uColCloudLit);
            const float l = std::max (1.0e-3f, 0.3f * lit[0] + 0.59f * lit[1] + 0.11f * lit[2]);
            float tint[3];
            for (int c = 0; c < 3; ++c)
                tint[c] = mixf (1.0f, std::min (1.6f, std::max (0.4f, lit[c] / l)), c_atmoTintShare);
            limitAtmoTint (tint);
            f.set (U::uAtmoTint, tint[0], tint[1], tint[2]);
        }

        if (m_ink)
            prepareInk (f);
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
        const float dt = std::isfinite (f.frameDt) ? std::min (0.1f, std::max (0.0f, f.frameDt)) : 0.0f;
        const float sleep = clamp01 (f.get (U::uSleep)), awake = 1.0f - sleep;
        const float stage = clamp01 (f.get (U::uSeedB, 3));
        const float* chr = f.at (U::uEvoChar);
        const std::array<float, 4> chars { clamp01 (chr[0]), clamp01 (chr[1]), clamp01 (chr[2]), clamp01 (chr[3]) };
        // the roar's heat: the ROAR module working, or simply a loud voice
        const float heat = 1.0f - (1.0f - clamp01 (f.get (U::uRoarDrive) * (0.4f + 0.6f * f.get (U::uRoar))))
                                * (1.0f - 0.8f * clamp01 (f.inputLoud) * awake);
        const float level = clamp01 (f.get (U::uLevel));
        const float glide = clamp01 (f.seedGlide);

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
        // the weighted jaw opens the rift; the lips' syllables wobble the strands; a shut gate closes it
        const float jaw = std::min (1.0f, std::max (0.0f, f.get (U::uJaw)));
        // (and no rift until there is a body round it: a hole in the first shred on the fold is a ring)
        pose.maw[2] = CreatureAnatomy::opening (jaw, stage) * awake * (1.0f - 0.85f * clamp01 (f.get (U::uGateClosed)))
                    * smooth (0.45f, 0.9f, clamp01 (f.inkFormation));
        pose.maw[3] = clamp01 (pose.maw[3] + 0.35f * std::min (1.0f, std::max (-1.0f, f.get (U::uLip))));
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
        f.set (U::uMaw, pose.maw[0], pose.maw[1], pose.maw[2], pose.maw[3]);

        // ---- formation: the blot being glided to forms out of the smoke of the one before
        const float F = clamp01 (f.inkFormation) * (1.0f - 0.75f * std::sin (3.14159265f * glide) * (f.seedTo != f.seedFrom ? 1.0f : 0.0f));
        const float un = 1.0f - F;

        // ---- the flow: gain from the level and the roar; the characters change how it moves
        const float cFluid = chars[0], cBeast = chars[1], cEld = chars[2], cDeep = chars[3];
        auto& fl = m_inkFlow;
        // (the level moves with every syllable; the eddies follow it over a third of a second, so
        // a long carry does not shake)
        m_inkLevel += (level * awake - m_inkLevel) * (1.0f - std::exp (-dt / 0.35f));
        fl.gain = CreatureFlow::gainFor (m_inkLevel, heat) * pose.look.flow * (1.0f - 0.3f * cDeep);
        fl.smallGain = 1.0f + 0.4f * cBeast - 0.3f * cFluid;
        const float swirl = clamp01 (f.get (U::uSwirlAmt));
        // (half gathered, the bound pair turns harder: the ink arrives turning, but never wound into
        // an arc round the pair, which is a drawn curve under whatever is above it)
        fl.vortex = F * (0.02f + 0.03f * heat + 0.02f * cEld) + 0.12f * swirl * awake + 0.08f * F * un;
        fl.vortexRadius = 0.24f;
        fl.vortexX = 0.22f + 0.2f * clamp01 (f.get (U::uSwirlNotch));
        fl.vortexY = pose.centreY;
        const double k = 0.42 + 0.10 * (double) fl.gain / (double) (CreatureFlow::c_restGain * std::max (0.5f, pose.look.flow));
        const auto scroll = [dt] (double& phase, double rate) { phase = phase + rate * (double) dt; phase -= std::floor (phase); };
        scroll (m_inkPhase[0], 0.011 * k);
        scroll (m_inkPhase[1], 0.017 * k);
        scroll (m_inkPhase[2], -0.016 * k);
        scroll (m_inkPhase[3], 0.009 * k);
        scroll (m_inkPhase[4], 0.006);     // the far cloud's drift
        scroll (m_inkPhase[5], 0.002);
        scroll (m_inkPhase[6], -0.009);    // the near cloud's
        scroll (m_inkPhase[7], 0.004);
        scroll (m_inkPhase[8], 0.004);     // the carry's patches
        // (the jaw's speed is uneven frame to frame: the breath follows it over a fifth of a second)
        m_inkJet += (clamp01 (0.7f * heat + 0.5f * std::abs (f.get (U::uJawVel))) - m_inkJet) * (1.0f - std::exp (-dt / 0.2f));
        const float jet = clamp01 (m_inkJet);
        scroll (m_inkPhase[9], 0.05 + 0.25 * (double) jet);   // the breath rises
        m_inkClock = std::fmod (m_inkClock + (double) dt, 4096.0);
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
        const float reach = (0.045f + 0.02f * stage + 0.05f * heat) * pose.look.carry;
        f.set (U::uCarry, reach / speed, un * CreatureFlow::c_restGain / fl.gain, un, (2.0f + 2.0f * heat) * CreatureFlow::c_restGain / fl.gain);
        f.set (U::uInkF, F, heat, pose.look.feather, pose.look.density * (1.0f + 0.25f * heat));
        f.set (U::uInkO, pose.look.ox, pose.look.oy, (float) m_inkPhase[8], pose.look.ribbon);

        // ---- the cloud: its own picture asleep, folding with the blot as it forms; banks and gaps
        f.set (U::uCloudK, 0.04f + 0.61f * F, pose.look.bank * m_inkFrontCloud, 0.10f * m_inkFrontCloud, 0.45f * heat);
        f.set (U::uCloudO, (float) m_inkPhase[4], (float) m_inkPhase[5], (float) m_inkPhase[6] + 0.61f, (float) m_inkPhase[7] + 0.61f);
        {
            // the far cloud's two carries, half a cycle apart; each has no share as it starts again
            scroll (m_inkCloudPhase, 1.0 / 9.0);
            const float ph = (float) m_inkCloudPhase, phB = ph < 0.5f ? ph + 0.5f : ph - 0.5f;
            f.set (U::uCloudT, 9.0f * ph, 9.0f * phB, std::abs (2.0f * ph - 1.0f), CreatureFlow::c_restGain / std::max (0.1f, fl.gain));
        }

        // ---- the rift's light: by seed and by moment, from a dark tear to drifting lit breath
        const float t = (float) m_inkClock, ph = pose.look.ox * 40.0f;
        const float moment = m_inkMoment >= 0.0f ? clamp01 (m_inkMoment)
                                                 : smooth (-0.35f, 0.55f, std::sin (t * 0.31f + ph) * std::cos (t * 0.17f + 1.3f * ph));
        const float light = m_inkLight >= 0.0f ? clamp01 (m_inkLight) : pose.look.glow * moment * (0.45f + 0.55f * heat);
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
            const float drift = falling > 0.0f ? -0.10f * falling : 0.28f * rising + 0.05f * F;
            // the release's kick: small eddies that tear what was let go, for two seconds after
            m_inkKick = std::max (falling, m_inkKick * std::exp (-dt / 2.0f));
            m_inkFalling = falling;
            m_inkGather = drift > 0.0f ? 0.28f * rising / drift : 0.0f;
            // (awake, what the rim sheds is the pigment it was made of; let go, the ink spreads into smoke)
            m_inkSmoke = mixf (1.0f / c_inkYield, 0.9f, clamp01 (std::max (2.0f * un, 1.5f * falling)));
            // (the floor under the body comes in over seconds once it has formed: the gathering is
            // the pigment's alone)
            m_inkFloor += ((F > 0.9f ? 1.0f : 0.0f) - m_inkFloor) * (1.0f - std::exp (-dt / (F > 0.9f ? 2.0f : 0.3f)));
            const float kick = (0.25f + 0.75f * m_inkKick) * (0.3f + 0.7f * un) + 0.15f * heat;
            f.set (U::uHold, 0.6f + 2.4f * F, 1.2f + 1.3f * un, drift, 0.45f + 0.25f * stage + 0.2f * heat);
            f.set (U::uPig, c_inkAmbient, m_inkBudget, kick, 0.008f);
            m_inkState = { F, pose.maw[2], heat, level, f.get (U::uBreath), reach / speed, drift, kick, 0.0f, 0.0f };
        }

        // ---- the sub's onsets push the pigment outwards from the body once each; a clipped peak
        // throws a few beads of pure pigment off the rim (no more often than the flash limiter lets
        // a spark through)
        {
            const auto& af = f.forces;
            const float push = std::min (0.45f, 0.12f * af.subPulse * clamp01 (f.get (U::uSub) * 2.0f)) * awake * F;
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
                    float r = 0.04f;
                    while (r < 1.2f && CreatureAnatomy::sdf (pose, dx * r, pose.centreY + dy * r) < 0.0f)
                        r += 0.02f;
                    m_inkBead[(size_t) i] = { dx * r, pose.centreY + dy * r, dx, dy };
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
        const auto& ts = spec (m_tier);
        for (int i = 0; i < c_numFrameUniforms; ++i)
        {
            const GLint loc = prog.frameLoc[(size_t) i];
            if (loc < 0)
                continue;
            const auto& s = c_uniformSpecs[i];
            const float* v = f.at ((U) i);
            float tmp[4];
            if (i == (int) U::uComp || i == (int) U::uGatherD)
            {
                std::copy (v, v + 4, tmp);
                if (i == (int) U::uComp) tmp[0] = ts.cubic ? 1.0f : 0.0f;
                else tmp[3] = ts.corrected ? 1.0f : 0.0f;
                v = tmp;
            }
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
    void MonsterRenderer::ensureWindowTargets (int w, int h, float dt)
    {
        if (w == m_winW && h == m_winH)
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
        // the first frame allocates at once; a change of size waits until it has held for 200 ms
        if (m_winW != 0 && m_pendSeconds < 0.2f)
            return;

        const float s = std::min (1.0f, std::min (1600.0f / (0.5f * (float) w), 500.0f / (0.5f * (float) h)));
        const int cfW = std::max (8, (int) std::lround (0.5f * (float) w * s)), cfH = std::max (8, (int) std::lround (0.5f * (float) h * s));
        makeTex (m_cf, cfW, cfH, true);
        int bw = std::max (4, cfW / 2), bh = std::max (4, cfH / 2);
        for (auto& b : m_bloom)
        {
            makeTex (b, bw, bh, true);
            bw = std::max (2, bw / 2);
            bh = std::max (2, bh / 2);
        }
        makeTex (m_half, std::max (8, (w + 1) / 2), std::max (8, (h + 1) / 2), false);
        makeTex (m_fade, std::max (8, (w + 1) / 2), std::max (8, (h + 1) / 2), false);
        m_tierFade = -1.0f;   // (the kept frame is gone: a fade from the cleared texture would dip to black)
        m_winW = w;
        m_winH = h;
        m_pendSeconds = 0.0f;
    }

    void MonsterRenderer::simStep (const MonsterFrame& f, float dt, bool solvePressure, float emitDt, float divDt)
    {
        int simW, simH, dyeW, dyeH;
        sizesFor (m_tier, simW, simH, dyeW, dyeH);
        const auto& ts = spec (m_tier);
        const auto common = [&]
        {
            setR (R::uSimTexel, 1.0f / (float) simW, 1.0f / (float) simH);
            setR (R::uSimRes, (float) simW, (float) simH);
            setScale (R::uSimS, m_V[0], simW, simH);
            setR (R::uDt, dt);
        };
        const int cfDiv = ts.cfDiv;
        const float cs = std::min (1.0f, std::min ((float) m_cf.w / ((float) m_winW / (float) cfDiv), (float) m_cf.h / ((float) m_winH / (float) cfDiv)));
        const int cfW = std::min (m_cf.w, (int) std::lround ((float) m_winW / (float) cfDiv * cs));
        const int cfH = std::min (m_cf.h, (int) std::lround ((float) m_winH / (float) cfDiv * cs));

        // the creature at sim resolution, with its own velocity (measured over the whole frame: the
        // field moves once a frame, so a second substep keeps the first one's speed, not 0 and 2x)
        if (emitDt > 0.0f)
        {
            use (P::emitter, f);
            common();
            setR (R::uDt, emitDt);
            setScale (R::uCFS, m_cf, cfW, cfH);
            setR (R::uFirst, m_firstEmitter ? 1.0f : 0.0f);
            bind (S::cf, m_cf);
            bind (S::e, m_E[m_e]);
            draw (&m_E[1 - m_e], simW, simH, "emitter");
            m_e = 1 - m_e;
            m_firstEmitter = false;
        }

        use (P::advect, f);
        common();
        setR (R::uDiss, std::exp (-0.25f * dt));
        bind (S::v, m_V[m_v]);
        draw (&m_V[1 - m_v], simW, simH, "advect");
        m_v = 1 - m_v;

        use (P::curl, f);
        common();
        bind (S::v, m_V[m_v]);
        draw (&m_Wc, simW, simH, "curl");

        use (P::forces, f);
        common();
        setR (R::uSeedMode, 0.0f);
        setScale (R::uDyeS, m_dye[0], dyeW, dyeH);
        bind (S::v, m_V[m_v]);
        bind (S::wc, m_Wc);
        bind (S::e, m_E[m_e]);
        bind (S::dye, m_dye[m_d]);
        draw (&m_V[1 - m_v], simW, simH, "forces");
        m_v = 1 - m_v;

        if (solvePressure)
        {
            use (P::divergence, f);
            common();
            setR (R::uDivK, std::min (4.0f, std::max (0.0f, divDt) * 30.0f));
            bind (S::v, m_V[m_v]);
            draw (&m_Dv, simW, simH, "divergence");

            use (P::jacobi, f);
            common();
            bind (S::dv, m_Dv);
            for (int i = 0; i < ts.jacobi; ++i)
            {
                setR (R::uWarm, i == 0 ? 0.8f : 1.0f);
                bind (S::p, m_P[m_p]);
                draw (&m_P[1 - m_p], simW, simH, "jacobi");
                m_p = 1 - m_p;
            }

            use (P::gradient, f);
            common();
            bind (S::v, m_V[m_v]);
            bind (S::p, m_P[m_p]);
            draw (&m_V[1 - m_v], simW, simH, "gradient");
            m_v = 1 - m_v;
        }

        use (P::dye, f);
        common();
        setR (R::uSeedMode, 0.0f);
        setScale (R::uDyeS, m_dye[0], dyeW, dyeH);
        setScale (R::uCFS, m_cf, cfW, cfH);
        setR (R::uDyeTexel, 1.0f / (float) dyeW, 1.0f / (float) dyeH);
        setR (R::uDyeRes, (float) dyeW, (float) dyeH);
        bind (S::dye, m_dye[m_d]);
        bind (S::v, m_V[m_v]);
        bind (S::e, m_E[m_e]);
        bind (S::cf, m_cf);
        draw (&m_dye[1 - m_d], dyeW, dyeH, "dye");
        m_d = 1 - m_d;
    }

    void MonsterRenderer::seed (const MonsterFrame& f)
    {
        int simW, simH, dyeW, dyeH;
        sizesFor (m_tier, simW, simH, dyeW, dyeH);
        for (auto* t : { &m_P[0], &m_P[1], &m_E[0], &m_E[1] })
        {
            glBindFramebuffer (GL_FRAMEBUFFER, t->fbo);
            glViewport (0, 0, t->w, t->h);
            glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
            glClear (GL_COLOR_BUFFER_BIT);
        }
        use (P::forces, f);
        setR (R::uSimTexel, 1.0f / (float) simW, 1.0f / (float) simH);
        setR (R::uSimRes, (float) simW, (float) simH);
        setScale (R::uSimS, m_V[0], simW, simH);
        setR (R::uDt, 1.0f / 30.0f);
        setR (R::uSeedMode, 1.0f);
        setScale (R::uDyeS, m_dye[0], dyeW, dyeH);
        bind (S::v, m_V[m_v]);
        bind (S::wc, m_Wc);
        bind (S::e, m_E[m_e]);
        bind (S::dye, m_dye[m_d]);
        draw (&m_V[1 - m_v], simW, simH, "seed velocity");
        m_v = 1 - m_v;
        setR (R::uSeedMode, 0.0f);

        use (P::dye, f);
        setR (R::uSimTexel, 1.0f / (float) simW, 1.0f / (float) simH);
        setScale (R::uSimS, m_V[0], simW, simH);
        setR (R::uSeedMode, 1.0f);
        draw (&m_dye[1 - m_d], dyeW, dyeH, "seed dye");
        m_d = 1 - m_d;
        setR (R::uSeedMode, 0.0f);
        m_firstEmitter = true;
    }

    void MonsterRenderer::resampleTier (int from, int to)
    {
        int sw0, sh0, dw0, dh0, sw1, sh1, dw1, dh1;
        sizesFor (from, sw0, sh0, dw0, dh0);
        sizesFor (to, sw1, sh1, dw1, dh1);
        auto& prog = m_progs[(size_t) P::copy];
        glUseProgram (prog.id);
        m_cur = &prog;
        setR (R::uDither, 0.0f);
        setScale (R::uSrcS, m_V[m_v], sw0, sh0);
        bind (S::src, m_V[m_v]);
        draw (&m_V[1 - m_v], sw1, sh1, "resample velocity");
        m_v = 1 - m_v;
        setScale (R::uSrcS, m_dye[m_d], dw0, dh0);
        bind (S::src, m_dye[m_d]);
        draw (&m_dye[1 - m_d], dw1, dh1, "resample dye");
        m_d = 1 - m_d;
        for (auto* t : { &m_P[0], &m_P[1] })
        {
            glBindFramebuffer (GL_FRAMEBUFFER, t->fbo);
            glViewport (0, 0, t->w, t->h);
            glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
            glClear (GL_COLOR_BUFFER_BIT);
        }
        m_firstEmitter = true;
    }

    // The single pass's half-resolution target: grown to fit, never shrunk (a live resize must not
    // reallocate every frame; the active rectangle is the frame's own size).
    bool MonsterRenderer::ensureQ0Target (int w, int h)
    {
        if (m_q0HalfFailed)
            return false;
        const int hw = std::max (8, (w + 1) / 2), hh = std::max (8, (h + 1) / 2);
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
    // Without a framebuffer object it draws straight to the host at full size.
    void MonsterRenderer::drawSinglePass (const MonsterFrame& f, unsigned int hostFbo, const int vp[4], float alpha)
    {
        const bool half = ! (m_ink && isInkFullSize()) && ensureQ0Target (vp[2], vp[3]);
        const int hw = std::max (8, (vp[2] + 1) / 2), hh = std::max (8, (vp[3] + 1) / 2);
        const P single = m_ink ? P::inkSinglePass : P::singlePass;
        use (single, f);
        bindRaw (S::logo, m_logoTex);
        bindRaw (S::noise, m_noiseTex);
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
        const auto blendOn = [alpha]
        {
            if (alpha < 0.999f)
            {
                glEnable (GL_BLEND);
                glBlendColor (0.0f, 0.0f, 0.0f, alpha);
                glBlendFunc (GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
            }
        };
        if (! half)
            blendOn();
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
            blendOn();
            m_hostFbo = hostFbo;
            std::copy (vp, vp + 4, m_hostVp);
            draw (nullptr, vp[2], vp[3], "single pass upscale");
        }
        if (alpha < 0.999f)
            glDisable (GL_BLEND);
    }

    //==============================================================================================
    // The ink buffer: half the view (x >= 0), a ping-pong pair with mips; mirrored across the fold
    // by its wrap mode, so filtered reads at the fold are right and the ink's symmetry is exact.
    bool MonsterRenderer::makeInkTex (int index, int w, int h)
    {
        auto& t = m_inkBuf[index];
        if (m_inkTopFbo[index] != 0)
            glDeleteFramebuffers (1, &m_inkTopFbo[index]);
        m_inkTopFbo[index] = 0;
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
        glGenFramebuffers (1, &m_inkTopFbo[index]);
        glBindFramebuffer (GL_FRAMEBUFFER, m_inkTopFbo[index]);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, m_inkTopLevel);
        if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            glBindFramebuffer (GL_FRAMEBUFFER, m_hostFbo);
            glDeleteFramebuffers (1, &m_inkTopFbo[index]);
            m_inkTopFbo[index] = 0;   // (no budget on this driver: the gain stays at 1)
        }
        glBindFramebuffer (GL_FRAMEBUFFER, m_hostFbo);
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
            if (m_inkTopFbo[i] != 0)
                glDeleteFramebuffers (1, &m_inkTopFbo[i]);
            m_inkTopFbo[i] = 0;
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
        setR (R::uInkStep, fresh ? 1.0f : 0.0f, m_inkBytes ? 1.0f : 0.0f, hashf ((float) (m_frame % 4093u) * 0.6180339f) * 64.0f, 0.0f);
        setR (R::uInkRule, m_inkFloor, m_inkFalling, m_inkSmoke, m_inkGather);
        // the cloud by tier: three layers, the far one carried in two phases, and threads, from the second buffer tier up
        setR (R::uInkTier, m_tier >= 2 ? 3.0f : 2.0f, m_tier >= 2 ? 1.0f : 0.0f, m_tier >= 2 ? 1.0f : 0.0f, 0.0f);
    }

    // One step of the buffer (fresh: filled as the shape stands and the cloud's own pigment, no history).
    void MonsterRenderer::inkStep (const MonsterFrame& f, float dt, bool fresh)
    {
        use (P::inkSim, f);
        // (its pixel-sized measures follow the buffer: the whole view at the buffer's scale)
        auto& prog = m_progs[(size_t) P::inkSim];
        if (const int loc = prog.frameLoc[(size_t) U::uRes]; loc >= 0)
            glUniform2f (loc, 2.0f * (float) m_inkBuf[0].w, (float) m_inkBuf[0].h);
        inkUniforms (dt, fresh);
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
        const unsigned int srcTop = m_inkTopFbo[m_inkCur];
        m_inkBuf[m_inkCur] = Tex();
        m_inkTopFbo[m_inkCur] = 0;
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
        if (srcTop != 0)
            glDeleteFramebuffers (1, &srcTop);
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
            if ((m_frame & 15u) != 0 || m_inkTopFbo[m_inkCur] == 0 || m_inkTopW * m_inkTopH > c_inkReadTexels)
                return;
            glBindFramebuffer (GL_FRAMEBUFFER, m_inkTopFbo[m_inkCur]);
            glBindBuffer (GL_PIXEL_PACK_BUFFER, m_inkPbo);
            glPixelStorei (GL_PACK_ALIGNMENT, 1);
            glReadPixels (0, 0, m_inkTopW, m_inkTopH, GL_RGBA, GL_FLOAT, nullptr);
            glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
            glBindFramebuffer (GL_FRAMEBUFFER, m_hostFbo);
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
                const float step = m_inkTotal > 1.15f ? -0.25f * (m_inkTotal - 1.15f) : m_inkTotal < 0.85f ? 0.25f * (0.85f - m_inkTotal) : 0.1f * (1.0f - m_inkBudget);
                m_inkBudget = std::min (1.3f, std::max (0.5f, m_inkBudget + step));
            }
        }
    }

    // The half-size composite's target and the tier crossfade's kept frame (a change of size waits
    // until it has held for 200 ms, as the window targets of the creature before did).
    void MonsterRenderer::ensureInkWindowTargets (int w, int h, float dt)
    {
        if (w == m_winW && h == m_winH)
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
        if (m_winW != 0 && m_pendSeconds < 0.2f)
            return;
        makeTex (m_half, std::max (8, (w + 1) / 2), std::max (8, (h + 1) / 2), false);
        makeTex (m_fade, std::max (8, (w + 1) / 2), std::max (8, (h + 1) / 2), false);
        m_tierFade = -1.0f;
        m_winW = w;
        m_winH = h;
        m_pendSeconds = 0.0f;
    }

    // Tiers 1 to 3 of the ink creature: one step of the buffer, then the composite (at half size and
    // upscaled on tier 1).
    void MonsterRenderer::renderInkTier (const MonsterFrame& f, int tier, bool fading, unsigned int hostFbo, const int hostViewport[4])
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
                drawSinglePass (f, hostFbo, hostViewport, 1.0f);
                return;
            }
        }
        else if (m_inkBuf[0].w != inkWidthFor (tier))
            inkResample (tier);
        m_tier = tier;
        m_prevTier = tier;
        ensureInkWindowTargets (W, H, f.frameDt);

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
        use (P::inkComposite, f);
        if (half)
        {
            auto& prog = m_progs[(size_t) P::inkComposite];
            const float k = (float) m_half.h / (float) std::max (1, H);
            if (const int loc = prog.frameLoc[(size_t) U::uRes]; loc >= 0) glUniform2f (loc, (float) m_half.w, (float) m_half.h);
            if (const int loc = prog.frameLoc[(size_t) U::uPxScale]; loc >= 0) glUniform1f (loc, f.get (U::uPxScale) * k);
            if (const int loc = prog.frameLoc[(size_t) U::uRadius]; loc >= 0) glUniform1f (loc, f.get (U::uRadius) * k);
        }
        inkUniforms (dt, false);
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

        if (fading)
        {
            drawSinglePass (f, hostFbo, hostViewport, std::min (1.0f, m_fadeQ0 / 0.5f));
            m_fadeQ0 += std::min (0.1f, std::max (0.0f, f.frameDt));
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
        int governed = m_governor.step (f.interval, f.targetInterval, lastGpu, f.dtClamped);
        if (m_governor.changes != changesBefore)
            rememberTier();
        int want = m_forcedTier >= 0 ? std::min (m_forcedTier, m_capTier) : std::min (governed, m_capTier);
        if (! m_simOk)
            want = 0;
        if (want >= 1 && m_fadeQ0 >= 0.0f)
        {
            // back out of the single pass (measured there, not a capability limit): the fluid restarts
            m_fadeQ0 = -1.0f;
            if (m_fluidFreed)
            {
                m_fluidFreed = false;
                if (! allocSim())
                {
                    m_simOk = false;
                    m_capTier = 0;
                    want = 0;
                }
            }
            m_needsSeed = true;
            m_firstEmitter = true;
            m_prevTier = -1;
        }
        if (want == 0 && m_tier >= 1 && m_fadeQ0 < 0.0f && m_forcedTier < 0)
            m_fadeQ0 = 0.0f;   // into the single pass: keep the fluid running under a 0.5 s crossfade
        const bool fading = m_fadeQ0 >= 0.0f && m_fadeQ0 < 0.5f;
        int tier = fading ? std::max (1, m_tier) : want;
        if (m_fadeQ0 >= 0.5f)
        {
            tier = 0;
            if (! m_fluidFreed)
                freeFluid();
        }

        if (tier == 0)
        {
            m_tier = 0;
            drawSinglePass (f, hostFbo, hostViewport, 1.0f);
        }
        else if (m_ink)
            renderInkTier (f, tier, fading, hostFbo, hostViewport);
        else
        {
            // a change of tier: the old tier draws once more and is kept, then the new one fades in under it
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
                    resampleTier (m_prevTier, tier);
                    m_tierCapture = false;
                    m_tierFade = canFade ? 0.0f : -1.0f;
                }
            }
            else
                m_tierCapture = false;
            m_tier = tier;
            m_prevTier = tier;
            const auto& ts = spec (tier);
            ensureWindowTargets (W, H, f.frameDt);

            const int qi = m_queryIndex;
            const bool timing = m_timers && m_passTimer == nullptr && ! m_queryPending[(size_t) qi];
            if (timing)
                glBeginQuery (GL_TIME_ELAPSED, m_queries[(size_t) qi]);

            int simW, simH, dyeW, dyeH;
            sizesFor (tier, simW, simH, dyeW, dyeH);
            const float cs = std::min (1.0f, std::min ((float) m_cf.w / ((float) W / (float) ts.cfDiv), (float) m_cf.h / ((float) H / (float) ts.cfDiv)));
            const int cfW = std::min (m_cf.w, (int) std::lround ((float) W / (float) ts.cfDiv * cs));
            const int cfH = std::min (m_cf.h, (int) std::lround ((float) H / (float) ts.cfDiv * cs));

            const auto creatureField = [&] (bool dyeOn)
            {
                use (P::creatureField, f);
                setR (R::uOutRes, (float) cfW, (float) cfH);
                setScale (R::uDyeS, m_dye[0], dyeW, dyeH);
                setR (R::uDyeOn, dyeOn ? 1.0f : 0.0f);
                bind (S::dye, m_dye[m_d]);
                bindRaw (S::logo, m_logoTex);
                draw (&m_cf, cfW, cfH, "creature field");
            };

            float dt = m_fixedStep ? 1.0f / 30.0f : std::min (c_maxDt, std::max (c_minDt, f.frameDt));
            if (m_needsSeed)
            {
                creatureField (false);
                seed (f);
                creatureField (true);
                // (fewer warm-up steps on the cheaper tiers: the reopen hitch stays small where the GPU is weak)
                const int warm = m_fixedStep ? m_warmupSteps : (tier >= 3 ? 30 : tier == 2 ? 20 : 12);
                for (int i = 0; i < warm; ++i)
                    simStep (f, 1.0f / 30.0f, true, 1.0f / 30.0f, 1.0f / 30.0f);
                m_needsSeed = false;
                m_divDt = 0.0f;
            }

            creatureField (true);
            const int steps = dt > 1.0f / 24.0f ? 2 : 1;
            m_pressureToggle = ! m_pressureToggle;
            const bool pressure = ! f.asleep || m_pressureToggle;
            for (int s = 0; s < steps; ++s)
            {
                const float h = dt / (float) steps;
                m_divDt = std::min (0.2f, m_divDt + h);
                simStep (f, h, pressure, s == 0 ? dt : 0.0f, m_divDt);
                if (pressure)
                    m_divDt = 0.0f;
            }

            // light
            use (P::light, f);
            setScale (R::uDyeS, m_dye[0], dyeW, dyeH);
            setScale (R::uCFS, m_cf, cfW, cfH);
            setR (R::uDyeTexel, 1.0f / (float) dyeW, 1.0f / (float) dyeH);
            {
                const float n = (float) ts.sunSteps;
                const float first = 0.125f * 0.5f / (std::pow (1.5f, n) - 1.0f);
                setR (R::uSteps, n, (float) ts.emitTaps, first, 1.5f);
            }
            bind (S::dye, m_dye[m_d]);
            bind (S::cf, m_cf);
            draw (&m_light, dyeW, dyeH, "light");

            // bloom: a bright pass at a quarter, down and back up
            const int levels = ts.bloomLevels;
            int bw[4], bh[4];
            bw[0] = std::max (4, cfW / 2);
            bh[0] = std::max (4, cfH / 2);
            for (int i = 1; i < 4; ++i)
            {
                bw[i] = std::max (2, bw[i - 1] / 2);
                bh[i] = std::max (2, bh[i - 1] / 2);
            }
            use (P::bright, f);
            setScale (R::uCFS, m_cf, cfW, cfH);
            setScale (R::uLightS, m_light, dyeW, dyeH);
            setScale (R::uDyeS, m_dye[0], dyeW, dyeH);
            bind (S::cf, m_cf);
            bind (S::light, m_light);
            bind (S::dye, m_dye[m_d]);
            draw (&m_bloom[0], bw[0], bh[0], "bloom bright");
            use (P::down, f);
            for (int i = 1; i <= levels; ++i)
            {
                setScale (R::uSrcS, m_bloom[i - 1], bw[i - 1], bh[i - 1]);
                setR (R::uSrcTexel, 1.0f / (float) bw[i - 1], 1.0f / (float) bh[i - 1]);
                bind (S::src, m_bloom[i - 1]);
                draw (&m_bloom[i], bw[i], bh[i], "bloom down");
            }
            use (P::up, f);
            glEnable (GL_BLEND);
            glBlendFunc (GL_ONE, GL_ONE);
            for (int i = levels; i >= 1; --i)
            {
                setScale (R::uSrcS, m_bloom[i], bw[i], bh[i]);
                setR (R::uSrcTexel, 1.0f / (float) bw[i], 1.0f / (float) bh[i]);
                setR (R::uUpGain, 1.0f);
                bind (S::src, m_bloom[i]);
                draw (&m_bloom[i - 1], bw[i - 1], bh[i - 1], "bloom up");
            }
            glDisable (GL_BLEND);

            // the composite (half resolution on tier 1, then upscaled)
            use (P::composite, f);
            const bool half = ts.halfComposite;
            const int ow = half ? m_half.w : W, oh = half ? m_half.h : H;
            setR (R::uOutRes, (float) ow, (float) oh);
            setScale (R::uDyeS, m_dye[0], dyeW, dyeH);
            setScale (R::uLightS, m_light, dyeW, dyeH);
            setScale (R::uCFS, m_cf, cfW, cfH);
            setScale (R::uSimS, m_V[0], simW, simH);
            setScale (R::uBloomS, m_bloom[0], bw[0], bh[0]);
            setR (R::uDyeRes, (float) dyeW, (float) dyeH);
            setR (R::uCFTexel, 1.0f / (float) cfW, 1.0f / (float) cfH);
            setR (R::uSimTexel, 1.0f / (float) simW, 1.0f / (float) simH);
            bind (S::dye, m_dye[m_d]);
            bind (S::light, m_light);
            bind (S::cf, m_cf);
            bind (S::v, m_V[m_v]);
            bindRaw (S::noise, m_noiseTex);
            bind (S::bloom, m_bloom[0]);
            draw (half ? &m_half : nullptr, ow, oh, "composite");
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
                {
                    // (a multisampled host can't be scaled: from now on tier changes switch outright,
                    // and this one does on the next frame)
                    m_fadeUnsupported = true;
                }
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

            if (fading)
            {
                drawSinglePass (f, hostFbo, hostViewport, std::min (1.0f, m_fadeQ0 / 0.5f));
                m_fadeQ0 += std::min (0.1f, std::max (0.0f, f.frameDt));
            }

            if (timing)
            {
                glEndQuery (GL_TIME_ELAPSED);
                m_queryPending[(size_t) qi] = true;
                m_queryIndex = (qi + 1) % (int) m_queries.size();
            }
        }

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

    void MonsterRenderer::collectObjectIds (std::vector<unsigned int>& textures, std::vector<unsigned int>& framebuffers,
                                            std::vector<unsigned int>& programs, std::vector<unsigned int>& buffers) const
    {
        for (const auto* t : { &m_E[0], &m_E[1], &m_V[0], &m_V[1], &m_P[0], &m_P[1], &m_dye[0], &m_dye[1], &m_Wc, &m_Dv, &m_light,
                               &m_cf, &m_bloom[0], &m_bloom[1], &m_bloom[2], &m_bloom[3], &m_half, &m_fade, &m_q0, &m_inkBuf[0], &m_inkBuf[1] })
        {
            if (t->tex != 0) textures.push_back (t->tex);
            if (t->fbo != 0) framebuffers.push_back (t->fbo);
        }
        if (m_noiseTex != 0) textures.push_back (m_noiseTex);
        if (m_logoTex != 0) textures.push_back (m_logoTex);
        for (const auto& p : m_progs)
            if (p.id != 0) programs.push_back (p.id);
        for (auto fb : m_inkTopFbo)
            if (fb != 0) framebuffers.push_back (fb);
        if (m_vbo != 0) buffers.push_back (m_vbo);
        if (m_inkPbo != 0) buffers.push_back (m_inkPbo);
        for (auto q : m_queries)
            if (q != 0) buffers.push_back (q);
    }

    bool MonsterRenderer::readTarget (Target which, std::vector<float>& rgba, int& w, int& h)
    {
        if (! m_created || m_tier == 0)
            return false;
        int simW, simH, dyeW, dyeH;
        sizesFor (m_tier, simW, simH, dyeW, dyeH);
        const Tex* t = nullptr;
        switch (which)
        {
            case Target::creatureField:
            {
                const auto& ts = spec (m_tier);
                const float cs = std::min (1.0f, std::min ((float) m_cf.w / ((float) m_winW / (float) ts.cfDiv), (float) m_cf.h / ((float) m_winH / (float) ts.cfDiv)));
                t = &m_cf;
                w = std::min (m_cf.w, (int) std::lround ((float) m_winW / (float) ts.cfDiv * cs));
                h = std::min (m_cf.h, (int) std::lround ((float) m_winH / (float) ts.cfDiv * cs));
                break;
            }
            case Target::emitter: t = &m_E[m_e]; w = simW; h = simH; break;
            case Target::velocity: t = &m_V[m_v]; w = simW; h = simH; break;
            case Target::pressure: t = &m_P[m_p]; w = simW; h = simH; break;
            case Target::dye: t = &m_dye[m_d]; w = dyeW; h = dyeH; break;
            case Target::light: t = &m_light; w = dyeW; h = dyeH; break;
            case Target::bloom: t = &m_bloom[0]; w = m_bloom[0].w; h = m_bloom[0].h; break;
            case Target::ink: t = &m_inkBuf[m_inkCur]; w = t->w; h = t->h; break;
            case Target::numTargets: return false;
        }
        if (t == nullptr || t->fbo == 0)
            return false;
        rgba.resize ((size_t) w * (size_t) h * 4);
        GLint prev = 0;
        glGetIntegerv (GL_FRAMEBUFFER_BINDING, &prev);
        glBindFramebuffer (GL_FRAMEBUFFER, t->fbo);
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, w, h, GL_RGBA, GL_FLOAT, rgba.data());
        glBindFramebuffer (GL_FRAMEBUFFER, (GLuint) prev);
        if (which == Target::ink && m_inkBytes)
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
