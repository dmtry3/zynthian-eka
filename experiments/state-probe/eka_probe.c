/* Eka Probe: временный плагин-эксперимент, не для игры.
 * Проверяет на Zynthian, можно ли хранить звук сессии в LV2-пресетах и снапшотах:
 * при сохранении пишет тестовый WAV размером size_mb через state:makePath (или mapPath),
 * при загрузке читает его обратно и сверяет контрольную сумму.
 * Всё пишет в лог /tmp/eka-probe.log: какие возможности дал хост, куда лёг файл, сколько длились
 * сохранение и загрузка и самый длинный перерыв между вызовами run() за это время (= тишина в звуке).
 * Проба 2: объявлен state:threadSafeRestore (jalv не должен останавливать звук на время restore),
 * у каждого сохранения свой файл.
 * Звук проходит насквозь. Чтение часов в run() допустимо только в эксперименте. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#define PROBE_URI "http://dmtry3.github.io/zynthian-eka/experiments/state-probe"
#define LOG_PATH "/tmp/eka-probe.log"

enum { P_IN_L, P_IN_R, P_OUT_L, P_OUT_R, P_SIZE };

typedef struct {
  float* ports[5];
  double sr;
  LV2_URID u_path, u_size, u_sum, u_time, a_path, a_int, a_long;
  volatile uint64_t runs;
  volatile uint64_t last_ns, max_gap_ns;
} Probe;

static uint64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void logf_(const char* fmt, ...) {
  char stamp[32];
  time_t t = time(NULL);
  strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&t));
  FILE* f = fopen(LOG_PATH, "a");
  va_list ap;
  if (f) {
    fprintf(f, "%s [pid %d tid %d] ", stamp, (int)getpid(), (int)gettid());
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
  }
  fprintf(stderr, "eka-probe: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static const void* feature(const LV2_Feature* const* fs, const char* uri) {
  for (int i = 0; fs && fs[i]; ++i)
    if (!strcmp(fs[i]->URI, uri)) return fs[i]->data ? fs[i]->data : (const void*)fs[i];
  return NULL;
}

static void log_features(const char* where, const LV2_Feature* const* fs) {
  char buf[2048] = "";
  for (int i = 0; fs && fs[i]; ++i) {
    const char* u = fs[i]->URI;
    const char* h = strrchr(u, '#');
    if (!h) h = strrchr(u, '/');
    strncat(buf, h ? h + 1 : u, sizeof buf - strlen(buf) - 2);
    strncat(buf, " ", sizeof buf - strlen(buf) - 1);
  }
  logf_("%s features: %s", where, buf[0] ? buf : "(none)");
}

/* Перерыв в звуке: сбросить максимум перед операцией, прочитать после. Если хост остановил звук
 * на всю операцию, run() не вызывается, поэтому учитываем и время с последнего run(). */
static void gap_reset(Probe* p) { p->max_gap_ns = 0; }
static double gap_ms(Probe* p) {
  uint64_t g = p->max_gap_ns, t = now_ns();
  if (p->last_ns && t - p->last_ns > g) g = t - p->last_ns;
  return g / 1e6;
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double sr, const char* bundle,
                              const LV2_Feature* const* fs) {
  (void)d;
  const LV2_URID_Map* map = feature(fs, LV2_URID__map);
  if (!map) return NULL;
  Probe* p = calloc(1, sizeof *p);
  p->sr = sr;
  p->u_path = map->map(map->handle, PROBE_URI "#wav");
  p->u_size = map->map(map->handle, PROBE_URI "#size_mb");
  p->u_sum = map->map(map->handle, PROBE_URI "#checksum");
  p->u_time = map->map(map->handle, PROBE_URI "#saved_at");
  p->a_path = map->map(map->handle, LV2_ATOM__Path);
  p->a_int = map->map(map->handle, LV2_ATOM__Int);
  p->a_long = map->map(map->handle, LV2_ATOM__Long);
  logf_("instantiate: sr=%.0f bundle=%s", sr, bundle);
  log_features("instantiate", fs);
  return p;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  if (port <= P_SIZE) ((Probe*)h)->ports[port] = data;
}

