// Офлайн-тесты сессий (этап 7а): 24-битные WAV, описание, сохранение с заморозкой, загрузка,
// вступление на такте, отмена, отказ без памяти. Запуск: make test. Файлы: tests/out/session_*/
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "../core/looper.h"
#include "../core/session.h"

using namespace alp;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (cond) {                                                    \
      ++g_pass;                                                    \
    } else {                                                       \
      ++g_fail;                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                              \
  } while (0)

static const double SR = 48000.0;
static const uint32_t BLOCK = 128;
static const double PI2 = 6.283185307179586;
static const uint64_t BAR = 96000;  // 120 BPM, 4/4

// Хост, как в test_looper.cpp: блоки по 128, рабочий поток сразу после блока.
struct Sim {
  std::unique_ptr<Looper> lp{new Looper()};
  Settings set;
  TimeInfo ti;
  double bpm = 120.0;
  uint64_t t = 0;
  double beat0 = 0.0;  // доля транспорта в кадре 0
  std::function<float(uint64_t)> in_l = [](uint64_t) { return 0.0f; };
  std::function<float(uint64_t)> in_r;
  std::vector<float> out_l, out_r;
  bool worker = true;
  std::unique_ptr<PeakShm> peaks{new PeakShm()};
  explicit Sim(size_t pool_mb = 64, bool quantize = true) {
    lp->init(SR, pool_mb << 20);
    lp->set_peaks(peaks.get());
    set.quantize = quantize;
    lp->set_settings(set);
    lp->set_monitor(false);
    ti.rolling = true;
  }
  double beats_now() const { return beat0 + (double)t * bpm / 60.0 / SR; }
  void apply() { lp->set_settings(set); }
  void block() {
    float il[BLOCK], ir[BLOCK], ol[BLOCK], orr[BLOCK];
    for (uint32_t i = 0; i < BLOCK; ++i) {
      il[i] = in_l(t + i);
      ir[i] = in_r ? in_r(t + i) : il[i] * 0.5f;
    }
    ti.beats_per_frame = bpm / 60.0 / SR;
    ti.beats = beats_now();
    lp->process(il, ir, ol, orr, BLOCK, ti);
    if (worker) {
      WorkMsg m;
      while (lp->take_work(m)) {
        WorkResp r;
        lp->do_work(m, r);
        lp->work_done(r);
      }
    }
    out_l.insert(out_l.end(), ol, ol + BLOCK);
    out_r.insert(out_r.end(), orr, orr + BLOCK);
    t += BLOCK;
  }
  void run(uint64_t frames) {
    for (uint64_t d = 0; d < frames; d += BLOCK) block();
  }
  void cmd(Cmd c) {
    ti.beats_per_frame = bpm / 60.0 / SR;
    ti.beats = beats_now();
    lp->set_time(ti);
    lp->command(c);
  }
  TrackView v(int i = 0) const { return lp->view(i); }
  float max_step(uint64_t from, uint64_t to) const {
    float m = 0;
    for (uint64_t i = from + 1; i < to && i < out_l.size(); ++i) {
      m = std::fmax(m, std::fabs(out_l[i] - out_l[i - 1]));
      m = std::fmax(m, std::fabs(out_r[i] - out_r[i - 1]));
    }
    return m;
  }
  float peak(uint64_t from, uint64_t to) const {
    float m = 0;
    for (uint64_t i = from; i < to && i < out_l.size(); ++i) m = std::fmax(m, std::fabs(out_l[i]));
    return m;
  }
  // до следующей черты такта (кадр, где доля переходит через целый такт)
  void run_to_bar() {
    const double b = beats_now();
    const double next = (std::floor(b / 4.0 + 1e-9) + 1.0) * 4.0;
    const uint64_t f = (uint64_t)std::ceil((next - beat0) * 60.0 / bpm * SR);
    if (f > t) run(f - t);
  }
};

static auto sine(double f, float a) {
  return [f, a](uint64_t t) { return a * (float)std::sin(PI2 * f * (double)t / SR); };
}

