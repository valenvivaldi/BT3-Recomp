// [customsongs] See include/runtime/custom_songs.h.
#include "runtime/custom_songs.h"
#include "runtime/ps2_audio.h"
#include "runtime/ps2_host_audio.h"
#include "runtime/ps2_gs_psmct32.h"
#include "runtime/ps2_gs_psmt8.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/bt3gl/src/external/stb_truetype.h"

namespace customsongs
{
    namespace
    {
        std::vector<CustomSong> g_songs;
        bool g_loaded = false;
        std::filesystem::path g_exeDir;   // assets/fonts for the menu labels
        std::atomic<int> g_requested{-1};   // what the selector asked for (any thread)

        constexpr uint32_t kRate = 48000u, kChunkFrames = 1024u;

        std::string foldAscii(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        bool isAudioFile(const std::filesystem::path &p)
        {
            static const char *const kExt[] = {".mp3", ".flac", ".ogg", ".opus", ".wav", ".m4a", ".aac"};
            const std::string e = foldAscii(p.extension().string());
            for (const char *k : kExt)
                if (e == k)
                    return true;
            return false;
        }

        // Render-thread decoder state: one open file at a time.
        struct Decoder
        {
            AVFormatContext *fmt = nullptr;
            AVCodecContext *cc = nullptr;
            SwrContext *swr = nullptr;
            AVPacket *pkt = nullptr;
            AVFrame *frame = nullptr;
            int streamIndex = -1;
            std::vector<int16_t> pcm;   // interleaved stereo, decoded ahead of the device
            bool eof = false;

            void close()
            {
                if (frame) av_frame_free(&frame);
                if (pkt) av_packet_free(&pkt);
                if (swr) swr_free(&swr);
                if (cc) avcodec_free_context(&cc);
                if (fmt) avformat_close_input(&fmt);
                streamIndex = -1;
                pcm.clear();
                eof = false;
            }

            bool open(const std::filesystem::path &path)
            {
                close();
                if (avformat_open_input(&fmt, path.string().c_str(), nullptr, nullptr) < 0)
                    return false;
                if (avformat_find_stream_info(fmt, nullptr) < 0)
                    return false;
                const AVCodec *dec = nullptr;
                streamIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
                if (streamIndex < 0 || !dec)
                    return false;
                cc = avcodec_alloc_context3(dec);
                if (!cc || avcodec_parameters_to_context(cc, fmt->streams[streamIndex]->codecpar) < 0 ||
                    avcodec_open2(cc, dec, nullptr) < 0)
                    return false;
                AVChannelLayout stereo;
                av_channel_layout_default(&stereo, 2);
                if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, (int)kRate,
                                        &cc->ch_layout, cc->sample_fmt, cc->sample_rate, 0, nullptr) < 0 ||
                    swr_init(swr) < 0)
                    return false;
                pkt = av_packet_alloc();
                frame = av_frame_alloc();
                return pkt && frame;
            }

            void appendFrame(const AVFrame *f)
            {
                const int outMax = swr_get_out_samples(swr, f ? f->nb_samples : 0);
                if (outMax <= 0)
                    return;
                const size_t base = pcm.size();
                pcm.resize(base + size_t(outMax) * 2u);
                uint8_t *out = reinterpret_cast<uint8_t *>(pcm.data() + base);
                const int got = swr_convert(swr, &out, outMax, f ? (const uint8_t **)f->extended_data : nullptr,
                                            f ? f->nb_samples : 0);
                pcm.resize(base + size_t(std::max(got, 0)) * 2u);
            }

            // Decodes until `frames` stereo frames are buffered; the song loops at its end.
            void fill(size_t frames)
            {
                int restarts = 0;
                while (pcm.size() < frames * 2u)
                {
                    if (eof)
                    {
                        if (++restarts > 1) return;   // an empty or undecodable file: stop trying this call
                        av_seek_frame(fmt, streamIndex, 0, AVSEEK_FLAG_BACKWARD);
                        avcodec_flush_buffers(cc);
                        eof = false;
                    }
                    const int r = av_read_frame(fmt, pkt);
                    if (r < 0)
                    {
                        avcodec_send_packet(cc, nullptr);
                        while (avcodec_receive_frame(cc, frame) == 0)
                            appendFrame(frame);
                        appendFrame(nullptr);   // flush the resampler's tail
                        eof = true;
                        continue;
                    }
                    if (pkt->stream_index == streamIndex && avcodec_send_packet(cc, pkt) == 0)
                        while (avcodec_receive_frame(cc, frame) == 0)
                            appendFrame(frame);
                    av_packet_unref(pkt);
                }
            }
        };

