// Тесты LV2-обёртки без хоста: ручки страницы 5 с захватом (catch), смена сетки, сессии в пресетах.
#include <cstdio>
#include <initializer_list>
#include <thread>

#include <utime.h>

#include "../lv2/eka_tape.cpp"

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

// ---------------------------------------------------------------- сессии (этап 7а)
// Хост-заглушка, как jalv в Zynthian: makePath в папку пресетов, mapPath, рабочий поток — сразу,
// save() — в своём потоке, пока «аудиопоток» (main) крутит run().

static std::vector<std::string> g_uris;
static LV2_URID map_uri(LV2_URID_Map_Handle, const char* u) {
  for (size_t i = 0; i < g_uris.size(); ++i)
    if (g_uris[i] == u) return (LV2_URID)(i + 1);
  g_uris.push_back(u);
  return (LV2_URID)g_uris.size();
}

struct KV {
  uint32_t key, type;
  std::vector<char> data;
};
static std::vector<KV> g_state;
static LV2_State_Status st_store(LV2_State_Handle, uint32_t key, const void* v, size_t n, uint32_t type, uint32_t) {
  for (auto& kv : g_state)
    if (kv.key == key) {
      kv.type = type;
      kv.data.assign((const char*)v, (const char*)v + n);
      return LV2_STATE_SUCCESS;
    }
  g_state.push_back(KV{key, type, std::vector<char>((const char*)v, (const char*)v + n)});
  return LV2_STATE_SUCCESS;
}
static const void* st_retrieve(LV2_State_Handle h, uint32_t key, size_t* n, uint32_t* type, uint32_t* flags) {
  auto* st = static_cast<std::vector<KV>*>(h);
  for (auto& kv : *st)
    if (kv.key == key) {
      *n = kv.data.size();
      *type = kv.type;
      *flags = 0;
      return kv.data.data();
    }
  return nullptr;
}
static const KV* st_find(const std::vector<KV>& st, LV2_URID key) {
  for (auto& kv : st)
    if (kv.key == key) return &kv;
  return nullptr;
}

static std::string g_bundle;
static char* mk_path(LV2_State_Make_Path_Handle, const char* p) { return strdup((g_bundle + "/" + p).c_str()); }
static char* abs_to_abstract(LV2_State_Map_Path_Handle, const char* p) {
  const std::string pre = g_bundle + "/";
  return strdup(!std::strncmp(p, pre.c_str(), pre.size()) ? p + pre.size() : p);
}
static char* abstract_to_abs(LV2_State_Map_Path_Handle, const char* p) {
  return strdup(p[0] == '/' ? p : (g_bundle + "/" + p).c_str());
}
static void fr_path(LV2_State_Free_Path_Handle, char* p) { free(p); }

static Plugin* g_sched_pl = nullptr;
static LV2_Worker_Status respond_now(LV2_Worker_Respond_Handle, uint32_t size, const void* data) {
  return work_response(g_sched_pl, size, data);
}
static LV2_Worker_Status schedule_now(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data) {
  return work(static_cast<Plugin*>(h), respond_now, nullptr, size, data);
}