static std::string out_dir(const char* name) {
  mkdir("tests/out", 0755);
  std::string d = std::string("tests/out/") + name;
  mkdir(d.c_str(), 0755);
  for (int p = 1; p <= 6; ++p)
    for (int k = 1; k <= 6; ++k) {
      char f[256];
      std::snprintf(f, sizeof f, "%s/part%d track%d.wav", d.c_str(), p, k);
      unlink(f);
    }
  return d;
}

// Сохранить сессию так, как это делает save() обёртки (в одном потоке: блоки, пока не заморозится).
static int save_session(Sim& s, const std::string& dir, SessionDesc& d, uint64_t max_frames = 48000 * 30) {
  s.lp->session_request_freeze();
  for (uint64_t f = 0; f < max_frames && !s.lp->session_frozen(); f += BLOCK) s.block();
  if (!s.lp->session_frozen()) {
    s.lp->session_unfreeze(false, 0);
    return SS_E_TIMEOUT;
  }
  const int st = s.lp->session_save_audio(dir.c_str(), d);
  s.lp->session_unfreeze(st == SS_SAVED, d.id);
  s.block();
  return st;
}

// Звук дорожки k у двух лупёров совпадает с точностью 24 бит.
static float buf_diff(const Looper& a, int ka, const Looper& b, int kb) {
  if (a.track_sum(ka) < 0 || b.track_sum(kb) < 0 || a.track_len(ka) != b.track_len(kb)) return 1e9f;
  const Buf& x = a.pool().buf(a.track_sum(ka));
  const Buf& y = b.pool().buf(b.track_sum(kb));
  float m = 0;
  for (uint64_t f = 0; f < a.track_len(ka); ++f) {
    float l1, r1, l2, r2;
    a.pool().read(x, f, l1, r1);
    b.pool().read(y, f, l2, r2);
    m = std::fmax(m, std::fmax(std::fabs(l1 - l2), std::fabs(r1 - r2)));
  }
  return m;
}

