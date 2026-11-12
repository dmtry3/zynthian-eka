// Офлайн-тесты ядра лупера. Запуск: make test. Результат для прослушивания: tests/out/*.wav
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include "../core/looper.h"

using namespace alp;

// Выделения памяти в аудиопотоке: считаем вызовы operator new, пока идёт process().
static bool g_count_alloc = false;
static int g_allocs = 0;
void* operator new(size_t n) {
  if (g_count_alloc) ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

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

// Имитация хоста: блоки по 128, рабочий поток выполняется сразу после блока.
struct Sim {
  std::unique_ptr<Looper> lp{new Looper()};
  Settings set;
  TimeInfo ti;
  double bpm = 120.0;
  uint64_t t = 0;  // кадров от старта
  std::function<float(uint64_t)> in_l = [](uint64_t) { return 0.0f; };
  std::function<float(uint64_t)> in_r;
  std::vector<float> out_l, out_r;
  bool count_allocs = false;
  std::function<double(uint64_t)> beats_at;  // позиция транспорта в долях (по умолчанию от t = 0)
  double beats_now() const { return beats_at ? beats_at(t) : (double)t * bpm / 60.0 / SR; }

  std::unique_ptr<PeakShm> peaks{new PeakShm()};
  explicit Sim(size_t pool_mb = 64, bool quantize = false) {
    lp->init(SR, pool_mb << 20);
    lp->set_peaks(peaks.get());
    set.quantize = quantize;
    lp->set_settings(set);
    lp->set_monitor(false);  // в тестах слушаем только лупы
    ti.rolling = true;       // запись — только при запущенном транспорте
  }
  void apply() { lp->set_settings(set); }
  void run(uint64_t frames) {
    float il[BLOCK], ir[BLOCK], ol[BLOCK], orr[BLOCK];
    for (uint64_t done = 0; done < frames; done += BLOCK) {
      for (uint32_t i = 0; i < BLOCK; ++i) {
        il[i] = in_l(t + i);
        ir[i] = in_r ? in_r(t + i) : il[i] * 0.5f;
      }
      ti.beats_per_frame = bpm / 60.0 / SR;
      ti.beats = beats_now();
      g_count_alloc = count_allocs;
      lp->process(il, ir, ol, orr, BLOCK, ti);
      g_count_alloc = false;
      WorkMsg m;
      while (lp->take_work(m)) {
        WorkResp r;
        lp->do_work(m, r);
        lp->work_done(r);
      }
      out_l.insert(out_l.end(), ol, ol + BLOCK);
      out_r.insert(out_r.end(), orr, orr + BLOCK);
      t += BLOCK;
    }
  }
  void cmd(Cmd c) {
    ti.beats_per_frame = bpm / 60.0 / SR;
    ti.beats = beats_now();
    lp->set_time(ti);
    lp->command(c);
  }
  TrackView v(int i = 0) const { return lp->view(i); }
  // самый большой скачок между соседними сэмплами на отрезке
  float max_step(uint64_t from, uint64_t to) const {
    float m = 0;
    for (uint64_t i = from + 1; i < to && i < out_l.size(); ++i) {
      m = std::fmax(m, std::fabs(out_l[i] - out_l[i - 1]));
      m = std::fmax(m, std::fabs(out_r[i] - out_r[i - 1]));
    }
    return m;
  }
  std::vector<float> slice(uint64_t from, uint64_t n) const {
    return std::vector<float>(out_l.begin() + from, out_l.begin() + from + n);
  }
  void wav(const char* name) const {
    char path[256];
    std::snprintf(path, sizeof path, "tests/out/%s.wav", name);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    uint32_t n = (uint32_t)out_l.size(), data = n * 8, sr = (uint32_t)SR, br = sr * 8, sz = 36 + data;
    uint16_t fmt = 3, ch = 2, ba = 8, bits = 32;
    uint32_t sixteen = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&sz, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&sixteen, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&sr, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < n; ++i) { std::fwrite(&out_l[i], 4, 1, f); std::fwrite(&out_r[i], 4, 1, f); }
    std::fclose(f);
  }
};

static float rms_diff(const std::vector<float>& a, const std::vector<float>& b) {
  double s = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += (a[i] - b[i]) * (a[i] - b[i]);
  return (float)std::sqrt(s / (double)a.size());
}
static float rms(const std::vector<float>& a) { return rms_diff(a, std::vector<float>(a.size(), 0.0f)); }
static auto sine(double f, float a) {
  return [f, a](uint64_t t) { return a * (float)std::sin(PI2 * f * (double)t / SR); };
}

// Free, RPO: записали секунду, луп играет записанное, стык и старт без щелчков.
static void test_record_play() {
  Sim s;
  s.in_l = sine(230.3, 0.5f);  // не целое число периодов на цикл: стык должен сгладить кроссфейд
  s.cmd(Cmd::Rec);
  s.run(48000);
  CHECK(s.v().state == 2);
  s.cmd(Cmd::Rec);
  uint64_t t_close = s.t;
  s.run(BLOCK * 4);  // хвост 10 мс ещё звучит на входе
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(48000 * 3);
  s.wav("record_play");
  CHECK(s.v().state == 3);
  CHECK(std::fabs(s.v().len_s - 1.0f) < 1e-3f);
  CHECK(s.max_step(0, s.t) < 0.03f);
  // второй цикл: то, что записали (кроме стыка)
  uint64_t L = 48000;
  float err = 0;
  for (uint64_t p = 1000; p < L - 10; ++p) {
    float got = s.out_l[t_close + L + p];
    float want = 0.5f * (float)std::sin(PI2 * 230.3 * (double)p / SR);
    err = std::fmax(err, std::fabs(got - want));
  }
  CHECK(err < 1e-4f);
}

// 10 овердабов подряд, undo по одному слою возвращает звук, redo возвращает слой.
static void test_overdubs_undo() {
  Sim s;
  const uint64_t L = 375 * BLOCK;  // 48000
  s.in_l = sine(220.0, 0.4f);
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  const uint64_t t0 = s.t;  // позиция 0 цикла совпадает с t0 + k*L
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(L);  // хвост уже тишина: проверяем чистую основу
  std::vector<std::vector<float>> ref;
  auto cycle_start = [&]() { return t0 + ((s.t - t0 + L - 1) / L) * L; };
  auto capture = [&]() {
    uint64_t c = cycle_start();
    s.run(c - s.t + L);
    return s.slice(c, L);
  };
  ref.push_back(capture());
  for (int k = 1; k <= 10; ++k) {
    // овердаб ровно на цикл: нажали на начале цикла, отжали через цикл
    s.run(cycle_start() - s.t);
    double f = 300.0 + 37.0 * k;
    s.in_l = sine(f, 0.05f);
    s.cmd(Cmd::Rec);
    CHECK(s.v().state == 4);
    s.run(L);
    s.cmd(Cmd::Rec);
    s.run(BLOCK * 8);  // затухание конца овердаба
    s.in_l = [](uint64_t) { return 0.0f; };
    CHECK(s.v().state == 3 && s.v().layers == (uint32_t)(k + 1));
    ref.push_back(capture());
  }
  s.wav("overdubs");
  CHECK(s.max_step(t0, s.t) < 0.08f);
  // каждый слой что-то добавил
  for (int k = 1; k <= 10; ++k) CHECK(rms_diff(ref[k], ref[k - 1]) > 0.01f);
  uint64_t t_undo = s.t;
  for (int k = 10; k >= 1; --k) {
    s.cmd(Cmd::Undo);
    s.run(BLOCK * 8);
    std::vector<float> got = capture();
    CHECK(rms_diff(got, ref[k - 1]) < 1e-4f);
  }
  CHECK(s.max_step(t_undo, s.t) < 0.08f);
  s.cmd(Cmd::Redo);
  s.run(BLOCK * 8);
  CHECK(rms_diff(capture(), ref[1]) < 1e-4f);
  CHECK(s.v().can_undo && s.v().can_redo);
  // undo основы: дорожка пустеет; ещё undo на пустой — молча ничего
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 8);
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 8);
  CHECK(s.v().state == 0);
  s.cmd(Cmd::Undo);
  s.run(L / 2);
  CHECK(s.v().state == 0);
  CHECK(rms(s.slice(s.t - L / 4, L / 4)) < 1e-6f);
}

// Mute: плавно по fade out, без щелчков, unmute возвращает.
static void test_mute() {
  Sim s;
  s.in_l = sine(220.0, 0.5f);
  s.cmd(Cmd::Rec);
  s.run(48000);
  s.cmd(Cmd::Rec);
  s.run(48000);
  uint64_t tm = s.t;
  s.cmd(Cmd::Mute);  // Free: сразу
  s.run(48000 / 2);
  CHECK(s.v().muted);
  CHECK(rms(s.slice(s.t - 2000, 2000)) < 1e-6f);  // через 200 мс тишина
  CHECK(rms(s.slice(tm, 2000)) > 0.1f);            // но не сразу
  s.cmd(Cmd::Mute);
  s.run(48000);
  CHECK(rms(s.slice(s.t - 4800, 4800)) > 0.2f);
  CHECK(s.max_step(tm - 100, s.t) < 0.03f);
  s.wav("mute");
}

// Reverse как лента: разворот без щелчков, потом звук идёт задом наперёд.
static void test_reverse() {
  Sim s;
  const uint64_t L = 48000;
  s.in_l = sine(180.0, 0.5f);
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 4);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(L * 2);
  uint64_t c = s.t - (s.t % 1);  // эталон: цикл вперёд, начиная с позиции 0
  (void)c;
  std::vector<float> fwd(L);
  {
    // позиция p звучит в момент t_close + k*L + p; t_close = L (запись началась в 0)
    uint64_t base = L + L;  // второй цикл
    for (uint64_t p = 0; p < L; ++p) fwd[p] = s.out_l[base + p];
  }
  uint64_t tr = s.t;
  s.cmd(Cmd::Reverse);
  s.run(BLOCK * 40);  // 50 мс разворот
  CHECK(s.v().reversed);
  CHECK(s.max_step(tr - 10, s.t) < 0.03f);
  // после разворота: каждый следующий сэмпл — предыдущая позиция
  uint64_t p = (uint64_t)std::llround(s.v().pos * (double)L) % L;
  uint64_t t1 = s.t;
  s.run(4096);
  float err = 0;
  for (uint64_t k = 0; k < 2000; ++k) {
    uint64_t pk = (p + L - k) % L;
    if (pk < 600) break;  // область стыка не сравниваем
    err = std::fmax(err, std::fabs(s.out_l[t1 + k] - fwd[pk]));
  }
  CHECK(err < 1e-4f);
  s.cmd(Cmd::Reverse);
  s.run(BLOCK * 40);
  CHECK(!s.v().reversed && s.max_step(tr, s.t) < 0.03f);
  s.wav("reverse");
}

