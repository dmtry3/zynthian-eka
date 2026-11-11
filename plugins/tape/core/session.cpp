#include "session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <unistd.h>

namespace alp {

namespace {
constexpr char kMagic[] = "eka-tape-session";
constexpr int kVersion = 1;
constexpr uint32_t kFitReserve = 64;  // кусков про запас после загрузки (запись и овердаб не упрутся сразу)
constexpr double kFull = 8388608.0;   // 2^23: полная шкала 24 бит

void put16(unsigned char* b, uint32_t v) {
  b[0] = (unsigned char)v;
  b[1] = (unsigned char)(v >> 8);
}
void put32(unsigned char* b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[i] = (unsigned char)(v >> (8 * i));
}
uint32_t get16(const unsigned char* b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8; }
uint32_t get32(const unsigned char* b) {
  return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

inline void put24(unsigned char* b, float x, float inv_gain) {
  double v = std::nearbyint((double)x * inv_gain * kFull);
  if (v > kFull - 1) v = kFull - 1;
  if (v < -kFull) v = -kFull;
  const int32_t s = (int32_t)v;
  b[0] = (unsigned char)s;
  b[1] = (unsigned char)(s >> 8);
  b[2] = (unsigned char)(s >> 16);
}
inline float get24(const unsigned char* b, float gain) {
  int32_t s = (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16);
  if (s & 0x800000) s -= 0x1000000;
  return (float)((double)s / kFull) * gain;
}

std::string dir_file(const char* dir, const char* file) {
  std::string p = dir ? dir : "";
  if (!p.empty() && p.back() != '/') p += '/';
  return p + file;
}
}  // namespace

int SessionDesc::used_tracks() const {
  int n = 0;
  for (const auto& t : this->t) n += t.used ? 1 : 0;
  return n;
}

void session_file_name(int k, int grid, char* out, size_t n) {
  if (grid < 1) grid = 4;
  std::snprintf(out, n, "part%d track%d.wav", k / grid + 1, k % grid + 1);
}

uint64_t session_new_id() {
  std::random_device rd;
  uint64_t a = ((uint64_t)rd() << 32) ^ rd();
  a ^= (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
  return a ? a : 1;
}

// Строки: заголовок, общие поля, затем по строке на каждую дорожку сетки:
// t <k> <used> <locked> <muted> <reversed> <synced> <len> <gain> <loop_beats> <rec_bpm> <b0> |<файл>
std::string session_to_text(const SessionDesc& d) {
  std::string s;
  char line[256];
  std::snprintf(line, sizeof line, "%s %d\nid %016llx\nsr %.17g\ngrid %d\npart %d\nactive %d\n", kMagic, kVersion,
                (unsigned long long)d.id, d.sr, d.grid, d.part, d.active);
  s += line;
  const int n = d.grid * d.grid;
  for (int k = 0; k < n && k < kSessionTracks; ++k) {
    const SessionTrack& t = d.t[k];
    std::snprintf(line, sizeof line, "t %d %d %d %d %d %d %llu %.9g %.17g %.17g %.17g |%s\n", k, t.used, t.locked,
                  t.muted, t.reversed, t.synced, (unsigned long long)t.len, (double)t.gain, t.loop_beats, t.rec_bpm,
                  t.b0, t.used ? t.file : "");
    s += line;
  }
  return s;
}

bool session_from_text(const char* txt, size_t n, SessionDesc& d) {
  d = SessionDesc();
  std::string s(txt, n);
  size_t at = 0;
  bool head = false, have_id = false, have_sr = false, have_grid = false;
  while (at < s.size()) {
    size_t e = s.find('\n', at);
    if (e == std::string::npos) e = s.size();
    std::string ln = s.substr(at, e - at);
    at = e + 1;
    if (ln.empty()) continue;
    if (!head) {
      int ver = 0;
      char mg[32] = {};
      if (std::sscanf(ln.c_str(), "%31s %d", mg, &ver) != 2 || std::strcmp(mg, kMagic) || ver != kVersion) return false;
      head = true;
      continue;
    }
    unsigned long long u = 0;
    double x = 0;
    int i = 0;
    if (std::sscanf(ln.c_str(), "id %llx", &u) == 1) { d.id = u; have_id = true; continue; }
    if (std::sscanf(ln.c_str(), "sr %lf", &x) == 1) { d.sr = x; have_sr = true; continue; }
    if (std::sscanf(ln.c_str(), "grid %d", &i) == 1) { d.grid = i; have_grid = true; continue; }
    if (std::sscanf(ln.c_str(), "part %d", &i) == 1) { d.part = i; continue; }
    if (std::sscanf(ln.c_str(), "active %d", &i) == 1) { d.active = i; continue; }
    if (ln[0] == 't') {
      int k, used, locked, muted, rev, synced;
      unsigned long long len;
      double gain, lb, bpm, b0;
      if (std::sscanf(ln.c_str(), "t %d %d %d %d %d %d %llu %lf %lf %lf %lf", &k, &used, &locked, &muted, &rev,
                      &synced, &len, &gain, &lb, &bpm, &b0) != 11)
        return false;
      if (k < 0 || k >= kSessionTracks) return false;
      SessionTrack& t = d.t[k];
      t.used = used != 0;
      t.locked = locked != 0;
      t.muted = muted != 0;
      t.reversed = rev != 0;
      t.synced = synced != 0;
      t.len = len;
      t.gain = (float)gain;
      t.loop_beats = lb;
      t.rec_bpm = bpm;
      t.b0 = b0;
      const size_t bar = ln.find('|');
      if (bar != std::string::npos) std::snprintf(t.file, sizeof t.file, "%s", ln.c_str() + bar + 1);
      if (t.used && (!t.file[0] || std::strchr(t.file, '/') || t.len == 0 || !(t.gain > 0))) return false;
      continue;
    }
  }
  if (!head || !have_id || !have_sr || !have_grid) return false;
  if (d.grid != 2 && d.grid != 4 && d.grid != 6) return false;
  if (d.part < 0 || d.part >= d.grid) d.part = 0;
  if (d.active < 0 || d.active >= d.grid) d.active = 0;
  return true;
}

uint32_t session_chunks(const SessionDesc& d) {
  uint32_t n = 0;
  for (const auto& t : d.t) {
    if (!t.used) continue;
    const uint32_t c = chunks_for(t.len);
    n += c + (c > kInline ? 1 : 0);  // кусок-таблица для длинных
  }
  return n;
}

int session_check(const SessionDesc& d, const char* dir, double sr, uint32_t pool_chunks) {
  if (std::fabs(d.sr - sr) > 0.5) return SS_E_RATE;
  for (int k = 0; k < kSessionTracks; ++k) {
    const SessionTrack& t = d.t[k];
    if (!t.used) continue;
    if (k >= d.grid * d.grid) return SS_E_FORMAT;
    if (chunks_for(t.len) > kMaxBufChunks) return SS_E_FORMAT;
    double fsr = 0;
    uint64_t frames = 0, off = 0;
    const int st = wav24_probe(dir_file(dir, t.file).c_str(), fsr, frames, off);
    if (st != SS_IDLE) return st;
    if (std::fabs(fsr - sr) > 0.5) return SS_E_RATE;
    if (frames != t.len) return SS_E_FORMAT;
  }
  if ((uint64_t)session_chunks(d) + kFitReserve > pool_chunks) return SS_E_MEMORY;
  return SS_IDLE;
}

float buf_peak(const Pool& p, int32_t buf, uint64_t len) {
  if (buf < 0) return 0.0f;
  const Buf& b = p.buf(buf);
  float m = 0.0f;
  for (uint32_t ci = 0; ci < chunks_for(len); ++ci) {
    const uint32_t id = p.entry(b, ci);
    if (id == kNone) continue;
    const Chunk* c = p.chunk(id);
    const uint64_t n = std::min<uint64_t>(kChunkFrames, len - (uint64_t)ci * kChunkFrames);
    for (uint64_t i = 0; i < n; ++i) m = std::fmax(m, std::fmax(std::fabs(c->l[i]), std::fabs(c->r[i])));
  }
  return m;
}

bool wav24_write(const char* path, const Pool& p, int32_t buf, uint64_t len, double sr, float gain,
                 std::atomic<uint64_t>* progress) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const uint64_t bytes = len * 6;
  unsigned char hd[44];
  std::memcpy(hd, "RIFF", 4);
  put32(hd + 4, (uint32_t)(36 + bytes));
  std::memcpy(hd + 8, "WAVEfmt ", 8);
  put32(hd + 16, 16);
  put16(hd + 20, 1);  // PCM
  put16(hd + 22, 2);
  put32(hd + 24, (uint32_t)std::lround(sr));
  put32(hd + 28, (uint32_t)std::lround(sr) * 6);
  put16(hd + 32, 6);
  put16(hd + 34, 24);
  std::memcpy(hd + 36, "data", 4);
  put32(hd + 40, (uint32_t)bytes);
  bool ok = std::fwrite(hd, 1, 44, f) == 44;
  std::vector<unsigned char> out((size_t)kChunkFrames * 6);
  const Buf& b = p.buf(buf);
  const float inv = gain > 0 ? 1.0f / gain : 1.0f;
  for (uint32_t ci = 0; ok && ci < chunks_for(len); ++ci) {
    const uint64_t n = std::min<uint64_t>(kChunkFrames, len - (uint64_t)ci * kChunkFrames);
    const uint32_t id = p.entry(b, ci);
    if (id == kNone) {
      std::memset(out.data(), 0, (size_t)n * 6);
    } else {
      const Chunk* c = p.chunk(id);
      for (uint64_t i = 0; i < n; ++i) {
        put24(&out[(size_t)i * 6], c->l[i], inv);
        put24(&out[(size_t)i * 6 + 3], c->r[i], inv);
      }
    }
    ok = std::fwrite(out.data(), 6, (size_t)n, f) == n;
    if (progress) progress->fetch_add(n, std::memory_order_relaxed);
  }
  // на карту, а не только в кэш: пресет ссылается на файл, после выключения он должен быть целым
  if (std::fflush(f) != 0 || fsync(fileno(f)) != 0) ok = false;
  if (std::fclose(f) != 0) ok = false;
  return ok;
}

int wav24_probe(const char* path, double& sr, uint64_t& frames, uint64_t& data_off) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return SS_E_MISSING;
  unsigned char h[12];
  int st = SS_E_FORMAT;
  if (std::fread(h, 1, 12, f) == 12 && !std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVE", 4)) {
    bool fmt_ok = false;
    unsigned char c[8];
    long pos = 12;
    while (std::fread(c, 1, 8, f) == 8) {
      const uint32_t sz = get32(c + 4);
      pos += 8;
      if (!std::memcmp(c, "fmt ", 4)) {
        unsigned char fm[16];
        if (sz < 16 || std::fread(fm, 1, 16, f) != 16) break;
        fmt_ok = get16(fm) == 1 && get16(fm + 2) == 2 && get16(fm + 12) == 6 && get16(fm + 14) == 24;
        sr = (double)get32(fm + 4);
      } else if (!std::memcmp(c, "data", 4)) {
        if (fmt_ok) {
          frames = sz / 6;
          data_off = (uint64_t)pos;
          st = SS_IDLE;
        }
        break;
      }
      pos += (long)sz + (sz & 1);
      if (std::fseek(f, pos, SEEK_SET) != 0) break;
    }
  }
  std::fclose(f);
  return st;
}

int wav24_read(const char* path, Pool& p, int32_t buf, uint64_t len, float gain, const std::function<bool()>& cancelled,
               std::atomic<uint64_t>* progress) {
  double sr = 0;
  uint64_t frames = 0, off = 0;
  int st = wav24_probe(path, sr, frames, off);
  if (st != SS_IDLE) return st;
  if (frames != len) return SS_E_FORMAT;
  FILE* f = std::fopen(path, "rb");
  if (!f) return SS_E_MISSING;
  if (std::fseek(f, (long)off, SEEK_SET) != 0) {
    std::fclose(f);
    return SS_E_READ;
  }
  if (!p.set_len(buf, len)) {
    std::fclose(f);
    return SS_E_MEMORY;
  }
  std::vector<unsigned char> in((size_t)kChunkFrames * 6);
  st = SS_IDLE;
  for (uint32_t ci = 0; ci < chunks_for(len); ++ci) {
    if (cancelled && cancelled()) {
      st = SS_IDLE;  // отмена — не ошибка; вызывающий сам знает, что отменил
      break;
    }
    const uint64_t n = std::min<uint64_t>(kChunkFrames, len - (uint64_t)ci * kChunkFrames);
    Chunk* c = p.ensure(buf, ci, false);
    if (!c) {
      st = SS_E_MEMORY;
      break;
    }
    if (std::fread(in.data(), 6, (size_t)n, f) != n) {
      st = SS_E_READ;
      break;
    }
    for (uint64_t i = 0; i < n; ++i) {
      c->l[i] = get24(&in[(size_t)i * 6], gain);
      c->r[i] = get24(&in[(size_t)i * 6 + 3], gain);
    }
    for (uint64_t i = n; i < kChunkFrames; ++i) c->l[i] = c->r[i] = 0.0f;
    if (progress) progress->fetch_add(n, std::memory_order_relaxed);
  }
  std::fclose(f);
  return st;
}

}  // namespace alp