// Две дорожки в Quantize: 1 такт (громче 0 дБ после двух овердабов) и 2 такта, Lock и mute.
static void record_two(Sim& s) {
  s.set.length[0] = 1;
  s.set.length[1] = 2;
  s.apply();
  s.in_l = sine(220.0, 0.6f);
  s.cmd(Cmd::Rec);   // взведена, пишет с черты
  s.run_to_bar();
  s.run(BAR + BLOCK * 8);  // закрылась сама, играет; дальше — овердаб тем же сигналом
  s.cmd(Cmd::Rec);
  s.run(BAR);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  s.in_l = sine(330.0, 0.3f);
  s.cmd(Cmd::TrackNext);
  s.cmd(Cmd::Rec);
  s.run_to_bar();
  s.run(2 * BAR + BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.cmd(Cmd::Mute);  // дорожка 2 — mute (на такте)
  s.cmd(Cmd::Lock);  // и Lock
  s.run_to_bar();
  s.run(BLOCK * 8);
}

static void test_wav_roundtrip() {
  Pool a, b;
  CHECK(a.init(8u << 20, 16));
  CHECK(b.init(8u << 20, 16));
  const uint64_t len = 40000;  // три куска, последний неполный
  const int32_t x = a.alloc_buf();
  a.set_len(x, len);
  uint32_t seed = 12345;
  for (uint64_t f = 0; f < len; ++f) {
    Chunk* c = a.ensure(x, (uint32_t)(f >> kChunkShift), false);
    seed = seed * 1664525u + 1013904223u;
    c->l[f & kChunkMask] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 3.0f;  // до ±1.5: громче 0 дБ
    c->r[f & kChunkMask] = (float)std::sin((double)f * 0.01) * 0.25f;
  }
  const std::string dir = out_dir("session_wav");
  const std::string path = dir + "/part1 track1.wav";
  const float pk = buf_peak(a, x, len);
  CHECK(pk > 1.4f && pk <= 1.5f);
  const float gain = pk / 0.999f;
  CHECK(wav24_write(path.c_str(), a, x, len, SR, gain, nullptr));
  double sr = 0;
  uint64_t frames = 0, off = 0;
  CHECK(wav24_probe(path.c_str(), sr, frames, off) == SS_IDLE);
  CHECK(sr == SR && frames == len && off == 44);
  const int32_t y = b.alloc_buf();
  CHECK(wav24_read(path.c_str(), b, y, len, gain, nullptr, nullptr) == SS_IDLE);
  float m = 0;
  for (uint64_t f = 0; f < len; ++f) {
    float l1, r1, l2, r2;
    a.read(a.buf(x), f, l1, r1);
    b.read(b.buf(y), f, l2, r2);
    m = std::fmax(m, std::fmax(std::fabs(l1 - l2), std::fabs(r1 - r2)));
  }
  std::printf("  wav24: max error %.3g (step %.3g)\n", (double)m, (double)gain / 8388608.0);
  CHECK(m <= gain / 8388608.0f);  // не больше шага 24 бит (с учётом gain)
  CHECK(wav24_read(path.c_str(), b, b.alloc_buf(), len + 1, 1.0f, nullptr, nullptr) == SS_E_FORMAT);
  CHECK(wav24_probe((dir + "/nope.wav").c_str(), sr, frames, off) == SS_E_MISSING);
}

static void test_text_roundtrip() {
  SessionDesc d;
  d.id = 0x0123456789abcdefull;
  d.sr = 48000;
  d.grid = 4;
  d.part = 2;
  d.active = 3;
  d.t[0].locked = true;
  d.t[5].used = true;
  d.t[5].muted = true;
  d.t[5].reversed = true;
  d.t[5].synced = true;
  d.t[5].len = 192000;
  d.t[5].gain = 1.25f;
  d.t[5].loop_beats = 8;
  d.t[5].rec_bpm = 123.456;
  d.t[5].b0 = 1234.0;
  session_file_name(5, 4, d.t[5].file, sizeof d.t[5].file);
  CHECK(std::strcmp(d.t[5].file, "part2 track2.wav") == 0);
  const std::string txt = session_to_text(d);
  SessionDesc e;
  CHECK(session_from_text(txt.data(), txt.size(), e));
  CHECK(e.id == d.id && e.grid == 4 && e.part == 2 && e.active == 3 && e.sr == 48000);
  CHECK(e.t[0].locked && !e.t[0].used);
  CHECK(e.t[5].used && e.t[5].muted && e.t[5].reversed && e.t[5].synced && e.t[5].len == 192000);
  CHECK(e.t[5].gain == 1.25f && e.t[5].loop_beats == 8 && e.t[5].rec_bpm == 123.456 && e.t[5].b0 == 1234.0);
  CHECK(std::strcmp(e.t[5].file, "part2 track2.wav") == 0);
  CHECK(e.used_tracks() == 1);
  CHECK(!session_from_text("garbage", 7, e));
  std::string bad = txt;
  bad.replace(bad.find("|part2"), 6, "|../x");  // путь из папки сессии наружу
  CHECK(!session_from_text(bad.data(), bad.size(), e));
}

// Сохранение и загрузка в другой лупер: тот же звук, состояние, вступление на такте без щелчков.
static void test_save_load() {
  Sim s;
  record_two(s);
  CHECK(s.v(0).state == 3 && s.v(1).state == 3 && s.v(1).muted && s.v(1).locked);
  const std::string dir = out_dir("session_basic");
  SessionDesc d;
  CHECK(save_session(s, dir, d) == SS_SAVED);
  CHECK(s.lp->session_status() == SS_SAVED);
  CHECK(d.used_tracks() == 2 && d.t[0].used && d.t[1].used && !d.t[2].used);
  CHECK(d.t[0].len == BAR && d.t[1].len == 2 * BAR);
  CHECK(d.t[0].gain > 1.0f);  // два овердаба 0,6: громче 0 дБ
  CHECK(d.t[1].muted && d.t[1].locked && d.t[0].synced && d.t[1].synced);
  CHECK(std::fabs(d.t[0].rec_bpm - 120.0) < 1e-6);
  struct stat st;
  CHECK(stat((dir + "/part1 track1.wav").c_str(), &st) == 0 && (uint64_t)st.st_size == 44 + BAR * 6);
  CHECK(stat((dir + "/part1 track2.wav").c_str(), &st) == 0);
  const std::string txt = session_to_text(d);

  Sim s2;
  s2.beat0 = (double)(7 * BAR + 30000) * 120.0 / 60.0 / SR;  // другая позиция транспорта
  s2.run(BAR / 2);
  SessionDesc e;
  CHECK(session_from_text(txt.data(), txt.size(), e));
  CHECK(session_check(e, dir.c_str(), SR, s2.lp->pool_chunks()) == SS_IDLE);
  s2.lp->session_load(e, dir.c_str());
  s2.block();  // принят, читается (рабочий поток в тесте — сразу)
  CHECK(s2.lp->session_status() == SS_LOADED);
  CHECK(s2.lp->session_id() == d.id);
  CHECK(buf_diff(*s.lp, 0, *s2.lp, 0) <= d.t[0].gain / 8388608.0f);
  CHECK(buf_diff(*s.lp, 1, *s2.lp, 1) <= d.t[1].gain / 8388608.0f);
  CHECK(s2.v(1).muted && s2.v(1).locked);
  CHECK(s2.v(0).state == 3 && s2.v(0).stopped);  // ждёт такта
  const uint64_t before = s2.t;
  s2.run_to_bar();
  CHECK(s2.peak(before, s2.t - BLOCK) < 1e-6f);  // до такта лупы молчат
  const uint64_t enter = s2.t;
  s2.run(BAR);
  CHECK(!s2.v(0).stopped);
  CHECK(s2.peak(enter, s2.t) > 0.5f);  // играет
  CHECK(s2.max_step(enter - BLOCK, s2.t) < 0.1f);  // вступление по огибающей, без щелчка
  // фаза: самая длинная (2 такта) начала цикл на черте — ещё через такт снова у начала
  s2.run_to_bar();
  CHECK(s2.v(1).pos < 0.01f || s2.v(1).pos > 0.99f);
}

// Save во время овердаба ждёт его конца; пока ждёт — новая запись не начинается;
// заморожено — Undo и Reset игнорируются.
static void test_save_waits_and_blocks() {
  Sim s;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(220.0, 0.4f);
  s.cmd(Cmd::Rec);
  s.run_to_bar();
  s.run(BAR + BLOCK * 8);
  s.cmd(Cmd::Rec);  // овердаб
  s.run(BLOCK * 4);
  CHECK(s.v(0).state == 4);
  s.lp->session_request_freeze();
  s.run(BAR / 2);
  CHECK(!s.lp->session_frozen());  // ждёт конца овердаба
  s.cmd(Cmd::TrackNext);  // уход с дорожки закрывает овердаб
  s.cmd(Cmd::Rec);        // новая запись на пустой дорожке — нет, Save ждёт
  s.run(BLOCK * 16);
  CHECK(s.v(1).state == 0);
  CHECK(s.lp->session_frozen());
  s.cmd(Cmd::TrackPrev);
  const uint32_t layers = s.v(0).layers;
  s.cmd(Cmd::Undo);
  s.cmd(Cmd::ResetTrack);
  s.cmd(Cmd::ResetAll);
  s.run(BLOCK * 8);
  CHECK(s.v(0).state == 3 && s.v(0).layers == layers);  // ничего не изменилось
  SessionDesc d;
  const std::string dir = out_dir("session_wait");
  CHECK(s.lp->session_save_audio(dir.c_str(), d) == SS_SAVED);
  s.lp->session_unfreeze(true, d.id);
  s.run(BLOCK * 2);
  CHECK(!s.lp->session_frozen());
  s.cmd(Cmd::Undo);  // после сохранения снова работает
  s.run(BLOCK * 8);
  CHECK(s.v(0).layers == layers - 1);
}

// Zynthian загружает пресет сразу после сохранения: та же сессия, звук не менялся — не грузим.
// Если звук изменился — грузим (лупы возвращаются к сохранённому).
static void test_skip_same() {
  Sim s;
  record_two(s);
  const std::string dir = out_dir("session_skip");
  SessionDesc d;
  CHECK(save_session(s, dir, d) == SS_SAVED);
  const int32_t sum0 = s.lp->track_sum(0);
  const uint64_t from = s.t;
  s.lp->session_load(d, dir.c_str());
  s.run(BAR);
  CHECK(s.lp->track_sum(0) == sum0);           // тот же буфер: не перегружали
  CHECK(s.lp->session_status() == SS_SAVED);
  CHECK(s.peak(from, s.t) > 0.3f && s.max_step(from, s.t) < 0.1f);  // играет без перерыва
  // меняем звук (undo овердаба) и грузим ту же сессию: вернулся сохранённый звук
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 16);
  s.lp->session_load(d, dir.c_str());
  s.run(BLOCK * 64);
  CHECK(s.lp->session_status() == SS_LOADED);
  CHECK(s.lp->track_sum(0) >= 0 && s.lp->track_len(0) == BAR);
}