        Decoder g_dec;
        ps2x_audio::Stream g_stream = 0;
        int g_playing = -1;   // render thread: the index g_dec holds
        bool g_started = false;   // render thread: playStream() issued for g_stream
    }

    void loadCatalog(const std::filesystem::path &exeDir)
    {
        if (g_loaded)
            return;
        g_loaded = true;
        g_exeDir = exeDir;
        if (const char *off = std::getenv("PS2X_CUSTOM_SONGS"); off && off[0] == '0')
            return;
        std::filesystem::path dir = exeDir / "songs";
        if (const char *p = std::getenv("PS2X_CUSTOM_SONGS_PATH"); p && p[0])
            dir = p;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && isAudioFile(it->path()))
                g_songs.push_back({it->path(), it->path().stem().string()});
        std::sort(g_songs.begin(), g_songs.end(),
                  [](const CustomSong &a, const CustomSong &b) { return foldAscii(a.title) < foldAscii(b.title); });
        std::fprintf(stderr, "[customsongs] %zu song(s) in %s\n", g_songs.size(), dir.string().c_str());
    }

    const std::vector<CustomSong> &songs() { return g_songs; }
    bool enabled() { return !g_songs.empty(); }

    void play(size_t index)
    {
        if (index < g_songs.size())
            g_requested.store((int)index, std::memory_order_release);
    }
    void stop() { g_requested.store(-1, std::memory_order_release); }
    bool active() { return g_requested.load(std::memory_order_acquire) >= 0; }
    int activeIndex() { return g_requested.load(std::memory_order_acquire); }

    void service()
    {
        const int want = g_requested.load(std::memory_order_acquire);
        if (want != g_playing)
        {
            if (g_stream)
            {
                ps2x_audio::closeStream(g_stream);
                g_stream = 0;
            }
            g_dec.close();
            g_playing = want;
            if (want >= 0)
            {
                if (!ps2x_audio::ready() || !g_dec.open(g_songs[size_t(want)].path))
                {
                    std::fprintf(stderr, "[customsongs] could not open %s\n", g_songs[size_t(want)].path.string().c_str());
                    g_dec.close();
                    return;
                }
                g_stream = ps2x_audio::openStream(kRate, 2, kChunkFrames);
                g_started = false;
                g_dec.fill(kChunkFrames * 2u);
                std::fprintf(stderr, "[customsongs] playing %s\n", g_songs[size_t(want)].title.c_str());
            }
        }
        if (!g_stream)
            return;
        // Same contract as the guest streams: hand over one whole chunk each time a sub-buffer frees up.
        static std::vector<int16_t> chunk(kChunkFrames * 2u);
        bool fed = false;
        while (ps2x_audio::streamProcessed(g_stream))
        {
            g_dec.fill(kChunkFrames);
            if (g_dec.pcm.size() < kChunkFrames * 2u)
                break;
            const float vol = PS2AudioBackend::masterVolume() * PS2AudioBackend::musicVolume() * PS2AudioBackend::curtainMute();
            for (size_t i = 0; i < chunk.size(); ++i)
                chunk[i] = (int16_t)std::clamp((int)(g_dec.pcm[i] * vol), -32768, 32767);
            g_dec.pcm.erase(g_dec.pcm.begin(), g_dec.pcm.begin() + (long)chunk.size());
            ps2x_audio::updateStream(g_stream, chunk.data(), kChunkFrames);
            fed = true;
        }
        if (fed && !g_started)
        {   // both sub-buffers are primed by the loop above before the device starts
            ps2x_audio::playStream(g_stream);
            g_started = true;
        }
    }

    namespace
    {
        constexpr uint32_t kMenuGlobal = 0x3b38d4u;
        constexpr uint32_t kTableOff = 0x3808u, kCountOff = 0x3810u, kListOff = 0x554u, kCursorOff = 0x4cu, kChosenOff = 0x50u;
        constexpr uint32_t kBuiltIn = 21u, kRandomId = 24u;
        constexpr uint32_t kTableAddr = 0x01F00100u;   // above kGuestHeapHardLimit (0x01F00000): the guest heap never hands it out
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        std::atomic<int> g_menuSong{-1};
        std::atomic<int> g_committed{-1};
        std::atomic<uint64_t> g_lastFight{0};

        uint32_t rd(const uint8_t *r, uint32_t a) { uint32_t v; std::memcpy(&v, r + (a & kRamMask), 4); return v; }
        void wr(uint8_t *r, uint32_t a, uint32_t v) { std::memcpy(r + (a & kRamMask), &v, 4); }
        bool inRam(uint32_t a, uint32_t span = 4u) { a &= kRamMask; return a >= 0x100000u && a + span <= 0x02000000u; }
    }

    int menuSong() { return g_menuSong.load(std::memory_order_acquire); }
    void noteFightUpdate(uint64_t frame) { g_lastFight.store(frame, std::memory_order_release); }

    void bgmMenuTick(uint8_t *rdram, uint64_t frame)
    {
        if (!rdram || g_songs.empty())
            return;
        const uint32_t n = (uint32_t)std::min<size_t>(g_songs.size(), 200u);
        int under = -1;
        const uint32_t base = rd(rdram, kMenuGlobal) & kRamMask;
        if (inRam(base, kCountOff + 4u))
        {
            const uint32_t table = rd(rdram, base + kTableOff) & kRamMask, count = rd(rdram, base + kCountOff);
            bool ours = table == kTableAddr && count == kBuiltIn + n;
            if (!ours && count == kBuiltIn && inRam(table, kBuiltIn * 4u) && rd(rdram, table) == 0u && rd(rdram, table + 4u) == 1u &&
                rd(rdram, table + 19u * 4u) == 19u && rd(rdram, table + 20u * 4u) == kRandomId)
            {   // the stock table: copy it, append one Random-id entry per song, and point the menu at the copy
                std::memcpy(rdram + kTableAddr, rdram + table, kBuiltIn * 4u);
                for (uint32_t i = 0; i < n; ++i)
                    wr(rdram, kTableAddr + (kBuiltIn + i) * 4u, kRandomId);
                wr(rdram, base + kTableOff, kTableAddr);
                wr(rdram, base + kCountOff, kBuiltIn + n);
                ours = true;
                std::fprintf(stderr, "[bgmmenu] frame %llu: BGM list extended %u -> %u (table %#x -> %#x)\n",
                             (unsigned long long)frame, kBuiltIn, kBuiltIn + n, table, kTableAddr);
            }
            if (ours)
            {
                const uint32_t list = rd(rdram, base + kListOff) & kRamMask;
                if (inRam(list, kChosenOff + 4u))
                {
                    const uint32_t cur = rd(rdram, list + kCursorOff), chosen = rd(rdram, list + kChosenOff);
                    if (cur >= kBuiltIn && cur < kBuiltIn + n)
                        under = (int)(cur - kBuiltIn);
                    // +0x50: the entry BGM Select confirmed (the fight reads table[chosen]; ours are Random, muted)
                    const int c = (chosen >= kBuiltIn && chosen < kBuiltIn + n) ? (int)(chosen - kBuiltIn) : -1;
                    if (g_committed.exchange(c, std::memory_order_acq_rel) != c)
                        std::fprintf(stderr, "[bgmmenu] frame %llu: chosen BGM entry %u%s%s\n", (unsigned long long)frame, chosen,
                                     c >= 0 ? " = " : "", c >= 0 ? g_songs[(size_t)c].title.c_str() : "");
                }
            }
        }
        const int prevUnder = g_menuSong.exchange(under, std::memory_order_acq_rel);
        if (under != prevUnder)
            std::fprintf(stderr, "[bgmmenu] frame %llu: cursor on %s\n", (unsigned long long)frame,
                         under >= 0 ? g_songs[(size_t)under].title.c_str() : "(built-in / closed)");

        // what plays: the committed song during a fight, else a preview while the cursor rests on a custom entry
        const bool fighting = frame < g_lastFight.load(std::memory_order_acquire) + 180u;
        const int committed = g_committed.load(std::memory_order_acquire);
        const int want = fighting ? committed : under;
        if (want >= 0) play((size_t)want); else stop();
    }

    namespace
    {
        // [bgmmenu] The labels live in 256x32 rows of a PSMT8 texture, four per upload: 32 KB sent as PSMCT32 256x32
        // (DBW 4) to the shared block 0x3000. The upload for ids 24..27 (Random, ???, two blanks) is recognised by its
        // content; while the cursor rests on a custom entry its Random row is redrawn with the song's title. The CLUT
        // index is the brightness (0 clear, 109 the plate, ~250..170 the lettering's top-to-bottom ramp, 255 the corners).
        constexpr uint64_t kRandomChunkHash = 0x1f4542cba131fe8dull;
        constexpr uint32_t kChunkBytes = 32768u, kLabelW = 256u, kLabelH = 32u;

        std::mutex g_labelMtx;
        int g_labelFor = -1;
        std::vector<uint8_t> g_labelPayload;   // the patched chunk for g_labelFor

        std::vector<uint32_t> utf8(const std::string &s)
        {
            std::vector<uint32_t> out;
            for (size_t i = 0; i < s.size();)
            {
                const unsigned char c = (unsigned char)s[i];
                const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
                uint32_t cp = len == 1 ? c : c & (0x7Fu >> len);
                for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3Fu);
                out.push_back(cp);
                i += len;
            }
            return out;
        }

        const stbtt_fontinfo *labelFont()
        {
            static std::vector<unsigned char> ttf;
            static stbtt_fontinfo info;
            static int ok = -1;
            if (ok < 0)
            {
                ok = 0;
                std::vector<std::filesystem::path> dirs;
                if (const char *a = std::getenv("PS2X_ASSETDIR"); a && a[0]) dirs.emplace_back(a);
                dirs.push_back(g_exeDir / "assets");
                dirs.push_back(g_exeDir / ".." / "Resources" / "assets");   // the macOS bundle
                for (const auto &d : dirs)
                {
                    std::ifstream f(d / "fonts" / "RussoOne-Regular.ttf", std::ios::binary);
                    if (!f) continue;
                    ttf.assign(std::istreambuf_iterator<char>(f), {});
                    if (!ttf.empty() && stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0)))
                    { ok = 1; break; }
                }
                if (!ok) std::fprintf(stderr, "[bgmmenu] RussoOne-Regular.ttf not found: custom entries keep the Random label\n");
            }
            return ok ? &info : nullptr;
        }

        // The title in the style of the stock plate, into a 256x32 brightness image.
        bool drawLabel(const std::string &title, uint8_t *img)
        {
            const stbtt_fontinfo *font = labelFont();
            if (!font) return false;
            constexpr int kX0 = 3, kY0 = 2, kY1 = 26, kPad = 3, kMaxW = (int)kLabelW - kX0 - 2 * kPad - 3;
            auto width = [&](const std::vector<uint32_t> &t, float sc)
            {
                int w = 0;
                for (size_t i = 0; i < t.size(); ++i)
                {
                    int adv, lsb; stbtt_GetCodepointHMetrics(font, (int)t[i], &adv, &lsb);
                    w += adv;
                    if (i + 1 < t.size()) w += stbtt_GetCodepointKernAdvance(font, (int)t[i], (int)t[i + 1]);
                }
                return (int)std::ceil(w * sc);
            };
            // file names are often "Artist - Song (Official Audio) - Artist": drop bracketed asides and repeated
            // segments, and when "Artist - Song" does not fit at a readable size keep the song alone
            std::string clean;
            for (size_t i = 0, depth = 0; i < title.size(); ++i)
            {
                const char c = title[i];
                if (c == '(' || c == '[') ++depth;
                else if ((c == ')' || c == ']') && depth) --depth;
                else if (!depth) clean += (c == '_') ? ' ' : c;
            }
            std::vector<std::string> segs;
            for (size_t a = 0; a <= clean.size();)
            {
                size_t b = clean.find(" - ", a);
                if (b == std::string::npos) b = clean.size();
                std::string seg = clean.substr(a, b - a);
                seg.erase(0, seg.find_first_not_of(' '));
                seg.erase(seg.find_last_not_of(' ') + 1);
                if (!seg.empty() && std::find(segs.begin(), segs.end(), seg) == segs.end()) segs.push_back(seg);
                a = b + 3;
            }
            std::string joined;
            for (const std::string &seg : segs) joined += (joined.empty() ? "" : " - ") + seg;
            std::vector<uint32_t> cps = utf8(joined.empty() ? title : joined);
            constexpr float kMaxPx = 23.0f, kMinPx = 15.0f;
            if (segs.size() >= 2 && width(cps, stbtt_ScaleForPixelHeight(font, kMinPx)) > kMaxW)
            {
                std::string rest;
                for (size_t i = 1; i < segs.size(); ++i) rest += (rest.empty() ? "" : " - ") + segs[i];
                cps = utf8(rest);
            }
            float px = kMaxPx, sc = stbtt_ScaleForPixelHeight(font, px);
            while (px > kMinPx && width(cps, sc) > kMaxW) { px -= 1.0f; sc = stbtt_ScaleForPixelHeight(font, px); }
            if (width(cps, sc) > kMaxW)
            {   // still too long at the smallest size: cut and close with an ellipsis
                while (cps.size() > 1 && width(cps, sc) + width({'.', '.', '.'}, sc) > kMaxW) cps.pop_back();
                while (!cps.empty() && cps.back() == ' ') cps.pop_back();
                cps.insert(cps.end(), {'.', '.', '.'});
            }
            // The stock plate's palette, read off the "Random" row: 0 and 95 clear, 96..109 the dark outline's edge
            // up to opaque, kInk the lettering's top-to-bottom gradient (one index per row), 1..47 the soft shadow.
            static constexpr uint8_t kInk[16] = {252, 248, 246, 243, 238, 234, 228, 223, 218, 212, 203, 195, 188, 181, 176, 172};
            int asc, desc, gap; stbtt_GetFontVMetrics(font, &asc, &desc, &gap);
            int cx0, cy0, cx1, cy1; stbtt_GetCodepointBox(font, 'H', &cx0, &cy0, &cx1, &cy1);
            const float capH = cy1 * sc;
            const int baseline = (int)std::lround(13.5f + capH * 0.5f);   // caps centred on the stock's rows 6..21
            constexpr int W = (int)kLabelW, H = (int)kLabelH;
            std::vector<float> cov(W * H, 0.0f);
            float pen = (float)(kX0 + kPad);
            for (size_t i = 0; i < cps.size(); ++i)
            {
                int gw, gh, gx, gy;
                unsigned char *bmp = stbtt_GetCodepointBitmapSubpixel(font, sc, sc, pen - std::floor(pen), 0.0f, (int)cps[i], &gw, &gh, &gx, &gy);
                for (int y = 0; y < gh; ++y)
                    for (int x = 0; x < gw; ++x)
                    {
                        const int X = (int)std::floor(pen) + gx + x, Y = baseline + gy + y;
                        if (X >= 0 && X < W && Y >= 0 && Y < H)
                            cov[Y * W + X] = std::max(cov[Y * W + X], bmp[y * gw + x] / 255.0f);
                    }
                stbtt_FreeBitmap(bmp, nullptr);
                int adv, lsb; stbtt_GetCodepointHMetrics(font, (int)cps[i], &adv, &lsb);
                pen += adv * sc;
                if (i + 1 < cps.size()) pen += stbtt_GetCodepointKernAdvance(font, (int)cps[i], (int)cps[i + 1]) * sc;
            }
            // the outline: ~3 texels around the glyphs, soft on its last texel
            constexpr int kR = 4;
            std::vector<float> line(W * H, 0.0f), shadow(W * H, 0.0f);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    float best = 1e9f;
                    for (int dy = -kR; dy <= kR; ++dy)
                        for (int dx = -kR; dx <= kR; ++dx)
                        {
                            const int X = x + dx, Y = y + dy;
                            if (X < 0 || X >= W || Y < 0 || Y >= H || cov[Y * W + X] < 0.5f) continue;
                            best = std::min(best, std::sqrt(float(dx * dx + dy * dy)));
                        }
                    line[y * W + x] = std::clamp(3.2f - best, 0.0f, 1.0f);
                }
            for (int y = 3; y < H - 1; ++y)   // the shadow: the outline pushed down-right, blurred
                for (int x = 3; x < W - 1; ++x)
                {
                    float sum = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) sum += line[(y - 2 + dy) * W + x - 2 + dx];
                    shadow[y * W + x] = sum / 9.0f;
                }
            int xEnd = kX0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    if (line[y * W + x] > 0.0f || shadow[y * W + x] > 0.0f) xEnd = std::max(xEnd, x);
            std::memset(img, 0, kLabelW * kLabelH);
            const float top = baseline - capH;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const float c = cov[y * W + x], o = line[y * W + x], s = shadow[y * W + x];
                    int v = (y >= kY0 && y <= kY1 + 2 && x >= kX0 && x <= xEnd) ? 95 : 0;
                    if (c >= 0.5f)
                    {   // the palette is not a ramp: only the stock lettering's own per-row indices are safe
                        const float t = std::clamp((y - top) / std::max(1.0f, capH), 0.0f, 1.0f);
                        v = kInk[std::min<int>(15, (int)std::lround(t * 15.0f))];
                    }
                    else if (o > 0.0f || c > 0.0f)
                        v = 96 + (int)std::lround(13.0f * std::max(o, c > 0.0f ? 1.0f : 0.0f));
                    else if (s > 0.05f)
                        v = std::max(1, (int)std::lround(44.0f * s));
                    img[y * W + x] = (uint8_t)v;
                }
            return true;
        }

        uint64_t fnv(const uint8_t *p, uint32_t n)
        {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
            return h;
        }
    }

    const uint8_t *filterGifPacket(const uint8_t *data, uint32_t size, std::vector<uint8_t> &scratch)
    {
        const int song = menuSong();
        if (song < 0 || size < kChunkBytes + 16u || (size_t)song >= g_songs.size())
            return data;
        for (uint32_t off = 0; off + 16u <= size;)
        {   // find the IMAGE tag carrying the Random chunk
            uint64_t lo; std::memcpy(&lo, data + off, 8);
            const uint32_t nloop = (uint32_t)(lo & 0x7FFFu), flg = (uint32_t)((lo >> 58) & 3u);
            const uint32_t nreg = (uint32_t)((lo >> 60) & 0xFu) ? (uint32_t)((lo >> 60) & 0xFu) : 16u;
            off += 16u;
            const uint32_t body = flg >= 2u ? nloop * 16u : (flg == 1u ? ((nloop * nreg * 8u + 15u) & ~15u) : nloop * nreg * 16u);
            if (body > size - off) return data;
            if (flg >= 2u && body == kChunkBytes && fnv(data + off, body) == kRandomChunkHash)
            {
                std::lock_guard<std::mutex> lk(g_labelMtx);
                if (g_labelFor != song)
                {
                    // un-swizzle: CT32 256x32 into a 32 KB page run, read back as PSMT8 256x128
                    std::vector<uint8_t> gs(kChunkBytes), img(kLabelW * kLabelH);
                    for (uint32_t y = 0; y < 32u; ++y)
                        for (uint32_t x = 0; x < 256u; ++x)
                            std::memcpy(&gs[GSPSMCT32::addrPSMCT32(0, 4, x, y)], data + off + (y * 256u + x) * 4u, 4);
                    if (!drawLabel(g_songs[(size_t)song].title, img.data()))
                        return data;
                    for (uint32_t y = 0; y < kLabelH; ++y)   // row 0 of the chunk is id 24's plate
                        for (uint32_t x = 0; x < kLabelW; ++x)
                            gs[GSPSMT8::addrPSMT8(0, 4, x, y)] = img[y * kLabelW + x];
                    g_labelPayload.resize(kChunkBytes);
                    for (uint32_t y = 0; y < 32u; ++y)
                        for (uint32_t x = 0; x < 256u; ++x)
                            std::memcpy(&g_labelPayload[(y * 256u + x) * 4u], &gs[GSPSMCT32::addrPSMCT32(0, 4, x, y)], 4);
                    g_labelFor = song;
                }
                scratch.assign(data, data + size);
                std::memcpy(scratch.data() + off, g_labelPayload.data(), kChunkBytes);
                return scratch.data();
            }
            off += body;
        }
        return data;
    }
}