// Quantize: запись ждёт такта, фиксированная длина закрывается сама; Free-длина округляется до такта.
// Счёт перед записью (этап 8): Quantize — с ближайшей черты, Free — от нажатия; отмена как у взведённой.
static void test_count_in() {
  const uint64_t BAR = 96000;  // 120 уд/мин, 4/4
  auto at = [](Sim& s, double bars) { s.run((uint64_t)std::llround(bars * BAR) - s.t); };
  {  // Quantize, 1 такт: Rec на 0,5 -> черта 1 -> счёт такт -> запись с черты 2
    Sim s(64, true);
    s.set.length[0] = 1;
    s.set.count_in = 1;
    s.apply();
    s.in_l = sine(200.0, 0.3f);
    at(s, 0.5);
    s.cmd(Cmd::Rec);
    at(s, 0.9);
    CHECK(s.v().state == 1 && s.v().count == 0);  // ждёт черты: счёта ещё нет
    at(s, 1.1);
    CHECK(s.v().state == 1 && s.v().count == 4);  // счёт: 4 доли до начала
    at(s, 1.8);
    CHECK(s.v().state == 1 && s.v().count == 1);
    at(s, 2.0 + 0.01);
    CHECK(s.v().state == 2 && s.v().count == 0);  // запись с черты 2
    at(s, 3.0 + 0.01);
    CHECK(s.v().state == 3 && std::fabs(s.v().len_s - 2.0f) < 1e-3f);
  }
  {  // Quantize, 2 такта; повторный Rec во время счёта — отмена
    Sim s(64, true);
    s.set.count_in = 2;
    s.apply();
    at(s, 0.5);
    s.cmd(Cmd::Rec);
    at(s, 1.5);
    CHECK(s.v().state == 1 && s.v().count == 6);
    s.cmd(Cmd::Rec);
    CHECK(s.v().state == 0);
    at(s, 4.3);
    CHECK(s.v().state == 0);
    // снова, посреди такта 4: черта 5, счёт 5-7, запись с 7
    s.cmd(Cmd::Rec);
    at(s, 6.9);
    CHECK(s.v().state == 1);
    at(s, 7.0 + 0.01);
    CHECK(s.v().state == 2);
  }
  {  // Free, 1 такт: счёт от нажатия, запись ровно через такт; овердаб без счёта
    Sim s(64, false);
    s.set.count_in = 1;
    s.apply();
    s.in_l = sine(200.0, 0.3f);
    at(s, 0.3);
    s.cmd(Cmd::Rec);
    at(s, 0.35);
    CHECK(s.v().state == 1 && s.v().count == 4);
    at(s, 1.29);
    CHECK(s.v().state == 1 && s.v().count == 1);
    at(s, 1.31);
    CHECK(s.v().state == 2);
    at(s, 2.3);
    s.cmd(Cmd::Rec);  // закрыть: такт доигрывается
    at(s, 3.5);
    CHECK(s.v().state == 3);
    s.cmd(Cmd::Rec);
    CHECK(s.v().state == 4);  // овердаб сразу
  }
  {  // счёт идёт, транспорт остановился — отмена; переход на другую дорожку — отмена
    Sim s(64, true);
    s.set.count_in = 1;
    s.apply();
    at(s, 0.5);
    s.cmd(Cmd::Rec);
    at(s, 1.2);
    s.ti.rolling = false;
    s.cmd(Cmd::Mute);  // передать транспорт (cmd вызывает set_time)
    CHECK(s.v().state == 0);
    s.ti.rolling = true;
    s.cmd(Cmd::Mute);
    s.cmd(Cmd::Rec);
    s.cmd(Cmd::TrackNext);
    CHECK(s.v(0).state == 0);
  }
}

// Старт записи по порогу (этап 8): только Free, основа; в начале записи 10 мс входа до атаки.
static void test_threshold() {
  const uint64_t BAR = 96000;
  auto at = [](Sim& s, double bars) { s.run((uint64_t)std::llround(bars * BAR) - s.t); };
  const uint64_t T = (uint64_t)(1.0 * BAR) + 77;  // атака не на границе блока
  {
    Sim s(64, false);
    s.set.rec_thresh_db = -30;
    s.apply();
    s.in_l = [T](uint64_t f) { return f < T ? 0.005f : 0.5f; };  // тихо (−46 дБ), потом громко
    s.in_r = s.in_l;
    at(s, 0.3);
    s.cmd(Cmd::Rec);
    at(s, 0.9);
    CHECK(s.v().state == 1 && s.v().count == 0);  // ждёт сигнала
    at(s, 1.2);
    CHECK(s.v().state == 2);
    at(s, 2.0);
    s.in_l = [](uint64_t) { return 0.0f; };  // хвост после закрытия (кроссфейд стыка) — тишина
    s.in_r = s.in_l;
    s.cmd(Cmd::Rec);
    s.run(BLOCK * 8);
    CHECK(s.v().state == 3);
    // начало цикла: 10 мс тихого входа до атаки (с нарастанием и стыком), с кадра 480 — атака
    const Pool& p = s.lp->pool();
    const Buf& b = p.buf(s.lp->track_sum(0));
    float l, r, l0, l479, l480;
    p.read(b, 0, l0, r);
    p.read(b, 479, l479, r);
    p.read(b, 480, l480, r);
    p.read(b, 240, l, r);
    CHECK(l0 == 0.0f && l480 == 0.5f);
    CHECK(l > 0.0f && l < 0.005f && l479 > 0.004f && l479 < 0.005f);  // тихий вход, не атака
    // длина: от атаки (минус 10 мс) до нажатия, такт не доигрывается (Free)
    const double len = s.v().len_s * SR;
    CHECK(std::fabs(len - (double)(2 * BAR - T + 480)) < BLOCK + 1);
    // овердаб — сразу, без порога: вход тихий, а овердаб идёт
    s.cmd(Cmd::Rec);
    CHECK(s.v().state == 4);
  }
  {  // счёт, потом порог: громкий вход во время счёта запись не запускает
    Sim s(64, false);
    s.set.rec_thresh_db = -30;
    s.set.count_in = 1;
    s.apply();
    s.in_l = [](uint64_t) { return 0.0f; };
    at(s, 0.3);
    s.cmd(Cmd::Rec);
    s.in_l = sine(220.0, 0.3f);
    at(s, 1.25);
    CHECK(s.v().state == 1 && s.v().count == 1);
    s.in_l = [](uint64_t) { return 0.0f; };
    at(s, 1.6);
    CHECK(s.v().state == 1 && s.v().count == 0);  // счёт кончился, ждёт сигнала
    s.in_l = sine(220.0, 0.3f);
    at(s, 1.7);
    CHECK(s.v().state == 2);
    s.cmd(Cmd::Rec);  // закрыть (короче 20 мс не бывает: здесь 0,1 такта)
  }
  {  // Quantize: порог не действует, запись с черты и в тишине
    Sim s(64, true);
    s.set.rec_thresh_db = -30;
    s.apply();
    at(s, 0.5);
    s.cmd(Cmd::Rec);
    at(s, 1.0 + 0.01);
    CHECK(s.v().state == 2);
  }
  {  // повторный Rec, пока ждёт сигнала, — отмена
    Sim s(64, false);
    s.set.rec_thresh_db = -30;
    s.apply();
    at(s, 0.3);
    s.cmd(Cmd::Rec);
    at(s, 0.5);
    s.cmd(Cmd::Rec);
    CHECK(s.v().state == 0);
    s.in_l = sine(220.0, 0.3f);
    at(s, 0.8);
    CHECK(s.v().state == 0);
  }
}

static void test_quantize() {
  const uint64_t BAR = 96000;  // 120 уд/мин, 4/4
  {
    Sim s(64, true);
    s.ti.rolling = true;
    s.set.length[0] = 1;
    s.apply();
    s.in_l = sine(200.0, 0.3f);
    s.run(BAR / 2);
    s.cmd(Cmd::Rec);
    s.run(BLOCK);
    CHECK(s.v().state == 1);  // ждёт такта
    s.run(BAR / 2);
    CHECK(s.v().state == 2);
    s.run(BAR);
    CHECK(s.v().state == 3);
    CHECK(std::fabs(s.v().len_s - 2.0f) < 1e-3f);
  }
  {  // Free-длина: отжали на 1,3 такта -> такт доигрывается, цикл 2 такта
    Sim s(64, true);
    s.ti.rolling = true;
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(BAR);                      // ждём черту
    CHECK(s.v().state == 2);
    s.run((uint64_t)(BAR * 1.3));
    s.cmd(Cmd::Rec);
    s.run(BLOCK);
    CHECK(s.v().state == 2);         // ещё пишет
    s.run((uint64_t)(BAR * 0.7));
    CHECK(s.v().state == 3 && std::fabs(s.v().len_s - 4.0f) < 1e-3f);
  }
  {  // отжали на 100 мс позже черты 1-го такта -> цикл 1 такт, сразу играет
    Sim s(64, true);
    s.ti.rolling = true;
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(BAR);
    s.run(BAR + 4800);
    s.cmd(Cmd::Rec);
    s.run(BLOCK);
    CHECK(s.v().state == 3 && std::fabs(s.v().len_s - 2.0f) < 1e-3f);
    s.run(BAR);
    CHECK(s.max_step(0, s.t) < 0.03f);
  }
  {  // то же в ROP: после нажатия посреди такта пишет до черты, потом овердаб
    Sim s(64, true);
    s.ti.rolling = true;
    s.set.rop = true;
    s.apply();
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(BAR);
    s.run((uint64_t)(BAR * 1.3));
    s.cmd(Cmd::Rec);
    s.run(BLOCK);
    CHECK(s.v().state == 2);
    s.run((uint64_t)(BAR * 0.7));
    CHECK(s.v().state == 4 && std::fabs(s.v().len_s - 4.0f) < 1e-3f);
  }
  // транспорт стоит: Rec молча игнорируется и в Quantize, и в Free (решение 2026-11-08)
  for (bool q : {true, false}) {
    Sim s(64, q);
    s.ti.rolling = false;
    s.run(BAR / 3);
    s.cmd(Cmd::Rec);
    s.run(BAR);
    CHECK(s.v().state == 0 && s.lp->grid_beats() == 0.0);
  }
  {  // взведённая запись отменяется остановкой транспорта
    Sim s(64, true);
    s.run(BAR / 3);
    s.cmd(Cmd::Rec);
    CHECK(s.v().state == 1);
    s.ti.rolling = false;
    s.run(BAR);
    CHECK(s.v().state == 0 && s.lp->grid_beats() == 0.0);
  }
  {  // остановили во время записи: запись дописывается, потом луп затихает, счёт встаёт в 0
    Sim s(64, true);
    s.set.length[0] = 2;
    s.apply();
    s.in_l = sine(220.0, 0.4f);
    s.run(BLOCK);
    s.cmd(Cmd::Rec);
    s.run(BAR + BAR / 2);  // запись с 1-го такта, идёт половина
    CHECK(s.v().state == 2);
    const double frozen = s.beats_now();
    s.ti.rolling = false;
    s.beats_at = [frozen](uint64_t) { return frozen; };
    s.run(BAR);
    CHECK(s.v().state == 2);  // дописывается по своему счёту
    s.run(BAR);
    CHECK(s.v().state == 3 && s.v().stopped && std::fabs(s.v().len_s - 4.0f) < 1e-3f);
    s.in_l = [](uint64_t) { return 0.0f; };
    s.run(BAR / 2);
    CHECK(rms(s.slice(s.t - 9600, 9600)) < 1e-6f);
    CHECK(s.v().pos == 0.0f && s.lp->grid_beats() == 0.0);
  }
}