// Транспорт стоит: сессия загружается молча и вступает при пуске, в фазе от точки пуска.
static void test_load_stopped() {
  Sim s;
  record_two(s);
  const std::string dir = out_dir("session_stopped");
  SessionDesc d;
  CHECK(save_session(s, dir, d) == SS_SAVED);
  Sim s2;
  s2.ti.rolling = false;
  s2.run(BLOCK * 8);
  s2.lp->session_load(d, dir.c_str());
  s2.run(BAR);
  CHECK(s2.lp->session_status() == SS_LOADED);
  CHECK(s2.peak(0, s2.t) < 1e-6f);
  CHECK(s2.v(0).state == 3 && s2.v(0).stopped);
  // пуск транспорта с начала
  s2.beat0 = -(double)s2.t * 120.0 / 60.0 / SR;  // доля 0 — сейчас
  s2.ti.rolling = true;
  const uint64_t go = s2.t;
  s2.run(BAR / 2);
  CHECK(!s2.v(0).stopped);
  CHECK(s2.peak(go, s2.t) > 0.5f);
  CHECK(s2.max_step(go, s2.t) < 0.1f);
  CHECK(s2.v(1).pos > 0.24f && s2.v(1).pos < 0.26f);  // 2-тактовая: прошла четверть цикла
}