struct Host {
  Plugin* pl = nullptr;
  LV2_URID_Map map{nullptr, map_uri};
  LV2_Worker_Schedule sched{nullptr, schedule_now};
  float ctl[P_COUNT] = {};
  float in_l[128], in_r[128], out_l[128], out_r[128];
  alignas(8) uint8_t ev_in[256], ev_out[8192];
  uint64_t t = 0;
  float peak_out = 0;
  explicit Host(bool roll = true) {
    pl = create_plugin(48000.0, &map, &sched, 64);
    sched.handle = pl;
    for (uint32_t p = 0; p < P_COUNT; ++p) connect_port(pl, p, &ctl[p]);
    connect_port(pl, P_IN_L, in_l);
    connect_port(pl, P_IN_R, in_r);
    connect_port(pl, P_OUT_L, out_l);
    connect_port(pl, P_OUT_R, out_r);
    connect_port(pl, P_EVENTS, ev_in);
    connect_port(pl, P_EVENTS_OUT, ev_out);
    auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(ev_in);
    seq->atom.type = pl->u.atom_Sequence;
    seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
    seq->body.unit = seq->body.pad = 0;
    if (roll) {
      PositionMsg m;
      m.has_speed = m.has_bpm = m.has_pos = true;
      m.speed = 1;
      m.bpm = 120;
      m.bar = m.bar_beat = 0;
      pl->clock.on_position(m);
    }
  }
  ~Host() { cleanup(pl); }
  void block(float amp = 0.0f) {
    for (int i = 0; i < 128; ++i) {
      in_l[i] = amp * (float)std::sin(6.283185307 * 220.0 * (double)(t + i) / 48000.0);
      in_r[i] = in_l[i] * 0.5f;
    }
    reinterpret_cast<LV2_Atom_Sequence*>(ev_out)->atom.size = sizeof ev_out - sizeof(LV2_Atom);
    g_sched_pl = pl;
    run(pl, 128);
    for (int i = 0; i < 128; ++i) peak_out = std::fmax(peak_out, std::fabs(out_l[i]));
    t += 128;
  }
  void blocks(int n, float amp = 0.0f) {
    for (int i = 0; i < n; ++i) block(amp);
  }
  // save() в своём потоке (как jalv), здесь крутится аудио
  void save_state(std::vector<KV>& st) {
    g_state.clear();
    LV2_State_Make_Path mk{nullptr, mk_path};
    LV2_State_Map_Path mp{nullptr, abs_to_abstract, abstract_to_abs};
    LV2_State_Free_Path fp{nullptr, fr_path};
    LV2_Feature f1{LV2_STATE__makePath, &mk}, f2{LV2_STATE__mapPath, &mp}, f3{LV2_STATE__freePath, &fp};
    const LV2_Feature* fs[] = {&f1, &f2, &f3, nullptr};
    std::atomic<bool> done{false};
    std::thread th([&] {
      save(pl, st_store, nullptr, 0, fs);
      done = true;
    });
    while (!done) block();
    th.join();
    st = g_state;
  }
  void restore_state(std::vector<KV>& st) {
    LV2_State_Map_Path mp{nullptr, abs_to_abstract, abstract_to_abs};
    LV2_State_Free_Path fp{nullptr, fr_path};
    LV2_Feature f2{LV2_STATE__mapPath, &mp}, f3{LV2_STATE__freePath, &fp};
    const LV2_Feature* fs[] = {&f2, &f3, nullptr};
    restore(pl, st_retrieve, &st, 0, fs);
  }
};

static bool exists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}
static std::string slurp(const std::string& p) {
  FILE* f = std::fopen(p.c_str(), "rb");
  std::string s;
  if (!f) return s;
  char b[4096];
  size_t n;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  std::fclose(f);
  return s;
}
static void touch_old(const std::string& p) {
  struct utimbuf ut{1000000000, 1000000000};
  utime(p.c_str(), &ut);
}