// ROP: после закрытия цикла сразу овердаб, хвост ложится на начало; undo снимает этот слой.
static void test_rop() {
  Sim s;
  s.set.rop = true;
  s.apply();
  const uint64_t L = 48000;
  s.in_l = sine(220.0, 0.4f);
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  CHECK(s.v().state == 4);
  s.in_l = sine(523.0, 0.2f);
  s.run(L / 2);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  CHECK(s.v().state == 3 && s.v().layers == 2);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 8);
  CHECK(s.v().layers == 1);
  // после undo играет только основа (сравниваем середину цикла)
  uint64_t c = L + ((s.t - L) / L + 1) * L;
  s.run(c - s.t + L);
  float err = 0;
  for (uint64_t p = 2000; p < L - 2000; ++p)
    err = std::fmax(err, std::fabs(s.out_l[c + p] - 0.4f * (float)std::sin(PI2 * 220.0 * (double)p / SR)));
  CHECK(err < 1e-4f);
  CHECK(s.max_step(0, s.t) < 0.05f);
}

// ROP: «Следующая дорожка» во время записи закрывает цикл без овердаба (решение 2026-11-07).
static void test_rop_leave() {
  {  // Free: цикл закрывается сразу и играет
    Sim s;
    s.set.rop = true;
    s.apply();
    s.in_l = sine(220.0, 0.4f);
    s.cmd(Cmd::Rec);
    s.run(48000);
    s.cmd(Cmd::TrackNext);
    s.run(BLOCK * 8);
    CHECK(s.lp->active() == 1 && s.v(0).state == 3 && s.v(0).layers == 1);
    s.run(48000);
    CHECK(s.v(0).state == 3);
    // дальше на этой дорожке Rec — обычный овердаб
    s.cmd(Cmd::TrackPrev);
    s.cmd(Cmd::Rec);
    CHECK(s.v(0).state == 4);
    // а запись следующей дорожки в ROP закрывается обычным нажатием с овердабом
    s.cmd(Cmd::TrackNext);
    s.run(BLOCK * 8);
    s.cmd(Cmd::Rec);
    s.run(48000);
    s.cmd(Cmd::Rec);
    CHECK(s.v(1).state == 4);
  }
  {  // Quantize, длина Free: нажали Rec (цикл ждёт конца такта), потом ушли — без овердаба
    Sim s(64, true);
    s.set.rop = true;
    s.apply();
    s.in_l = sine(220.0, 0.4f);
    s.cmd(Cmd::Rec);
    s.run(48000 * 3);  // 1,5 такта при 120
    s.cmd(Cmd::Rec);
    s.cmd(Cmd::TrackNext);
    s.run(48000 * 2);  // черта второго такта прошла
    CHECK(s.v(0).state == 3 && s.v(0).layers == 1 && s.lp->active() == 1);
    // и без нажатия Rec: уход во время записи закрывает на конце такта
    s.cmd(Cmd::Rec);
    s.run(48000 * 3);
    s.cmd(Cmd::TrackNext);
    s.run(48000 * 2);
    CHECK(s.v(1).state == 3 && s.v(1).layers == 1);
  }
}

// Reset: тап очищает и откатывается undo; удержание очищает всё и возвращает память.
static void test_reset() {
  Sim s;
  s.in_l = sine(220.0, 0.4f);
  uint32_t total = s.lp->free_chunks();
  s.cmd(Cmd::Rec);
  s.run(48000);
  s.cmd(Cmd::Rec);
  s.run(48000);
  s.cmd(Cmd::Rec);  // овердаб
  s.run(24000);
  s.cmd(Cmd::ResetTrack);
  s.run(BLOCK * 8);  // овердаб сначала плавно закрывается (вход ещё звучит), потом reset
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(4800);
  CHECK(s.v().state == 0);
  CHECK(rms(s.slice(s.t - 2400, 2400)) < 1e-6f);
  s.cmd(Cmd::Undo);
  s.run(48000);
  CHECK(s.v().state == 3);
  CHECK(rms(s.slice(s.t - 24000, 24000)) > 0.1f);
  // вторая дорожка, потом полный сброс
  s.cmd(Cmd::TrackNext);
  CHECK(s.lp->active() == 1);
  s.in_l = sine(330.0, 0.3f);
  s.cmd(Cmd::Rec);
  s.run(24000);
  s.cmd(Cmd::Rec);
  s.run(24000);
  s.cmd(Cmd::ResetAll);
  s.run(4800);
  CHECK(s.v(0).state == 0 && s.v(1).state == 0 && !s.v(0).can_undo);
  CHECK(s.lp->free_chunks() == total);
  CHECK(s.max_step(0, s.t) < 0.05f);
}

// Пул кончился: старые слои вливаются в основу, всё продолжает работать.
static void test_pool_full() {
  Sim s(32);
  const uint64_t L = 48000 * 3;  // ~9 кусков на слой, в 32 МБ влезает около 25 слоёв
  s.in_l = sine(200.0, 0.2f);
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  for (int k = 0; k < 40; ++k) {
    s.in_l = sine(250.0 + 7 * k, 0.01f);
    s.cmd(Cmd::Rec);
    s.run(L);
    s.cmd(Cmd::Rec);
    s.run(BLOCK * 8);
  }
  CHECK(s.v().state == 3);
  CHECK(s.v().layers > 5 && s.v().layers < 40);
  uint32_t before = s.v().layers;
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 16);
  CHECK(s.v().layers < before);  // undo нужна память под новую сумму: старые слои влились в основу
  CHECK(s.max_step(0, s.t) < 0.05f);
}

// Quantize: reverse доигрывает такт, лента останавливается ровно на тактовой черте,
// после разворота позиция как при мгновенном развороте на черте (лупы остаются в долю).
static void test_reverse_quantized() {
  const uint64_t BAR = 96000;  // 120 уд/мин, 4/4
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  s.run(BAR * 2 + BLOCK * 8);  // ждём такт, пишем 1 такт (+ хвост стыка): цикл начинается на черте
  CHECK(s.v().state == 3);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BAR / 3);  // середина такта
  const uint64_t L = BAR;
  const uint64_t next_bar = ((s.t / BAR) + 1) * BAR;
  s.cmd(Cmd::Reverse);
  const uint64_t n = 2400;  // 50 мс
  s.run(next_bar - n / 2 - s.t - BLOCK);  // до начала разворота — ещё вперёд
  CHECK(!s.v().reversed);
  s.run(BLOCK * 2);
  CHECK(s.v().reversed);  // разворот начался за n/2 до черты
  s.run(next_bar + n - s.t);  // разворот закончился
  // идеальная позиция: на черте 0 (цикл = такт), дальше назад
  const uint64_t want = (L - (s.t - next_bar) % L) % L;
  const uint64_t got = (uint64_t)std::llround(s.v().pos * (double)L) % L;
  int64_t diff = (int64_t)got - (int64_t)want;
  if (diff > (int64_t)L / 2) diff -= L;
  if (diff < -(int64_t)L / 2) diff += L;
  CHECK(diff >= -2 && diff <= 2);
  CHECK(s.max_step(BAR * 2, s.t) < 0.03f);
  // повторное нажатие до черты отменяет разворот
  s.cmd(Cmd::Reverse);
  s.cmd(Cmd::Reverse);
  s.run(BAR * 2);
  CHECK(s.v().reversed);
}

// Записали луп, остановили транспорт (луп играет), Zynthian пошёл заново с первого такта:
// луп встаёт в фазу с позицией Zynthian через кроссфейд, без щелчка.
static void test_transport_restart_resync() {
  const uint64_t BAR = 96000;
  const double bpf = 120.0 / 60.0 / SR;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  s.run(BAR * 2 + BLOCK * 8);  // запись с 1-го такта, 1 такт (+ хвост стыка)
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run((uint64_t)(BAR * 1.3));
  CHECK(s.v().state == 3);
  // стоп: позиция Zynthian замерла, луп играет дальше
  const double frozen = s.beats_now();
  s.ti.rolling = false;
  s.beats_at = [frozen](uint64_t) { return frozen; };
  s.run((uint64_t)(BAR * 0.45));
  // пуск с начала (доля 0)
  const uint64_t t_start = s.t;
  s.ti.rolling = true;
  s.beats_at = [t_start, bpf](uint64_t t) { return (double)(t - t_start) * bpf; };
  s.run(BLOCK * 20);
  // фаза: начало цикла на тактовой черте Zynthian
  const uint64_t want = (s.t - t_start) % BAR;
  const uint64_t got = (uint64_t)std::llround(s.v().pos * (double)BAR) % BAR;
  int64_t diff = (int64_t)got - (int64_t)want;
  CHECK(diff >= -2 && diff <= 2);
  CHECK(s.max_step(BAR * 2, s.t) < 0.03f);
  s.wav("transport_restart");
}