// Free: дорожка без фазы вступает на такте с начала цикла.
static void test_load_free() {
  Sim s(64, false);
  s.in_l = sine(200.0, 0.5f);
  s.cmd(Cmd::Rec);
  s.run(70000);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BLOCK * 8);
  const std::string dir = out_dir("session_free");
  SessionDesc d;
  CHECK(save_session(s, dir, d) == SS_SAVED);
  CHECK(d.t[0].used && !d.t[0].synced && d.t[0].len >= 70000 && d.t[0].len < 70000 + BLOCK);
  Sim s2(64, false);
  s2.beat0 = 12345.0 * 120.0 / 60.0 / SR;
  s2.lp->session_load(d, dir.c_str());
  s2.block();
  CHECK(s2.v(0).stopped);
  s2.run_to_bar();
  s2.run(BLOCK);
  CHECK(!s2.v(0).stopped);
  CHECK(s2.v(0).pos < 2.0f * BLOCK / (float)d.t[0].len);  // с начала цикла
}

// Рабочий поток вручную: прочие задания (волны) выполнить, вернуть задание загрузки.
static bool next_load_job(Sim& s, WorkMsg& m) {
  for (int i = 0; i < 2000; ++i) {
    while (s.lp->take_work(m)) {
      if (m.op == 5) return true;
      WorkResp r;
      s.lp->do_work(m, r);
      s.lp->work_done(r);
    }
    s.block();
  }
  return false;
}

// Новый запрос, пока старый читается: грузится последний.
static void test_load_latest() {
  Sim a;
  record_two(a);
  const std::string da = out_dir("session_a");
  SessionDesc A;
  CHECK(save_session(a, da, A) == SS_SAVED);
  Sim b(64, false);
  b.in_l = sine(500.0, 0.2f);
  b.cmd(Cmd::Rec);
  b.run(30000);
  b.cmd(Cmd::Rec);
  b.run(BLOCK * 16);
  const std::string db = out_dir("session_b");
  SessionDesc B;
  CHECK(save_session(b, db, B) == SS_SAVED);

  Sim s;
  s.worker = false;  // рабочий поток ведём вручную
  s.lp->session_load(A, da.c_str());
  s.block();  // принят, лупы пустые — пул сразу уходит
  WorkMsg m;
  CHECK(s.lp->take_work(m) && m.op == 5);
  s.lp->session_load(B, db.c_str());  // пришёл новый, пока читаем A
  WorkResp r;
  s.lp->do_work(m, r);  // читает самый свежий запрос — B
  s.lp->work_done(r);
  CHECK(s.lp->session_id() == B.id);
  CHECK(s.lp->track_len(0) == B.t[0].len);
  // и отмена посреди чтения: запрос сменился после того, как рабочий поток взял задание
  s.lp->session_load(A, da.c_str());
  CHECK(next_load_job(s, m));  // B затихает, пул уходит
  s.lp->do_work(m, r);  // читает A
  s.lp->session_load(B, db.c_str());  // но ответ придёт уже после нового запроса
  s.lp->work_done(r);
  CHECK(s.lp->session_id() != A.id);  // A не поставлен
  CHECK(next_load_job(s, m));
  s.lp->do_work(m, r);
  s.lp->work_done(r);
  CHECK(s.lp->session_id() == B.id && s.lp->track_len(0) == B.t[0].len);
}