static void run(LV2_Handle h, uint32_t n) {
  Probe* p = h;
  uint64_t t = now_ns();
  if (p->last_ns && t - p->last_ns > p->max_gap_ns) p->max_gap_ns = t - p->last_ns;
  p->last_ns = t;
  p->runs++;
  if (p->ports[P_OUT_L] != p->ports[P_IN_L]) memcpy(p->ports[P_OUT_L], p->ports[P_IN_L], n * sizeof(float));
  if (p->ports[P_OUT_R] != p->ports[P_IN_R]) memcpy(p->ports[P_OUT_R], p->ports[P_IN_R], n * sizeof(float));
}

/* Тестовые данные: псевдослучайный шум, чтобы файл нельзя было сжать или подменить нулями. */
static uint32_t next_word(uint32_t* s) {
  *s ^= *s << 13;
  *s ^= *s >> 17;
  *s ^= *s << 5;
  return *s;
}

static void put32(unsigned char* b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static void put16(unsigned char* b, uint16_t v) { b[0] = v; b[1] = v >> 8; }

/* Пишет стерео WAV (32-bit float) примерно на mb мегабайт. Возвращает контрольную сумму или 0 при ошибке. */
static uint64_t write_wav(const char* path, int mb, double sr) {
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  uint64_t bytes = (uint64_t)mb * 1024 * 1024;
  bytes -= bytes % 8;
  unsigned char hd[44];
  memcpy(hd, "RIFF", 4); put32(hd + 4, (uint32_t)(36 + bytes)); memcpy(hd + 8, "WAVEfmt ", 8);
  put32(hd + 16, 16); put16(hd + 20, 3); put16(hd + 22, 2); put32(hd + 24, (uint32_t)sr);
  put32(hd + 28, (uint32_t)sr * 8); put16(hd + 32, 8); put16(hd + 34, 32);
  memcpy(hd + 36, "data", 4); put32(hd + 40, (uint32_t)bytes);
  fwrite(hd, 1, 44, f);
  enum { W = 1 << 18 };
  uint32_t* buf = malloc(W * sizeof *buf);
  uint32_t s = 2463534242u;
  uint64_t sum = 1469598103934665603ull, left = bytes / 4;
  while (left) {
    uint32_t k = left < W ? (uint32_t)left : W;
    for (uint32_t i = 0; i < k; ++i) {
      /* тихий шум в float: экспонента фиксирована, мантисса случайная */
      buf[i] = 0x3A000000u | (next_word(&s) & 0x007FFFFFu);
      sum = (sum ^ buf[i]) * 1099511628211ull;
    }
    if (fwrite(buf, 4, k, f) != k) { sum = 0; break; }
    left -= k;
  }
  free(buf);
  if (fflush(f) || fsync(fileno(f))) sum = 0; /* fsync: время до реальной записи на SD-карту */
  if (fclose(f)) sum = 0;
  return sum;
}

static uint64_t read_wav(const char* path, uint64_t* bytes_out) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  enum { W = 1 << 18 };
  uint32_t* buf = malloc(W * sizeof *buf);
  uint64_t sum = 1469598103934665603ull, bytes = 0;
  size_t k;
  fseek(f, 44, SEEK_SET);
  while ((k = fread(buf, 4, W, f)) > 0) {
    for (size_t i = 0; i < k; ++i) sum = (sum ^ buf[i]) * 1099511628211ull;
    bytes += k * 4;
  }
  free(buf);
  fclose(f);
  *bytes_out = bytes;
  return sum;
}

static LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh, uint32_t flags,
                             const LV2_Feature* const* fs) {
  Probe* p = h;
  int mb = p->ports[P_SIZE] ? (int)(*p->ports[P_SIZE] + 0.5f) : 100;
  if (mb < 1) mb = 1;
  logf_("save: start, size=%d MB, flags=0x%x", mb, flags);
  log_features("save", fs);
  const LV2_State_Make_Path* mk = feature(fs, LV2_STATE__makePath);
  const LV2_State_Map_Path* mp = feature(fs, LV2_STATE__mapPath);
  const LV2_State_Free_Path* fp = feature(fs, LV2_STATE__freePath);
  char* path = NULL;
  /* Своё имя у каждого сохранения: все пресеты плагина лежат в одной папке (проба 1 это показала). */
  static unsigned counter;
  char name[64];
  snprintf(name, sizeof name, "eka-probe-%lld-%u.wav", (long long)time(NULL), counter++);
  if (mk) {
    path = mk->path(mk->handle, name);
    logf_("save: makePath -> %s", path ? path : "(null)");
  } else {
    if (asprintf(&path, "/tmp/%s", name) < 0) path = NULL;
    logf_("save: no makePath, writing %s", path ? path : "(null)");
  }
  if (!path) return LV2_STATE_ERR_UNKNOWN;
  uint64_t runs0 = p->runs, t0 = now_ns();
  gap_reset(p);
  uint64_t sum = write_wav(path, mb, p->sr);
  double sec = (now_ns() - t0) / 1e9;
  logf_("save: wrote %s in %.2f s (%.1f MB/s), checksum=%016llx, run() calls during save=%llu, "
        "longest audio gap=%.1f ms", sum ? "OK" : "FAILED", sec, sec > 0 ? mb / sec : 0.0,
        (unsigned long long)sum, (unsigned long long)(p->runs - runs0), gap_ms(p));
  char* apath = mp ? mp->abstract_path(mp->handle, path) : NULL;
  logf_("save: mapPath %s, stored path=%s", mp ? "yes" : "no", apath ? apath : path);
  const char* sp = apath ? apath : path;
  store(sh, p->u_path, sp, strlen(sp) + 1, p->a_path, LV2_STATE_IS_POD);
  int32_t mbv = mb;
  store(sh, p->u_size, &mbv, sizeof mbv, p->a_int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  int64_t sv = (int64_t)sum, tv = (int64_t)time(NULL);
  store(sh, p->u_sum, &sv, sizeof sv, p->a_long, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  store(sh, p->u_time, &tv, sizeof tv, p->a_long, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  if (apath) { if (fp) fp->free_path(fp->handle, apath); else free(apath); }
  if (mk) { if (fp) fp->free_path(fp->handle, path); else free(path); } else free(path);
  logf_("save: done");
  return sum ? LV2_STATE_SUCCESS : LV2_STATE_ERR_UNKNOWN;
}

static LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh,
                                uint32_t flags, const LV2_Feature* const* fs) {
  Probe* p = h;
  uint64_t runs0 = p->runs, t0 = now_ns();
  gap_reset(p);
  logf_("restore: start, flags=0x%x", flags);
  log_features("restore", fs);
  size_t sz;
  uint32_t ty, fl;
  const void* v = retrieve(sh, p->u_path, &sz, &ty, &fl);
  if (!v) {
    logf_("restore: no wav path in state (preset/snapshot saved without file)");
    return LV2_STATE_SUCCESS;
  }
  const LV2_State_Map_Path* mp = feature(fs, LV2_STATE__mapPath);
  const LV2_State_Free_Path* fp = feature(fs, LV2_STATE__freePath);
  char* apath = mp ? mp->absolute_path(mp->handle, (const char*)v) : NULL;
  const char* path = apath ? apath : (const char*)v;
  const int64_t* want = retrieve(sh, p->u_sum, &sz, &ty, &fl);
  const int64_t* when = retrieve(sh, p->u_time, &sz, &ty, &fl);
  logf_("restore: stored=%s absolute=%s saved_at=%lld", (const char*)v, path, when ? (long long)*when : 0LL);
  uint64_t bytes = 0, sum = read_wav(path, &bytes);
  double sec = (now_ns() - t0) / 1e9;
  int ok = want && sum == (uint64_t)*want && bytes > 0;
  logf_("restore: read %.1f MB in %.2f s, checksum %s, run() calls during restore=%llu, longest audio gap=%.1f ms",
        bytes / 1048576.0, sec, ok ? "MATCH" : "MISMATCH/MISSING", (unsigned long long)(p->runs - runs0),
        gap_ms(p));
  if (apath) { if (fp) fp->free_path(fp->handle, apath); else free(apath); }
  return LV2_STATE_SUCCESS;
}

static const void* extension_data(const char* uri) {
  static const LV2_State_Interface st = {save, restore};
  return strcmp(uri, LV2_STATE__interface) ? NULL : &st;
}

static void cleanup(LV2_Handle h) {
  logf_("cleanup");
  free(h);
}

static const LV2_Descriptor desc = {PROBE_URI, instantiate, connect_port, NULL, run, NULL, cleanup, extension_data};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i == 0 ? &desc : NULL; }