// Транспорт остановили: лупы плавно затихают. Пустили: входят обратно в фазе, без щелчка.
static void test_transport_stop_start() {
  const uint64_t BAR = 96000;
  const double bpf = 120.0 / 60.0 / SR;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  s.run(BAR * 2 + BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run((uint64_t)(BAR * 1.3));
  const double frozen = s.beats_now();
  s.ti.rolling = false;
  s.beats_at = [frozen](uint64_t) { return frozen; };
  const uint64_t t_stop = s.t;
  s.run(BAR / 2);
  CHECK(s.v().stopped && s.v().state == 3);
  CHECK(rms(s.slice(t_stop, 2400)) > 0.05f);           // затухает не сразу (200 мс)
  CHECK(rms(s.slice(s.t - 9600, 9600)) < 1e-6f);       // потом тишина
  // затихла: курсор и счёт тактов в 0 и стоят
  CHECK(s.v().pos == 0.0f && s.lp->grid_beats() == 0.0);
  s.run(BAR / 2);
  CHECK(s.v().pos == 0.0f && s.lp->grid_beats() == 0.0);
  s.cmd(Cmd::Rec);                                      // Rec на остановленной: молча ничего
  CHECK(s.v().state == 3);
  const uint64_t t_start = s.t;
  s.ti.rolling = true;
  s.beats_at = [t_start, bpf](uint64_t t) { return (double)(t - t_start) * bpf; };
  s.run(BAR / 2);
  CHECK(!s.v().stopped);
  CHECK(rms(s.slice(s.t - 9600, 9600)) > 0.1f);        // снова играет
  const uint64_t want = (s.t - t_start) % BAR;
  const uint64_t got = (uint64_t)std::llround(s.v().pos * (double)BAR) % BAR;
  int64_t diff = (int64_t)got - (int64_t)want;
  CHECK(diff >= -2 && diff <= 2);
  CHECK(s.max_step(BAR * 2, s.t) < 0.03f);
  s.wav("transport_stop_start");
}

// Play/Stop: стоп на границе такта (транспорт идёт дальше), старт с начала такта в фазе.
static void test_play_stop() {
  const uint64_t BAR = 96000;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  s.run(BAR * 2 + BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BAR / 4);                         // четверть такта
  s.cmd(Cmd::PlayStop);
  CHECK(s.lp->playstop_pending() == 1);
  s.run(BAR / 2);                         // ещё до черты: играет
  CHECK(!s.v().stopped && rms(s.slice(s.t - 4800, 4800)) > 0.1f);
  s.run(BAR / 2);                         // черта прошла, 200 мс затухания прошли
  CHECK(s.v().stopped && s.lp->user_stopped());
  CHECK(rms(s.slice(s.t - 4800, 4800)) < 1e-6f);
  s.cmd(Cmd::PlayStop);
  s.cmd(Cmd::PlayStop);                   // отмена до черты
  s.run(BAR * 2);
  CHECK(s.v().stopped);
  s.cmd(Cmd::PlayStop);
  s.run(BAR + BLOCK * 4);                 // старт на черте
  CHECK(!s.v().stopped && rms(s.slice(s.t - 4800, 4800)) > 0.1f);
  const uint64_t want = s.t % BAR;        // цикл = такт, сетка от t = 0
  const uint64_t got = (uint64_t)std::llround(s.v().pos * (double)BAR) % BAR;
  int64_t diff = (int64_t)got - (int64_t)want;
  CHECK(diff >= -2 && diff <= 2);
  CHECK(s.max_step(BAR * 2, s.t) < 0.03f);
  // остановили педалью — пуск транспорта не включает
  s.cmd(Cmd::PlayStop);
  s.run(BAR * 2);
  CHECK(s.v().stopped);
  const double frozen = s.beats_now();
  s.ti.rolling = false;
  s.beats_at = [frozen](uint64_t) { return frozen; };
  s.run(BAR / 2);
  const uint64_t t0 = s.t;
  s.ti.rolling = true;
  s.beats_at = [t0](uint64_t t) { return (double)(t - t0) * 120.0 / 60.0 / SR; };
  s.run(BAR);
  CHECK(s.v().stopped);
  s.wav("play_stop");
}

// Пики для волн: после записи — волна суммы, после овердабов — слои (новые первыми),
// во время овердаба волна слоя растёт вживую; undo убирает слой.
// Волна записи основы растёт вживую: колонки по 20 мс, кончились — сливаются попарно;
// при фиксированной длине вся полоса — эта длина; после закрытия цикла волна записи гаснет.
static void test_rec_peaks() {
  Sim s;
  const PeakTrack& pk = s.peaks->t[0];
  s.in_l = sine(220.0, 0.5f);
  s.cmd(Cmd::Rec);
  uint32_t live0 = pk.live;
  s.run(24000);  // 0,5 с
  CHECK(s.v().state == 2 && pk.live > live0);
  CHECK(pk.rec_cols >= 24 && pk.rec_cols <= 26 && std::fabs(pk.rec_col_s - 0.02f) < 1e-4f);
  CHECK(pk.rec.mx[10] >= 60 && pk.rec.mx[10] <= 66 && pk.rec.mn[10] <= -60 && pk.rec.mx[100] == 0);
  s.run(48000 * 12);  // 12,5 с: 512 колонок по 20 мс кончились, колонки по 40 мс
  CHECK(std::fabs(pk.rec_col_s - 0.04f) < 1e-4f && pk.rec_cols > 300 && pk.rec_cols < 320);
  CHECK(pk.rec.mx[5] >= 60 && pk.rec.mx[200] >= 60 && pk.rec.mx[300] >= 60 && pk.rec.mx[400] == 0);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  CHECK(s.v().state == 3 && pk.rec_cols == 0);
  // фиксированная длина 2 такта при 120 BPM = 4 с: 512 колонок на 4 с
  Sim q(64, true);
  q.set.length[0] = 2;
  q.apply();
  q.in_l = sine(220.0, 0.5f);
  q.cmd(Cmd::Rec);
  q.run(48000 * 5);
  const PeakTrack& qk = q.peaks->t[0];
  CHECK(q.v().state == 3 && qk.rec_cols == 0);
  CHECK(std::fabs(qk.rec_col_s * 512.0f - 4.0f) < 0.01f);
}

static void test_peaks() {
  Sim s;
  const uint64_t L = 48000;
  const PeakTrack& pk = s.peaks->t[0];
  s.in_l = [](uint64_t t) { return t < 24000 ? 0.5f * (float)std::sin(PI2 * 220.0 * (double)t / SR) : 0.0f; };
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  CHECK(pk.seq % 2 == 0 && pk.seq > 0 && pk.nlayers == 0);
  // первая половина цикла — синус 0,5 (64 из 127), вторая — тишина
  int mx = 0, mn = 0;
  for (int c = 50; c < 200; ++c) {
    mx = std::max(mx, (int)pk.sum.mx[c]);
    mn = std::min(mn, (int)pk.sum.mn[c]);
  }
  CHECK(mx >= 60 && mx <= 66 && mn <= -60);
  CHECK(pk.sum.mx[400] == 0 && pk.sum.mn[400] == 0);
  // овердаб во второй половине: живая волна слоя растёт
  s.in_l = sine(330.0, 0.25f);
  s.run(L - (s.t % L) + L / 2);  // до середины цикла
  s.cmd(Cmd::Rec);
  uint32_t live0 = pk.live;
  s.run(L / 4);
  CHECK(pk.dub_active == 1 && pk.live > live0);
  CHECK(pk.dub.mx[320] >= 28 && pk.dub.mx[100] == 0);
  s.run(L / 4);
  s.cmd(Cmd::Rec);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BLOCK * 8);
  CHECK(pk.dub_active == 0 && pk.nlayers == 1 && pk.layer_no[0] == 1);
  CHECK(pk.layer[0].mx[400] >= 28 && pk.layer[0].mx[100] == 0);
  CHECK(pk.sum.mx[400] >= 28);
  s.cmd(Cmd::Undo);
  s.run(BLOCK * 8);
  CHECK(pk.nlayers == 0 && pk.sum.mx[400] == 0);
  s.cmd(Cmd::ResetTrack);
  s.run(BLOCK * 8);
  CHECK(pk.sum.mx[100] == 0);
}

// Парты и Lock (этап 2).
static void test_parts_lock() {
  const uint64_t BAR = 96000;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = s.set.length[1] = 1;
  s.apply();
  auto at = [&](double bars) { s.run((uint64_t)std::llround(bars * BAR) - s.t); };
  // дорожка 1: синус 180 Гц, 1 такт с 1-го такта; Lock
  s.in_l = sine(180.0, 0.4f);
  s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
  s.cmd(Cmd::Rec);
  at(2.0 + 0.02);
  s.in_l = [](uint64_t) { return 0.0f; };
  CHECK(s.v(0).locked && !s.v(1).locked);  // Lock по умолчанию на первой дорожке
  // дорожка 2: синус 300 Гц, со 2-го такта
  s.cmd(Cmd::TrackNext);
  s.in_l = sine(300.0, 0.3f);
  s.cmd(Cmd::Rec);
  at(3.0 + 0.02);
  s.in_l = [](uint64_t) { return 0.0f; };
  at(4.0);
  CHECK(s.v(0).state == 3 && s.v(1).state == 3 && s.lp->parts_mask() == 1u);
  at(5.0);
  const uint32_t free_before = s.lp->free_chunks();
  // переход в пустой парт 2 посреди такта: ждёт черты
  at(5.25);
  s.cmd(Cmd::PartNext);
  CHECK(s.lp->pending_part() == 1 && s.lp->part() == 0);
  at(5.9);
  CHECK(s.lp->part() == 0);
  at(6.0 + 0.01);
  CHECK(s.lp->part() == 1 && s.lp->pending_part() == -1);
  const std::vector<float> ref0 = s.slice(5 * BAR, BAR);  // звук парта 1 за последний такт перед переходом
  CHECK(s.v(0).state == 3 && s.v(0).locked);  // Lock-дорожка перешла
  CHECK(s.v(1).state == 0);                   // остальные — нет
  CHECK(s.lp->active() == 1);                 // активная — первая свободная
  CHECK(s.lp->free_chunks() == free_before);  // копия не тратит память
  at(6.5);
  // звучит только дорожка 1, в той же фазе (позиция цикла = t % BAR), без скачка
  float err = 0;
  for (uint64_t t = (uint64_t)(6.3 * BAR); t < (uint64_t)(6.5 * BAR); ++t) {
    const double p = (double)(BAR + t % BAR);
    err = std::fmax(err, std::fabs(s.out_l[t] - 0.4f * (float)std::sin(PI2 * 180.0 * p / SR)));
  }
  CHECK(err < 1e-4f);
  CHECK(s.max_step(5 * BAR, s.t) < 0.03f);
  // овердаб на копии: память тратится только на изменённые куски, старый парт не меняется
  s.cmd(Cmd::TrackPrev);
  CHECK(s.lp->active() == 0);
  at(7.0);
  s.in_l = sine(523.0, 0.2f);
  s.cmd(Cmd::Rec);
  at(8.0);
  s.cmd(Cmd::Rec);
  at(8.0 + 0.02);
  s.in_l = [](uint64_t) { return 0.0f; };
  CHECK(s.v(0).state == 3 && s.v(0).layers == 1);
  CHECK(s.lp->free_chunks() < free_before);
  CHECK(s.lp->parts_mask() == 3u);
  // назад в записанный парт 1: на черте, с начала цикла, без овердаба копии
  at(8.5);
  s.cmd(Cmd::PartPrev);
  at(9.0 + 0.01);
  CHECK(s.lp->part() == 0 && s.v(0).state == 3 && s.v(1).state == 3);
  at(11.0);
  CHECK(rms_diff(s.slice(10 * BAR, BAR), ref0) < 1e-4f);
  CHECK(s.max_step(8 * BAR, s.t) < 0.05f);
  // повторный тап переносит цель, вернулись на текущий — отмена
  s.cmd(Cmd::PartNext);
  s.cmd(Cmd::PartNext);
  CHECK(s.lp->pending_part() == 2);
  s.cmd(Cmd::PartPrev);
  CHECK(s.lp->pending_part() == 1);
  s.cmd(Cmd::PartPrev);
  CHECK(s.lp->pending_part() == -1);
  // касание на экране (этап 7б): точка парта — переход по PART AT, строка — активная дорожка
  s.lp->select_part(2);
  CHECK(s.lp->pending_part() == 2);
  s.lp->select_part(1);  // другая точка — новая цель
  CHECK(s.lp->pending_part() == 1);
  s.lp->select_part(1);  // та же — отмена
  CHECK(s.lp->pending_part() == -1);
  s.lp->select_part(1);
  s.lp->select_part(0);  // текущий парт — отмена
  CHECK(s.lp->pending_part() == -1 && s.lp->part() == 0);
  s.lp->select_part(0);  // текущий без ожидания — ничего
  s.lp->select_part(9);  // нет такого — молча
  CHECK(s.lp->pending_part() == -1 && s.lp->part() == 0);
  s.lp->select_slot(3);
  CHECK(s.lp->active() == 3);
  s.lp->select_slot(9);
  CHECK(s.lp->active() == 3);
  s.lp->select_part(1);
  at(11.5);
  CHECK(s.lp->part() == 0 && s.lp->pending_part() == 1);
  at(12.0 + 0.01);
  CHECK(s.lp->part() == 1 && s.lp->pending_part() == -1);
  s.wav("parts_lock");
}