// Не помещается в пул, нет файла, другая частота — проверка отказывает, лупы целы.
static void test_check_errors() {
  Sim s;
  record_two(s);
  const std::string dir = out_dir("session_err");
  SessionDesc d;
  CHECK(save_session(s, dir, d) == SS_SAVED);
  Sim small(32);  // 3 такта стерео float — около 4,4 МБ: в 32 МБ помещается
  CHECK(session_check(d, dir.c_str(), SR, small.lp->pool_chunks()) == SS_IDLE);
  CHECK(session_check(d, dir.c_str(), SR, (uint32_t)(session_chunks(d) + 10)) == SS_E_MEMORY);
  CHECK(session_check(d, dir.c_str(), 44100.0, s.lp->pool_chunks()) == SS_E_RATE);
  unlink((dir + "/part1 track2.wav").c_str());
  CHECK(session_check(d, dir.c_str(), SR, s.lp->pool_chunks()) == SS_E_MISSING);
  // нечитаемая (обрезанная) сессия на загрузке: лупер пустой, ошибка в статусе
  Sim s2;
  s2.lp->session_load(d, dir.c_str());
  s2.run(BLOCK * 4);
  CHECK(s2.lp->session_status() == SS_E_MISSING);
  CHECK(s2.v(0).state == 0 && s2.lp->session_id() == 0);
}

// Reverse сохраняется; при другом темпе дорожка из Quantize следует ему (темп записи в описании).
static void test_reverse_tempo() {
  Sim s;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(220.0, 0.4f);
  s.cmd(Cmd::Rec);
  s.run_to_bar();
  s.run(BAR + BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.set.quantize = false;  // reverse сразу, без ожидания такта
  s.apply();
  s.cmd(Cmd::Reverse);
  s.run(BAR / 4);
  s.set.quantize = true;
  s.apply();
  CHECK(s.v(0).reversed);
  const std::string dir = out_dir("session_rev");
  SessionDesc d;
  CHECK(save_session(s, dir, d) == SS_SAVED);
  CHECK(d.t[0].reversed);
  Sim s2;
  s2.bpm = 140.0;
  s2.lp->session_load(d, dir.c_str());
  s2.block();
  s2.run_to_bar();
  s2.run(BAR);
  CHECK(s2.v(0).reversed && !s2.v(0).stopped);
  std::printf("  load at 140 BPM: effective speed %.4f (want %.4f)\n", (double)s2.v(0).espeed, 140.0 / 120.0);
  CHECK(std::fabs(s2.v(0).espeed - 140.0f / 120.0f) < 0.01f);
}

// Save, когда овердаб не закрывается: таймаут, звук не сохранён, лупер работает дальше.
static void test_save_timeout() {
  Sim s;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(220.0, 0.3f);
  s.cmd(Cmd::Rec);
  s.run_to_bar();
  s.run(BAR + BLOCK * 8);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 4);
  SessionDesc d;
  CHECK(save_session(s, out_dir("session_timeout"), d, BAR) == SS_E_TIMEOUT);
  CHECK(!s.lp->session_frozen() && s.v(0).state == 4);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  CHECK(s.v(0).state == 3);
}

int main() {
  test_wav_roundtrip();
  test_text_roundtrip();
  test_save_load();
  test_save_waits_and_blocks();
  test_skip_same();
  test_load_stopped();
  test_load_free();
  test_load_latest();
  test_check_errors();
  test_save_timeout();
  test_reverse_tempo();
  std::printf("session: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