static void test_sessions() {
  init_params();
  mkdir("tests/out", 0755);
  g_bundle = "tests/out/Eka_Tape.presets.lv2";
  mkdir(g_bundle.c_str(), 0755);
  {  // папка пресетов с прошлого запуска: удалить свои папки
    DIR* d = opendir(g_bundle.c_str());
    std::vector<std::string> old;
    while (dirent* e = d ? readdir(d) : nullptr)
      if (e->d_name[0] != '.') old.push_back(e->d_name);
    if (d) closedir(d);
    for (auto& n : old) {
      remove_session_folder(g_bundle + "/" + n);
      unlink((g_bundle + "/" + n + "/notes.txt").c_str());
      rmdir((g_bundle + "/" + n).c_str());
      unlink((g_bundle + "/" + n).c_str());
    }
  }
  Host a;
  // запись: 1 такт в Quantize, громкость дорожки 1 — −7 дБ
  set_param(a.pl, PR_LEN1, 1);
  on_param(a.pl, PR_LEN1);
  set_param(a.pl, PR_LEV0, -7.0);
  on_param(a.pl, PR_LEV0);
  a.blocks(4);
  do_action(a.pl, A_REC);
  a.blocks(3 * 96000 / 128, 0.4f);  // взвелась, записала такт, играет
  CHECK(a.pl->looper->view(0).state == 3);

  // пресет без makePath (другой хост) — только настройки
  {
    g_state.clear();
    std::atomic<bool> done{false};
    std::thread th([&] {
      save(a.pl, st_store, nullptr, 0, nullptr);
      done = true;
    });
    while (!done) a.block();
    th.join();
    CHECK(!st_find(g_state, a.pl->u.sess_text) && st_find(g_state, a.pl->u.param[PR_LEV0]));
  }

  std::vector<KV> st;
  a.save_state(st);
  const KV* tx = st_find(st, a.pl->u.sess_text);
  const KV* dv = st_find(st, a.pl->u.sess_dir);
  CHECK(tx && dv && st_find(st, a.pl->u.param[PR_LEV0]));
  CHECK(a.pl->looper->session_status() == SS_SAVED);
  std::string folder = dv ? std::string(dv->data.data()) : "";
  CHECK(is_session_folder(folder.c_str()));  // путь — относительно папки пресетов
  const std::string dir = g_bundle + "/" + folder;
  CHECK(exists(dir + "/part1 track1.wav"));
  const std::string info = slurp(dir + "/info.txt");
  CHECK(info.find("Eka Tape session") != std::string::npos && info.find("part1 track1.wav") != std::string::npos);
  CHECK(info.find("Preset: (not known yet)") != std::string::npos);
  CHECK(a.pl->looper->session_id() != 0);

  // Zynthian сразу загружает сохранённое: та же сессия — не перегружается
  const int32_t sum = a.pl->looper->track_sum(0);
  a.restore_state(st);
  a.blocks(40);
  CHECK(a.pl->looper->track_sum(0) == sum && a.pl->looper->view(0).state == 3);

  // в другой экземпляр: настройки — не в restore(), а в run(); звук — в фоне
  Host b;
  b.blocks(4);
  b.restore_state(st);
  CHECK(b.pl->looper->mix(0).level_db == 0.0f);  // ещё не применено
  b.blocks(2);
  CHECK(b.pl->looper->mix(0).level_db == -7.0f);
  CHECK(b.pl->looper->session_status() == SS_LOADED);
  CHECK(b.pl->looper->track_len(0) == 96000 && b.pl->looper->session_id() == a.pl->looper->session_id());
  b.blocks(2 * 96000 / 128);  // вступила на такте
  CHECK(!b.pl->looper->view(0).stopped && b.peak_out > 0.1f);
  CHECK(b.ctl[O_SESS] == (float)SS_LOADED);

  // пресет только с настройками: лупы остаются
  {
    std::vector<KV> only;
    for (auto& kv : st)
      if (kv.key != a.pl->u.sess_text && kv.key != a.pl->u.sess_dir) only.push_back(kv);
    for (auto& kv : only)
      if (kv.key == a.pl->u.param[PR_LEV0]) {
        const float v = -3.0f;
        kv.data.assign((const char*)&v, (const char*)&v + sizeof v);
      }
    b.restore_state(only);
    b.blocks(4);
    CHECK(b.pl->looper->mix(0).level_db == -3.0f && b.pl->looper->track_len(0) == 96000);
  }

  // сессию нельзя загрузить (папки нет): не меняется ничего, причина — в статусе
  {
    std::vector<KV> bad = st;
    for (auto& kv : bad)
      if (kv.key == a.pl->u.sess_dir) {
        const char* p = "tape_1999-01-01_00-00-00";
        kv.data.assign(p, p + std::strlen(p) + 1);
      }
    b.restore_state(bad);
    b.blocks(4);
    CHECK(b.pl->looper->session_status() == SS_E_MISSING);
    CHECK(b.pl->looper->mix(0).level_db == -3.0f && b.pl->looper->track_len(0) == 96000);
  }

  // уборка: папка без пресета (старая) — удаляется; с пресетом — остаётся и узнаёт его имя;
  // с чужим файлом — свои файлы удалены, папка и чужое остаются
  {
    const std::string orphan = g_bundle + "/tape_2000-01-01_00-00-00";
    const std::string kept = g_bundle + "/tape_2000-01-01_00-00-01";
    const std::string foreign = g_bundle + "/tape_2000-01-01_00-00-02";
    for (auto* p : {&orphan, &kept, &foreign}) {
      mkdir(p->c_str(), 0755);
      FILE* f = std::fopen((*p + "/info.txt").c_str(), "wb");
      std::fputs("Eka Tape session\nPreset: (not known yet)\n", f);
      std::fclose(f);
      f = std::fopen((*p + "/part1 track1.wav").c_str(), "wb");
      std::fclose(f);
      touch_old(*p);
    }
    FILE* f = std::fopen((foreign + "/notes.txt").c_str(), "wb");
    std::fclose(f);
    touch_old(foreign);
    f = std::fopen((g_bundle + "/Keep.ttl").c_str(), "wb");
    std::fputs("<> rdfs:label \"Keep me\" ;\n  <x#session_dir> <tape_2000-01-01_00-00-01> .\n", f);
    std::fclose(f);
    std::vector<KV> st2;
    a.save_state(st2);
    CHECK(!exists(orphan));
    CHECK(exists(kept + "/part1 track1.wav"));
    CHECK(slurp(kept + "/info.txt").find("Preset: Keep me") != std::string::npos);
    CHECK(exists(foreign + "/notes.txt") && !exists(foreign + "/part1 track1.wav"));
    CHECK(exists(dir));  // первая сессия моложе 10 минут — не трогаем
    const KV* dv2 = st_find(st2, a.pl->u.sess_dir);
    CHECK(dv2 && std::string(dv2->data.data()) != folder);  // своя папка у каждого сохранения
    unlink((g_bundle + "/Keep.ttl").c_str());
  }

  // пустой лупер: пресет без звука (при загрузке лупы не трогает)
  {
    Host e;
    e.blocks(4);
    std::vector<KV> st3;
    e.save_state(st3);
    CHECK(!st_find(st3, e.pl->u.sess_text) && st_find(st3, e.pl->u.param[PR_GRID]));
  }
}