// Компенсация задержки 10 мс: вход опаздывает на 480 кадров. Сыгранное на 2-ю долю ложится
// в цикле на 2-ю долю (основа с черты такта), овердаб на 3-ю долю — на 3-ю.
static void test_latency() {
  const uint64_t BAR = 96000, LAT = 480;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.set.latency_ms = 10.0f;
  s.apply();
  // музыкант играет короткий удар на доле beat (кадр внутри такта); в плагин приходит позже на LAT
  auto hit_at = [&](uint64_t beat) {
    return [beat, BAR, LAT](uint64_t t) {
      if (t < LAT) return 0.0f;
      const uint64_t x = (t - LAT) % BAR;
      return x >= beat && x < beat + 48 ? 0.5f : 0.0f;
    };
  };
  auto onset = [&](uint64_t from, uint64_t to) {
    for (uint64_t i = from; i < to; ++i)
      if (std::fabs(s.out_l[i]) > 0.05f) return (int64_t)(i - from);
    return (int64_t)-1;
  };
  s.in_l = hit_at(24000);
  s.run(BLOCK);
  s.cmd(Cmd::Rec);           // ждёт черты такта 1, запись с 1-го такта + LAT
  s.run(2 * BAR + 2000 - s.t);  // цикл закрылся сам (длина 1 такт)
  s.in_l = [](uint64_t) { return 0.0f; };
  CHECK(s.v(0).state == 3);
  s.run(3 * BAR + 1000 - s.t);
  CHECK(onset(2 * BAR + 3000, 3 * BAR) == 24000 - 3000);  // удар звучит ровно на 2-й доле
  // овердаб: удар на 3-ю долю
  s.cmd(Cmd::Rec);
  s.in_l = hit_at(48000);
  s.run(3 * BAR + 90000 - s.t);
  s.cmd(Cmd::Rec);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(5 * BAR - s.t);
  CHECK(onset(4 * BAR + 3000, 5 * BAR) == 24000 - 3000);
  CHECK(onset(4 * BAR + 30000, 5 * BAR) == 48000 - 30000);
  CHECK(s.max_step(2 * BAR + 2000, 3 * BAR) < 0.6f);
}

// Смена размера пула: лупы плавно затихают, пул меняется (старый освобождается раньше нового),
// пока меняется — Rec не начинает запись; потом всё работает, микшер не трогается.
static void test_pool_resize() {
  Sim s;  // пул 64 МБ
  CHECK(s.lp->pool_bytes() == (size_t)s.lp->total_chunks() * sizeof(Chunk));
  s.in_l = sine(220.0, 0.4f);
  s.cmd(Cmd::Rec);
  s.run(24000);
  s.cmd(Cmd::Rec);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(24000);
  Mix m;
  m.level_db = -3.0f;
  s.lp->set_mix(0, m);
  const uint64_t t0 = s.t;
  s.lp->set_pool_size((size_t)128 << 20);
  s.cmd(Cmd::TrackNext);
  s.cmd(Cmd::Rec);  // пока пул меняется — молча ничего
  s.run(BLOCK * 32);
  CHECK(!s.lp->pool_changing() && s.lp->pool_bytes() >= ((size_t)127 << 20) &&
        s.lp->pool_bytes() <= ((size_t)128 << 20));
  CHECK(s.v(0).state == 0 && s.v(1).state == 0 && s.lp->parts_mask() == 0u);
  CHECK(s.max_step(t0, s.t) < 0.05f);
  CHECK(rms(s.slice(s.t - BLOCK * 4, BLOCK * 4)) < 1e-6f);
  CHECK(s.lp->free_chunks() == s.lp->total_chunks());
  CHECK(s.lp->mix(0).level_db == -3.0f);
  // новый пул работает (курсор после стирания — на дорожке 1)
  CHECK(s.lp->active() == 0);
  s.in_l = sine(330.0, 0.3f);
  s.cmd(Cmd::Rec);
  s.run(24000);
  s.cmd(Cmd::Rec);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(24000);
  CHECK(s.v(0).state == 3 && rms(s.slice(s.t - 12000, 12000)) > 0.1f);
  // тот же размер ещё раз — ничего не происходит
  s.lp->set_pool_size(s.lp->pool_bytes());
  CHECK(!s.lp->pool_changing());
}

// Сетка 6×6 и 2×2: дорожки и парты по кругу в пределах сетки, Lock в пустой парт;
// смена сетки стирает лупы плавно и освобождает память, микшер и скорости не трогает.
static void test_grid() {
  Sim s;
  const uint32_t free0 = s.lp->free_chunks();
  CHECK(s.lp->grid() == 4);
  s.lp->set_grid(3);  // такой сетки нет
  CHECK(s.lp->grid() == 4);
  s.lp->set_grid(6);
  CHECK(s.lp->grid() == 6);
  for (int i = 1; i <= 6; ++i) {
    s.cmd(Cmd::TrackNext);
    CHECK(s.lp->active() == i % 6);
  }
  s.cmd(Cmd::TrackPrev);
  CHECK(s.lp->active() == 5);
  // дорожка 6 и дорожка 1 (с Lock по умолчанию) в парте 1
  s.in_l = sine(220.0, 0.4f);
  s.cmd(Cmd::Rec);
  s.run(24000);
  s.cmd(Cmd::Rec);
  s.cmd(Cmd::TrackNext);
  CHECK(s.lp->active() == 0);
  s.cmd(Cmd::Rec);
  s.run(24000);
  s.cmd(Cmd::Rec);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BLOCK * 8);
  CHECK(s.v(5).state == 3 && s.v(0).state == 3 && s.v(0).locked && s.lp->parts_mask() == 1u);
  // назад из парта 1 — парт 6 (пустой): Lock-дорожка переходит, дорожка 6 — нет
  s.set.part_q = 0;
  s.apply();
  s.cmd(Cmd::PartPrev);
  s.run(BLOCK * 8);
  CHECK(s.lp->part() == 5 && s.v(0).state == 3 && s.v(5).state == 0);
  CHECK(s.lp->parts_mask() == (1u | 1u << 5));
  // микшер и скорость дорожки 36 (парт 6, место 6) переживают смену сетки
  Mix m;
  m.level_db = -6.0f;
  s.lp->set_mix(35, m);
  s.lp->set_speed(35, 0.5f);
  const uint64_t t0 = s.t;
  s.lp->set_grid(2);
  CHECK(s.lp->grid() == 2 && s.lp->part() == 0 && s.lp->active() == 0 && s.lp->parts_mask() == 0u);
  s.run(BLOCK * 16);
  CHECK(s.max_step(t0, s.t) < 0.05f);             // лупы затихли плавно
  CHECK(rms(s.slice(s.t - BLOCK * 4, BLOCK * 4)) < 1e-6f);
  CHECK(s.lp->free_chunks() == free0);           // память вернулась в пул
  CHECK(s.lp->mix(35).level_db == -6.0f && s.lp->speed(35) == 0.5f);
  CHECK(s.v(0).locked && !s.v(1).locked);
  s.cmd(Cmd::TrackNext);
  s.cmd(Cmd::TrackNext);
  CHECK(s.lp->active() == 0);
  s.cmd(Cmd::PartNext);  // пустой парт: сразу
  CHECK(s.lp->part() == 1);
  s.cmd(Cmd::PartNext);
  CHECK(s.lp->part() == 0);
  // в 2×2 записывается и играет вторая дорожка второго парта
  s.cmd(Cmd::PartNext);
  s.cmd(Cmd::TrackNext);
  s.in_l = sine(330.0, 0.3f);
  s.cmd(Cmd::Rec);
  s.run(24000);
  s.cmd(Cmd::Rec);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(24000);
  CHECK(s.lp->active_index() == 3 && s.v(1).state == 3 && rms(s.slice(s.t - 12000, 12000)) > 0.1f);
  CHECK(s.lp->parts_mask() == 2u);
  // удержание Reset сбрасывает и микшер, сетка остаётся
  s.cmd(Cmd::ResetAll);
  s.run(BLOCK * 16);
  CHECK(s.lp->grid() == 2 && s.lp->mix(35).level_db == 0.0f && s.lp->speed(35) == 1.0f);
}

// Из пустого парта — сразу; запись закрывается на границе перехода; Lock во время овердаба.
static void test_parts_edges() {
  const uint64_t BAR = 96000;
  {
    Sim s(64, true);
    s.cmd(Cmd::PartNext);
    CHECK(s.lp->part() == 1 && s.lp->pending_part() == -1);
    s.cmd(Cmd::PartPrev);
    CHECK(s.lp->part() == 0);
  }
  {
    Sim s(64, true);
    s.ti.rolling = true;
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(BAR + BAR / 2);  // пишет с 1-го такта
    s.cmd(Cmd::PartNext);
    CHECK(s.lp->pending_part() == 1);
    s.run(BAR / 2 + BLOCK * 2);
    CHECK(s.lp->part() == 1);
    s.cmd(Cmd::PartPrev);  // парт 2 пустой — сразу назад
    CHECK(s.lp->part() == 0 && s.v(0).state == 3 && std::fabs(s.v(0).len_s - 2.0f) < 1e-3f);
  }
  {  // во 2-м парте на 2-й дорожке запись закрывается нажатием (длина по месту в парте)
    Sim s(64, true);
    s.ti.rolling = true;
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);
    s.cmd(Cmd::Rec);
    s.run(BAR * 2);
    s.cmd(Cmd::Rec);   // парт 1, дорожка 1: 1 такт
    s.run(BAR);
    s.cmd(Cmd::PartNext);
    s.run(BAR);
    CHECK(s.lp->part() == 1);
    CHECK(s.lp->active() == 1);  // первая — Lock-копия, активная — первая свободная
    s.cmd(Cmd::Rec);
    s.run(BAR + BAR / 2);
    CHECK(s.v(1).state == 2);
    s.cmd(Cmd::Rec);
    s.run(BAR);
    CHECK(s.v(1).state == 3 && std::fabs(s.v(1).len_s - 2.0f) < 1e-3f);
  }
  {  // Rec, пока переход ждёт: запись с границы в новом парте, старый парт не трогается
    Sim s(64, true);
    s.ti.rolling = true;
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);
    s.cmd(Cmd::Rec);
    s.run(BAR * 2);
    s.cmd(Cmd::Rec);   // парт 1, дорожка 1: 1 такт, играет
    s.run(BAR / 2);
    s.cmd(Cmd::PartNext);
    s.cmd(Cmd::Rec);
    CHECK(s.lp->rec_after_switch() && s.v(0).state == 3);  // в старом парте не овердаб
    s.run(BAR / 2 + BLOCK * 2);
    CHECK(s.lp->part() == 1 && s.lp->active() == 1 && s.v(1).state == 2);  // пишет с границы
    s.run(BAR);
    s.cmd(Cmd::Rec);
    s.run(BLOCK * 4);
    CHECK(s.v(1).state == 3 && std::fabs(s.v(1).len_s - 2.0f) < 1e-3f);
    s.cmd(Cmd::PartPrev);   // назад: старый парт без овердаба (1 слой — основа)
    s.run(BAR + BLOCK * 4);
    CHECK(s.lp->part() == 0 && s.v(0).state == 3 && s.v(0).layers == 1);
    // отмена: Rec дважды до границы
    s.cmd(Cmd::PartNext);
    s.cmd(Cmd::Rec);
    s.cmd(Cmd::Rec);
    CHECK(!s.lp->rec_after_switch());
  }
  {  // Lock-дорожка в овердабе: копия после затухания овердаба, без щелчка
    Sim s(64, true);
    s.ti.rolling = true;
    s.set.length[0] = 1;
    s.apply();
    s.in_l = sine(180.0, 0.4f);
    s.run(BLOCK);  // мимо сильной доли 0 (пуск транспорта): запись с 1-го такта
    s.cmd(Cmd::Rec);
    s.run(2 * BAR + BLOCK * 8);
    s.in_l = sine(400.0, 0.2f);
    s.run(BAR / 4);
    s.cmd(Cmd::Rec);  // овердаб
    s.run(BAR / 2);
    s.cmd(Cmd::PartNext);
    s.run(BAR / 4 + BLOCK * 8);
    s.in_l = [](uint64_t) { return 0.0f; };
    CHECK(s.lp->part() == 1 && s.v(0).state == 3 && s.v(0).locked);
    s.run(BAR);
    CHECK(rms(s.slice(s.t - BAR / 2, BAR / 2)) > 0.1f);
    CHECK(s.max_step(2 * BAR, s.t) < 0.05f);
  }
}

// Смена парта ждёт конца цикла самого длинного лупа (PART AT = LOOP END, по умолчанию);
// PART AT = BAR — ближайший такт.
static void test_part_at_loop_end() {
  const uint64_t BAR = 96000;
  for (int q : {3, 2}) {
    Sim s(64, true);
    s.ti.rolling = true;
    s.set.length[0] = 2;
    s.set.part_q = q;
    s.apply();
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);
    s.cmd(Cmd::Rec);
    s.run(3 * BAR + BLOCK * 8);  // цикл 2 такта: с 1-го по 3-й
    s.in_l = [](uint64_t) { return 0.0f; };
    CHECK(s.v(0).state == 3 && std::fabs(s.v(0).len_s - 4.0f) < 1e-3f);
    s.run((uint64_t)(BAR * 0.5) - BLOCK * 8);  // 3,5 такта: середина первого такта цикла
    s.cmd(Cmd::PartNext);
    s.run(BAR / 2 + BLOCK);                    // прошли черту 4-го такта (середина цикла)
    CHECK(s.lp->part() == (q == 3 ? 0 : 1));
    s.run(BAR);                                 // прошли конец цикла (5-й такт)
    CHECK(s.lp->part() == 1);
  }
}

// Микшер (этап 4): режимы каналов, уровень, сглаживание, ограничитель, сброс, перенос Lock.
static void test_mixer() {
  const uint64_t L = 48000;
  auto rec_left_only = [&](Sim& s) {  // луп: синус только в L, R — тишина
    s.in_l = sine(220.0, 0.4f);
    s.in_r = [](uint64_t) { return 0.0f; };
    s.cmd(Cmd::Rec);
    s.run(L);
    s.cmd(Cmd::Rec);
    s.run(BLOCK * 8);
    s.in_l = [](uint64_t) { return 0.0f; };
    s.run(L);
  };
  auto peak_lr = [&](Sim& s, float& pl, float& pr) {
    pl = pr = 0;
    for (uint64_t i = s.t - L / 2; i < s.t; ++i) {
      pl = std::fmax(pl, std::fabs(s.out_l[i]));
      pr = std::fmax(pr, std::fabs(s.out_r[i]));
    }
  };
  {  // сухой вход — через уровень, панораму и режим активной дорожки; смена дорожки без щелчка
    Sim s;
    s.lp->set_monitor(true);
    s.in_l = sine(220.0, 0.4f);
    s.in_r = [](uint64_t) { return 0.0f; };
    s.run(L);
    float pl, pr;
    peak_lr(s, pl, pr);
    CHECK(std::fabs(pl - 0.4f) < 0.005f && pr < 1e-4f);  // по умолчанию как есть
    Mix m;
    m.mode = 1;  // активная: «L» посередине
    s.lp->set_mix(s.lp->active_index(), m);
    Mix q;
    q.level_db = kMinDb;  // следующая: −∞
    s.lp->set_mix(s.lp->active_index() + 1, q);
    s.run(L);
    peak_lr(s, pl, pr);
    CHECK(std::fabs(pl - pr) < 0.005f && std::fabs(pl - 0.4f * 0.70711f) < 0.005f);
    const uint64_t t0 = s.t;
    s.cmd(Cmd::TrackNext);
    s.run(L);
    peak_lr(s, pl, pr);
    CHECK(pl < 1e-4f && pr < 1e-4f);
    float mx = 0;  // затухание плавное: шаг не больше, чем у самого синуса
    for (uint64_t i = t0 + 1; i < t0 + 4800; ++i) mx = std::fmax(mx, std::fabs(s.out_l[i] - s.out_l[i - 1]));
    CHECK(mx < 0.4f * (float)(PI2 * 220.0 / SR) * 1.05f);
  }
  {  // «L»: левый вход при панораме 0 посередине, на краях с одной стороны; «R»: левый не слышен
    Sim s;
    rec_left_only(s);
    const int k = s.lp->active_index();
    Mix m;
    m.mode = 1;
    s.lp->set_mix(k, m);
    s.run(L);
    float pl, pr;
    peak_lr(s, pl, pr);
    CHECK(std::fabs(pl - pr) < 0.005f && std::fabs(pl - 0.4f * 0.70711f) < 0.005f);
    m.pan = 1.0f;
    s.lp->set_mix(k, m);
    s.run(L);
    peak_lr(s, pl, pr);
    CHECK(pl < 1e-4f && pr > 0.39f && pr < 0.41f);
    m.pan = -1.0f;
    s.lp->set_mix(k, m);
    s.run(L);
    peak_lr(s, pl, pr);
    CHECK(pr < 1e-4f && pl > 0.39f && pl < 0.41f);
    m.pan = 0.0f;
    m.mode = 2;  // только R: записан только L — тишина
    s.lp->set_mix(k, m);
    s.run(L);
    peak_lr(s, pl, pr);
    CHECK(pl < 1e-4f && pr < 1e-4f);
  }
  {  // «Стерео»: центр как записано, влево — правый канал уходит, левый +3 дБ
    Sim s;
    rec_left_only(s);
    const int k = s.lp->active_index();
    Mix m;
    m.pan = -1.0f;
    s.lp->set_mix(k, m);
    s.run(L);
    float pl, pr;
    peak_lr(s, pl, pr);
    CHECK(std::fabs(pl - 0.4f * 1.41421f) < 0.01f && pr < 1e-6f);
    // уровень −6 дБ, смена без щелчка
    m.pan = 0.0f;
    m.level_db = -6.0206f;
    const uint64_t t0 = s.t;
    s.lp->set_mix(k, m);
    s.run(L);
    peak_lr(s, pl, pr);
    CHECK(std::fabs(pl - 0.2f) < 0.005f);
    CHECK(s.max_step(t0 - 10, s.t) < 0.03f);
    // −∞
    m.level_db = -60.0f;
    s.lp->set_mix(k, m);
    s.run(L);
    peak_lr(s, pl, pr);
    CHECK(pl < 1e-6f);
  }
  {  // ограничитель мастера: две громкие дорожки не клиппят
    Sim s;
    s.in_l = sine(220.0, 0.9f);
    s.in_r = sine(220.0, 0.9f);
    s.cmd(Cmd::Rec);
    s.run(L);
    s.cmd(Cmd::Rec);
    s.cmd(Cmd::TrackNext);
    s.cmd(Cmd::Rec);
    s.run(L);
    s.cmd(Cmd::Rec);
    s.in_l = s.in_r = [](uint64_t) { return 0.0f; };
    s.run(L * 2);
    float pl, pr;
    peak_lr(s, pl, pr);
    CHECK(pl <= 1.0f && pr <= 1.0f && pl > 0.8f);
    CHECK(s.lp->master_peak(0) > 0.8f && s.lp->master_peak(0) <= 1.0f && s.lp->peak(0) > 0.5f);
    s.lp->set_limiter(false);
    s.run(L);
    peak_lr(s, pl, pr);
    CHECK(pl > 1.5f);  // без ограничителя сумма двух 0,9
    // сброс удержанием Reset: микшер в значения по умолчанию
    Mix m;
    m.level_db = -12.0f;
    m.mode = 1;
    s.lp->set_mix(3, m);
    s.lp->set_master_db(-6.0f);
    s.lp->set_speed(2, 0.5f);
    s.lp->set_speed(9, 1.5f);
    while (s.lp->active() != 2) s.cmd(Cmd::TrackNext);
    const uint32_t ep = s.lp->mix_epoch();
    s.cmd(Cmd::ResetAll);
    CHECK(s.lp->mix(3).level_db == 0.0f && s.lp->mix(3).mode == 0 && s.lp->master_db() == 0.0f &&
          s.lp->limiter() && s.lp->mix_epoch() != ep);
    // скорость всех дорожек — 1×, курсор на первой дорожке первого парта
    CHECK(s.lp->speed(2) == 1.0f && s.lp->speed(9) == 1.0f);
    CHECK(s.lp->active() == 0 && s.lp->part() == 0);
  }
  {  // Lock-копия переносит уровень, панораму и режим
    const uint64_t BAR = 96000;
    Sim s(64, true);
    s.ti.rolling = true;
    s.set.length[0] = 1;
    s.apply();
    s.in_l = sine(200.0, 0.3f);
    s.run(BLOCK);
    s.cmd(Cmd::Rec);
    s.run(2 * BAR + BLOCK * 8);
    s.in_l = [](uint64_t) { return 0.0f; };
    Mix m;
    m.level_db = -9.0f;
    m.pan = 0.3f;
    m.mode = 1;
    s.lp->set_mix(0, m);
    s.cmd(Cmd::PartNext);
    s.run(BAR + BLOCK * 4);
    CHECK(s.lp->part() == 1);
    const Mix& c = s.lp->mix(4);
    CHECK(c.level_db == -9.0f && c.pan == 0.3f && c.mode == 1);
    CHECK(s.max_step(2 * BAR, s.t) < 0.03f);
  }
}

// Частота по пересечениям нуля на отрезке.
static double zc_freq(const std::vector<float>& v) {
  int n = 0;
  for (size_t i = 1; i < v.size(); ++i)
    if ((v[i - 1] < 0) != (v[i] < 0)) ++n;
  return n / 2.0 / ((double)v.size() / SR);
}