int main() {
  Plugin::Catch c;
  // первое значение только запоминается
  CHECK(!catch_knob(c, 0.0f, 0, 0.0f, 0.5f));
  // дорожка 1 на 0 дБ: педаль проходит через 0 — сразу ведёт
  CHECK(catch_knob(c, -1.0f, 0, 0.0f, 0.5f) && c.applied == -1.0f);
  CHECK(catch_knob(c, -20.0f, 0, -1.0f, 0.5f) && c.applied == -20.0f);
  // переключились на дорожку 2 (0 дБ): педаль на −20, двигаем к −25 и −10 — не захватывает
  CHECK(!catch_knob(c, -25.0f, 1, 0.0f, 0.5f));
  CHECK(!catch_knob(c, -10.0f, 1, 0.0f, 0.5f));
  // дошли до уровня дорожки (перешли через 0) — захватила и ведёт
  CHECK(catch_knob(c, 0.3f, 1, 0.0f, 0.5f) && c.applied == 0.3f);
  CHECK(catch_knob(c, -3.0f, 1, 0.3f, 0.5f) && c.applied == -3.0f);
  // уровень изменили не педалью (виджет: −12) — педаль снова ждёт захвата
  CHECK(!catch_knob(c, -2.0f, 1, -12.0f, 0.5f));
  CHECK(catch_knob(c, -12.2f, 1, -12.0f, 0.5f));
  // шкала ручек Level и Master: 75 = 0 дБ, 100 = +6, 0 = −∞, около 0 дБ мелко
  CHECK(std::fabs(pos_to_db(75.0f)) < 1e-6f && std::fabs(pos_to_db(100.0f) - 6.0f) < 1e-5f);
  CHECK(pos_to_db(0.0f) == kMinDb);
  bool mono = true;
  for (int i = 1; i <= 127; ++i)
    if (pos_to_db(i * 100.0f / 127.0f) < pos_to_db((i - 1) * 100.0f / 127.0f)) mono = false;
  CHECK(mono);
  // шаг MIDI (1/127) около 0 дБ: снизу меньше 0,1 дБ, сверху около 0,2 дБ
  const float step = 100.0f / 127.0f;
  CHECK(pos_to_db(75.0f) - pos_to_db(75.0f - step) < 0.1f);
  CHECK(pos_to_db(75.0f + step) - pos_to_db(75.0f) < 0.25f);
  for (float db : {-40.0f, -12.0f, -3.0f, -0.5f, 0.0f, 3.0f})
    CHECK(std::fabs(pos_to_db(db_to_pos(db)) - db) < 1e-3f);
  // смена сетки пользователем: ядро в 6×6, микшер и скорости дорожек по умолчанию, Master остаётся
  {
    init_params();
    Plugin pl;
    pl.looper = new Looper();
    pl.looper->init(48000.0, (size_t)64 << 20);
    for (int i = 0; i < PR_COUNT; ++i) pl.vals[i] = kParams[i].def;
    pl.apply_all();
    CHECK(pl.looper->grid() == 4);
    set_param(&pl, PR_LEV0 + 5, -12.0);
    on_param(&pl, PR_LEV0 + 5);
    set_param(&pl, PR_SPD0 + 2, 0.5);
    on_param(&pl, PR_SPD0 + 2);
    set_param(&pl, PR_MASTER, -6.0);
    on_param(&pl, PR_MASTER);
    CHECK(pl.looper->mix(5).level_db == -12.0f && pl.looper->speed(2) == 0.5f);
    set_param(&pl, PR_GRID, 2);
    on_param(&pl, PR_GRID);
    CHECK(pl.looper->grid() == 6);
    CHECK(pl.vals[PR_LEV0 + 5] == 0.0f && pl.looper->mix(5).level_db == 0.0f && pl.dirty[PR_LEV0 + 5]);
    CHECK(pl.vals[PR_SPD0 + 2] == 1.0f && pl.looper->speed(2) == 1.0f);
    CHECK(pl.vals[PR_MASTER] == -6.0f && pl.looper->master_db() == -6.0f);
    // повторно та же сетка — микшер не трогается
    set_param(&pl, PR_LEV0 + 30, -3.0);
    on_param(&pl, PR_LEV0 + 30);
    set_param(&pl, PR_GRID, 2);
    on_param(&pl, PR_GRID);
    CHECK(pl.looper->mix(30).level_db == -3.0f);
    // снапшот (restore -> apply_all) с другой сеткой: сетка меняется, его микшер остаётся
    pl.vals[PR_GRID] = 0;
    pl.vals[PR_LEV0 + 1] = -9.0f;
    pl.apply_all();
    CHECK(pl.looper->grid() == 2 && pl.looper->mix(1).level_db == -9.0f);
    // команды виджета для дорожек 5 и 6 в 6×6
    pl.vals[PR_GRID] = 2;
    pl.apply_all();
    set_param(&pl, PR_TMUTE, 6);
    CHECK(pl.vals[PR_TMUTE] == 6.0f);
    on_param(&pl, PR_TMUTE);
    CHECK(pl.vals[PR_TMUTE] == 0.0f);
    // настройки этапа 6: задержка, пороги жестов
    set_param(&pl, PR_LAT, 25);
    on_param(&pl, PR_LAT);
    CHECK(pl.looper->settings().latency_ms == 25.0f);
    set_param(&pl, PR_LAT, 500);  // за пределом — до 100
    CHECK(pl.vals[PR_LAT] == 100.0f);
    set_param(&pl, PR_HOLD, 300);
    on_param(&pl, PR_HOLD);
    auto hold_after = [&](uint32_t b, uint32_t blocks) {  // удержание кнопки b: через сколько блоков Hold
      Button& btn = pl.buttons[b];
      btn.process(0.0f, 128);
      btn.process(0.0f, 128);
      for (uint32_t k = 1; k <= blocks; ++k)
        if (btn.process(1.0f, 128) == Gesture::Hold) return k;
      return 0u;
    };
    const uint32_t undo_b = P_UNDO - P_REC, reset_b = P_RESET - P_REC;
    CHECK(hold_after(undo_b, 1000) == 114);   // 300 мс = 112,5 блока по 128 (+ блок нажатия)
    CHECK(hold_after(reset_b, 1000) == 376);  // Reset — свой порог, 1 с
    set_param(&pl, PR_RHOLD, 2000);
    on_param(&pl, PR_RHOLD);
    pl.buttons[reset_b].process(0.0f, 128);
    CHECK(hold_after(reset_b, 1000) == 751);
    // пул: при загрузке 64 МБ (в тесте), другие настройки замену не запускают, pool_mb — запускает
    auto blocks = [&](int nb) {
      float in[128] = {}, ol[128], orr[128];
      TimeInfo ti;
      for (int b = 0; b < nb; ++b) {
        pl.looper->process(in, in, ol, orr, 128, ti);
        WorkMsg wm;
        while (pl.looper->take_work(wm)) {
          WorkResp wr;
          pl.looper->do_work(wm, wr);
          pl.looper->work_done(wr);
        }
      }
    };
    const size_t before = pl.looper->pool_bytes();
    set_param(&pl, PR_FADE_OUT, 300);
    on_param(&pl, PR_FADE_OUT);
    blocks(4);
    CHECK(!pl.looper->pool_changing() && pl.looper->pool_bytes() == before);
    set_param(&pl, PR_POOL, 300);
    on_param(&pl, PR_POOL);
    CHECK(pl.looper->pool_changing());
    blocks(8);
    CHECK(!pl.looper->pool_changing() && (pl.looper->pool_bytes() >> 20) >= 299 &&
          (pl.looper->pool_bytes() >> 20) <= 300);
    delete pl.looper;
  }
  test_sessions();
  std::printf("plugin: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