// Ошибка фазы дорожки (кадры) относительно сетки: цикл в 1 такт начинается на черте.
static double phase_err(Sim& s, double beats, double loop_beats, uint64_t L) {
  double ph = std::fmod(beats / loop_beats, 1.0);
  if (ph < 0) ph += 1.0;
  double err = (double)s.v().pos * (double)L - ph * (double)L;
  if (err > L / 2.0) err -= (double)L;
  if (err < -(double)L / 2.0) err += (double)L;
  return err;
}

// Следование темпу: 120 -> 140 без щелчков и вибрато, после установления ошибка ≤ 1 мс.
static void test_tempo_follow() {
  const uint64_t BAR = 96000;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(200.0, 0.3f);
  s.run(BLOCK);
  s.cmd(Cmd::Rec);
  s.run(2 * BAR + BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BAR);
  CHECK(std::fabs(phase_err(s, s.beats_now(), 4.0, BAR)) <= 48.0);
  // темп 140 с этого момента
  const uint64_t tc = s.t;
  const double bc = s.beats_now();
  s.bpm = 140.0;
  s.beats_at = [tc, bc](uint64_t t) { return bc + (double)(t - tc) * 140.0 / 60.0 / SR; };
  // переход: скорость переходит цель не больше чем на 15 % шага, фаза не дальше 10 мс
  float pk = 0;
  double emx = 0;
  for (int k = 0; k < 100; ++k) {
    s.run(BLOCK * 4);
    pk = std::fmax(pk, s.v().espeed);
    emx = std::fmax(emx, std::fabs(phase_err(s, s.beats_now(), 4.0, BAR)));
  }
  CHECK(pk - 140.0f / 120.0f < 0.15f * (140.0f / 120.0f - 1.0f));
  CHECK(emx < 0.01 * SR);
  std::printf("  tempo 120->140: overshoot %.2f %%, max phase error %.1f ms\n", (pk * 120.0 / 140.0 - 1.0) * 100.0,
              emx / SR * 1000.0);
  s.run((uint64_t)(SR * 3));
  const double e = phase_err(s, s.beats_now(), 4.0, BAR);
  CHECK(std::fabs(e) <= 48.0);  // ≤ 1 мс
  // скорость установилась: 140/120, без вибрато
  float mn = 9, mx = 0;
  for (int k = 0; k < 40; ++k) {
    s.run(BLOCK * 10);
    mn = std::fmin(mn, s.v().espeed);
    mx = std::fmax(mx, s.v().espeed);
  }
  CHECK(std::fabs(mx - 140.0f / 120.0f) < 0.002f && mx - mn < 0.001f);
  CHECK(std::fabs(phase_err(s, s.beats_now(), 4.0, BAR)) <= 48.0);
  CHECK(s.max_step(2 * BAR, s.t) < 0.03f);
  std::printf("  tempo 120->140: phase error after 4 s = %.1f frames\n", e);
  s.wav("tempo_follow");
}

// Темп крутилкой: 120 -> 140 по 1 BPM каждые 40 мс. Без щелчков, фаза не дальше 10 мс,
// после остановки крутилки ошибка ≤ 1 мс.
static void test_tempo_knob() {
  const uint64_t BAR = 96000;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(200.0, 0.3f);
  s.run(BLOCK);
  s.cmd(Cmd::Rec);
  s.run(2 * BAR + BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BAR);
  const uint64_t t0 = s.t;
  double emx = 0;
  for (int b = 121; b <= 140; ++b) {
    const uint64_t tc = s.t;
    const double bc = s.beats_now();
    s.bpm = b;
    s.beats_at = [tc, bc, b](uint64_t t) { return bc + (double)(t - tc) * b / 60.0 / SR; };
    for (int k = 0; k < 15; ++k) {  // 15 блоков по 128 = 40 мс
      s.run(BLOCK);
      emx = std::fmax(emx, std::fabs(phase_err(s, s.beats_now(), 4.0, BAR)));
    }
  }
  CHECK(emx < 0.01 * SR);
  s.run((uint64_t)(SR * 3));
  CHECK(std::fabs(phase_err(s, s.beats_now(), 4.0, BAR)) <= 48.0);
  CHECK(std::fabs(s.v().espeed - 140.0f / 120.0f) < 0.002f);
  CHECK(s.max_step(t0, s.t) < 0.03f);
  std::printf("  tempo knob 120->140: max phase error %.1f ms\n", emx / SR * 1000.0);
}

// Ручная скорость: плавный ход без щелчков, 0,25× снизу; сброс — к 1× и в фазу с тактом.
static void test_manual_speed() {
  const uint64_t BAR = 96000;
  Sim s(64, true);
  s.ti.rolling = true;
  s.set.length[0] = 1;
  s.apply();
  s.in_l = sine(200.0, 0.3f);
  s.run(BLOCK);
  s.cmd(Cmd::Rec);
  s.run(2 * BAR + BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BAR);
  const int k = s.lp->active_index();
  s.lp->set_speed(k, 2.0f);
  s.run(BAR / 2);
  CHECK(std::fabs(s.v().speed - 2.0f) < 1e-4f);
  CHECK(std::fabs(zc_freq(s.slice(s.t - 9600, 9600)) - 400.0) < 6.0);
  s.lp->set_speed(k, 0.1f);  // ниже предела
  CHECK(s.lp->speed(k) == 0.25f);
  s.run(BAR / 2);
  CHECK(std::fabs(zc_freq(s.slice(s.t - 19200, 19200)) - 50.0) < 4.0);
  CHECK(s.max_step(3 * BAR, s.t) < 0.03f);
  s.run(BAR + BAR / 3);  // фаза уехала
  CHECK(std::fabs(phase_err(s, s.beats_now(), 4.0, BAR)) > 1000.0);
  s.lp->speed_reset(k);
  s.run(2 * BAR);  // доехала до 1×, на ближайшем такте встала в фазу
  CHECK(s.v().speed == 1.0f);
  CHECK(std::fabs(phase_err(s, s.beats_now(), 4.0, BAR)) <= 48.0);
  CHECK(s.max_step(3 * BAR, s.t) < 0.03f);
  // привязка по полутонам: 1,1 -> 2 полутона
  s.set.spd_snap = 1;
  s.apply();
  s.lp->set_speed(k, 1.1f);
  s.run(BAR / 2);
  CHECK(std::fabs(s.v().speed - 1.12246f) < 1e-3f);
  s.wav("manual_speed");
}

// Овердаб на 1,5× как на ленте: при возврате к 1× слой звучит в 1,5 раза ниже.
static void test_dub_at_speed() {
  Sim s;  // Free: без следования темпу
  const uint64_t L = 48000;
  s.cmd(Cmd::Rec);
  s.run(L);  // основа — тишина
  s.cmd(Cmd::Rec);
  s.run(L);
  const int k = s.lp->active_index();
  s.lp->set_speed(k, 1.5f);
  s.run(L / 2);
  s.in_l = sine(300.0, 0.3f);
  s.cmd(Cmd::Rec);
  s.run(L);  // полтора цикла на 1,5×
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.lp->set_speed(k, 1.0f);
  s.run(L);
  CHECK(std::fabs(zc_freq(s.slice(s.t - L / 2, L / 2)) - 200.0) < 4.0);
  CHECK(rms(s.slice(s.t - L / 2, L / 2)) > 0.15f);
  s.wav("dub_at_speed");
}


// ---------------------------------------------------------------- сдвиг высоты (этап 8)

// Записать луп длиной L из сигнала f (Free, RPO), дальше вход — тишина.
static void rec_loop(Sim& s, std::function<float(uint64_t)> f, uint64_t L) {
  s.in_l = f;
  s.in_r = nullptr;
  s.cmd(Cmd::Rec);
  s.run(L);
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 4);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.run(BLOCK * 8);
}

// Лаг (кадров) максимума взаимной корреляции b относительно a в пределах ±maxlag и её величина.
static int best_lag(const std::vector<float>& a, const std::vector<float>& b, int maxlag, double* corr) {
  int best = 0;
  double bc = -1e30, na = 0, nb = 0;
  for (size_t i = 0; i < a.size(); ++i) na += a[i] * a[i];
  for (size_t i = 0; i < b.size(); ++i) nb += b[i] * b[i];
  for (int lag = -maxlag; lag <= maxlag; ++lag) {
    double c = 0;
    for (int i = maxlag; i + maxlag < (int)a.size(); ++i) c += a[i] * b[i + lag];
    if (c > bc) {
      bc = c;
      best = lag;
    }
  }
  if (corr) *corr = bc / std::sqrt(na * nb + 1e-30);
  return best;
}

// Сигнал с атаками: шумовые всплески каждые 0,25 с и тихий тон.
static float burst_sig(uint64_t t) {
  const uint64_t ph = t % 12000;
  float v = 0.15f * (float)std::sin(PI2 * 331.0 * (double)t / SR);
  if (ph < 480) {
    uint32_t x = (uint32_t)(t * 2654435761u);
    x ^= x >> 13;
    v += 0.5f * ((float)(x & 0xffff) / 32768.0f - 1.0f) * (1.0f - (float)ph / 480.0f);
  }
  return v;
}

// Сдвинутый звук в фазе: тот же луп со сдвигом 0,01 полутона совпадает с исходным по времени.
static void test_pitch_align() {
  const uint64_t L = 48000;
  Sim a, b;
  rec_loop(a, burst_sig, L);
  rec_loop(b, burst_sig, L);
  CHECK(b.lp->pitch_latency() > 4000);
  b.lp->set_pitch(0, 0.01f);
  a.run(L * 2);
  b.run(L * 2);
  double c = 0;
  const int lag = best_lag(a.slice(a.t - L, L), b.slice(b.t - L, L), 1500, &c);
  std::printf("  pitch align: lag %d frames, corr %.3f\n", lag, c);
  CHECK(std::abs(lag) <= 48);  // не больше 1 мс
  CHECK(c > 0.8);  // шум после Stretch теряет точную фазу
  CHECK(b.v().pitch == 0.01f);
}

// Высота: +12 — октава вверх, −12 вниз, +7 — квинта; со скоростью 2× и −12 — исходная.
static void test_pitch_freq() {
  const uint64_t L = 48000;
  Sim s;
  rec_loop(s, sine(220.0, 0.5f), L);
  struct Case { float st, spd; double f; } cases[] = {{12, 1, 440}, {-12, 1, 110}, {7, 1, 220 * 1.4983}, {-12, 2, 220}, {0, 1, 220}};
  for (const Case& c : cases) {
    s.lp->set_speed(0, c.spd);
    s.lp->set_pitch(0, c.st);
    s.run(L);
    const double f = zc_freq(s.slice(s.t - L / 2, L / 2));
    std::printf("  pitch %+.0f speed %.0fx: %.1f Hz (want %.1f)\n", c.st, c.spd, f, c.f);
    CHECK(std::fabs(f - c.f) < c.f * 0.02);
    CHECK(rms(s.slice(s.t - L / 2, L / 2)) > 0.25f);
  }
}

// Включение, смена и выключение сдвига без щелчков; в аудиопотоке память не выделяется.
static void test_pitch_smooth() {
  const uint64_t L = 48000;
  Sim s;
  rec_loop(s, sine(220.0, 0.5f), L);
  s.count_allocs = true;
  g_allocs = 0;
  const uint64_t t0 = s.t;
  s.run(L / 2);
  s.lp->set_pitch(0, 5);
  s.run(L / 2);
  s.lp->set_pitch(0, -3);  // плавный ход между значениями
  s.run(L / 2);
  s.lp->set_pitch(0, 0);
  s.run(L / 2);
  s.lp->set_pitch(0, 12);
  s.run(L / 4);
  s.cmd(Cmd::Mute);
  s.run(L / 2);
  s.cmd(Cmd::Mute);
  s.run(L / 2);
  s.cmd(Cmd::Reverse);
  s.run(L);
  s.set.pitch_q = 1;  // HIGH: дорожка перезапускается в другом качестве
  s.apply();
  s.run(L);
  CHECK(s.lp->pitch_latency() > 4000);
  const float ms = s.max_step(t0, s.t);
  for (uint64_t x = t0; x + 4800 < s.t; x += 4800)  // где щёлкнуло (если щёлкнуло)
    if (s.max_step(x, x + 4800) > 0.06f) std::printf("    step %.3f at %.2f s\n", s.max_step(x, x + 4800), (x - t0) / SR);
  std::printf("  pitch smooth: max step %.4f, allocs %d\n", ms, g_allocs);
  CHECK(ms < 0.06f);
  CHECK(g_allocs == 0);
  CHECK(std::fabs(zc_freq(s.slice(s.t - L / 2, L / 2)) - 440.0) < 9.0);
  s.count_allocs = false;
  s.wav("pitch_smooth");
}

// Овердаб «как слышно»: музыкант играет то, что слышит на +12; при 0 овердаб совпадает с основой
// по высоте и месту (сумма вдвое громче исходной, в фазе).
static void test_pitch_dub() {
  const uint64_t L = 48000;
  Sim s;
  s.set.latency_ms = (float)(BLOCK * 1000.0 / SR);  // вход ниже — выход, задержанный на блок
  s.apply();
  rec_loop(s, burst_sig, L);
  s.run(L / 2);
  const uint64_t T0 = s.t;
  s.run(L);
  const std::vector<float> ref = s.slice(T0, L);
  s.lp->set_pitch(0, 12);
  s.run(L);
  s.count_allocs = true;
  g_allocs = 0;
  // «играем» то, что слышим (выход, задержанный на блок: как вход через аудиоинтерфейс)
  s.in_l = [&s](uint64_t t) { return t >= BLOCK && t - BLOCK < s.out_l.size() ? s.out_l[t - BLOCK] : 0.0f; };
  s.in_r = [&s](uint64_t t) { return t >= BLOCK && t - BLOCK < s.out_r.size() ? s.out_r[t - BLOCK] : 0.0f; };
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 4);
  CHECK(s.v().state == 4);
  s.run(L);
  s.in_l = [](uint64_t) { return 0.0f; };
  s.in_r = nullptr;
  s.cmd(Cmd::Rec);
  s.run(BLOCK * 8);
  CHECK(s.v().state == 4);  // дописывает то, что ещё в Stretch
  s.run(L / 2);
  CHECK(s.v().state == 3);
  CHECK(s.v().layers == 2);
  CHECK(g_allocs == 0);
  s.count_allocs = false;
  s.lp->set_pitch(0, 0);
  s.run(L);
  const uint64_t T1 = s.t;
  s.run(L);
  std::vector<float> got(L), want(L);
  const uint64_t d = (T1 - T0) % L;
  for (uint64_t i = 0; i < L; ++i) {
    got[i] = s.out_l[T1 + i];
    want[i] = ref[(i + d) % L];
  }
  // овердаб = сумма − основа; после двух сдвигов шум не в фазе с основой, поэтому место
  // сверяем по огибающей (всплески там же), а громкость — по RMS
  std::vector<float> dub(L), env_d(L), env_b(L);
  for (uint64_t i = 0; i < L; ++i) dub[i] = got[i] - want[i];
  float ed = 0, eb = 0;
  for (uint64_t i = 0; i < L; ++i) {
    ed += (std::fabs(dub[i]) - ed) * 0.02f;
    eb += (std::fabs(want[i]) - eb) * 0.02f;
    env_d[i] = ed;
    env_b[i] = eb;
  }
  double c = 0;
  const int lag = best_lag(env_b, env_d, 1500, &c);
  const float ratio = rms(dub) / rms(want);
  std::printf("  pitch dub: envelope lag %d frames, corr %.3f, dub level x%.2f\n", lag, c, ratio);
  CHECK(std::abs(lag) <= 96);  // не больше 2 мс
  CHECK(c > 0.9);
  CHECK(ratio > 0.7f && ratio < 1.3f);
  s.wav("pitch_dub");
}

// Перегрузка: Stretch не укладывается в долю блока — сдвиг выключается, дорожка звучит без него.
static void test_pitch_overload() {
  const uint64_t L = 48000;
  Sim s;
  rec_loop(s, sine(220.0, 0.5f), L);
  s.lp->set_pitch_budget(0.0f);
  s.lp->set_pitch(0, 12);
  s.run(L / 2);
  CHECK(s.lp->pitch_overload());
  s.run(L / 2);
  CHECK(std::fabs(zc_freq(s.slice(s.t - L / 4, L / 4)) - 220.0) < 5.0);
  CHECK(rms(s.slice(s.t - L / 4, L / 4)) > 0.3f);
  s.lp->set_pitch_budget(0.5f);
  s.lp->set_pitch(0, 7);  // новое значение — новая попытка
  CHECK(!s.lp->pitch_overload());
  s.run(L);
  CHECK(!s.lp->pitch_overload());
  CHECK(std::fabs(zc_freq(s.slice(s.t - L / 2, L / 2)) - 329.6) < 7.0);
}

// Новая основа — без сдвига; Lock-копия в пустом парте продолжает со сдвигом без шва.
static void test_pitch_parts() {
  const uint64_t L = 48000;
  Sim s;
  s.lp->set_pitch(0, 5);
  rec_loop(s, sine(220.0, 0.5f), L);
  CHECK(s.v().pitch == 0.0f);
  s.lp->set_pitch(0, 12);
  s.run(L);
  const uint64_t t0 = s.t;
  s.cmd(Cmd::PartNext);  // пустой парт 2, переход в конце цикла: дорожка 1 с Lock копируется
  s.run(L * 2);
  CHECK(s.lp->part() == 1);
  const int g = s.lp->grid();
  CHECK(s.lp->pitch(g) == 12.0f);
  CHECK(s.v().state == 3);
  const float ms = s.max_step(t0, s.t);
  std::printf("  pitch lock copy: max step %.4f\n", ms);
  CHECK(ms < 0.06f);
  CHECK(std::fabs(zc_freq(s.slice(s.t - L / 2, L / 2)) - 440.0) < 9.0);
  // удержание Reset: сдвиг всех дорожек — в 0
  s.cmd(Cmd::ResetAll);
  s.run(BLOCK * 8);
  CHECK(s.lp->pitch(g) == 0.0f);
}

// Сдвиг вместе со скоростью: плавный ход 1× → 1,5× → 0,75× без щелчков, высота = скорость × сдвиг.
static void test_pitch_speed_glide() {
  const uint64_t L = 48000;
  Sim s;
  rec_loop(s, sine(220.0, 0.5f), L);
  s.lp->set_pitch(0, 7);
  s.run(L / 2);
  const uint64_t t0 = s.t;
  s.lp->set_speed(0, 1.5f);
  s.run(L);
  CHECK(std::fabs(zc_freq(s.slice(s.t - L / 4, L / 4)) - 220 * 1.5 * 1.4983) < 10.0);
  s.lp->set_speed(0, 0.75f);
  s.run(L);
  CHECK(std::fabs(zc_freq(s.slice(s.t - L / 4, L / 4)) - 220 * 0.75 * 1.4983) < 5.0);
  const float ms = s.max_step(t0, s.t);
  std::printf("  pitch + speed glide: max step %.4f\n", ms);
  CHECK(ms < 0.06f);
  s.wav("pitch_speed");
}

// Шесть сдвинутых дорожек (6×6), остановка и пуск транспорта (Quantize), переход в записанный парт:
// без щелчков и без выделения памяти; дорожки входят в фазе.
static void test_pitch_many() {
  Sim s(64, true);
  s.lp->set_grid(6);
  s.run(BLOCK);
  const uint64_t bar = (uint64_t)(4 * 60.0 / 120.0 * SR);  // 1 такт при 120
  const double fr[6] = {110, 165, 220, 275, 330, 385};
  for (int k = 0; k < 6; ++k) {
    s.in_l = sine(fr[k], 0.12f);
    s.cmd(Cmd::Rec);
    while (s.v(k).state != 2) s.run(BLOCK);
    s.run(bar - BLOCK);
    s.cmd(Cmd::Rec);
    while (s.v(k).state != 3) s.run(BLOCK);
    s.in_l = [](uint64_t) { return 0.0f; };
    s.cmd(Cmd::TrackNext);
  }
  for (int k = 0; k < 6; ++k) s.lp->set_pitch(k, (float)(k % 2 ? -5 : 4));
  s.run(bar);
  s.count_allocs = true;
  g_allocs = 0;
  const uint64_t t0 = s.t;
  s.ti.rolling = false;  // стоп транспорта: затихают
  s.run(bar);
  s.ti.rolling = true;
  const uint64_t ts = s.t;
  s.beats_at = [ts](uint64_t t) { return (double)(t - ts) * 120.0 / 60.0 / SR; };
  s.run(bar * 2);
  CHECK(rms(s.slice(s.t - bar / 2, bar / 2)) > 0.1f);
  const float ms = s.max_step(t0, s.t);
  std::printf("  6 pitched tracks, stop/start: max step %.4f, allocs %d\n", ms, g_allocs);
  CHECK(ms < 0.08f);
  CHECK(g_allocs == 0);
  s.count_allocs = false;
}

int main() {
  test_pitch_align();
  test_pitch_freq();
  test_pitch_smooth();
  test_pitch_dub();
  test_pitch_overload();
  test_pitch_parts();
  test_pitch_speed_glide();
  test_pitch_many();
  test_record_play();
  test_overdubs_undo();
  test_mute();
  test_reverse();
  test_reverse_quantized();
  test_transport_restart_resync();
  test_transport_stop_start();
  test_play_stop();
  test_peaks();
  test_rec_peaks();
  test_rop_leave();
  test_latency();
  test_pool_resize();
  test_grid();
  test_parts_lock();
  test_parts_edges();
  test_part_at_loop_end();
  test_mixer();
  test_tempo_follow();
  test_tempo_knob();
  test_manual_speed();
  test_dub_at_speed();
  test_quantize();
  test_count_in();
  test_threshold();
  test_rop();
  test_reset();
  test_pool_full();
  std::printf("looper: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
