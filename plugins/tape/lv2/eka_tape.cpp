// Eka Tape: тонкая LV2-обёртка над ядром из core/.
// Порты и контроллеры-действия (тап и удержание), темп из time:Position (запасной путь — MIDI-клок),
// настройки как LV2-параметры (patch:Set, сохраняются через state), undo/redo через LV2 worker.
// Сессии (этап 7а): пресет Zynthian хранит и звук — WAV в своей папке рядом с пресетом (state:makePath).
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include "../core/gestures.h"
#include "../core/looper.h"
#include "../core/session.h"
#include "../core/tempo.h"

#define AL_URI "http://dmtry3.github.io/zynthian-eka/tape"

namespace {

using namespace alp;

constexpr int kPoolMbDefault = 2048;  // пул при загрузке: 2 ГБ по ТЗ; настройка pool_mb меняет его на ходу

// Порядок совпадает с lv2:index в eka_tape.ttl.
// Входные контроллеры идут первыми (0..17): Zynthian нумерует LV2-параметры сразу после
// их количества, и порт с бо́льшим номером затёрся бы параметром (так пропадала страница Mix).
enum Port : uint32_t {
  // страницы 1–4: контроллеры-действия
  P_REC, P_UNDO, P_REDO, P_RESET,
  P_MUTE, P_REVERSE, P_LOCK, P_TRK_NEXT,
  P_TRK_PREV, P_PART_NEXT, P_PART_PREV, P_PLAY,
  P_W_LOOP, P_W_MIXER,
  // страница 5: плавные (уровень, панорама, master, скорость)
  P_LEVEL, P_PAN, P_MASTER, P_SPEED,
  P_IN_L, P_IN_R, P_OUT_L, P_OUT_R, P_EVENTS,
  // выходы только для чтения
  O_TRACK, O_PART, O_PEND, O_WIDGET, O_ACTION, O_ACT_CNT, O_HELD,
  O_BPM, O_BAR, O_BEAT, O_BPB, O_ROLLING, O_TSRC, O_CLK_BPM, O_POS_CNT,
  P_EVENTS_OUT,
  O_ST1, O_ST2, O_ST3, O_ST4,
  O_POS1, O_POS2, O_POS3, O_POS4,
  O_LEN1, O_LEN2, O_LEN3, O_LEN4,
  O_FLAGS, O_POOL, O_PARTS,
  O_PK1, O_PK2, O_PK3, O_PK4, O_MPK_L, O_MPK_R,
  O_SPD1, O_SPD2, O_SPD3, O_SPD4, O_ESPD1, O_ESPD2, O_ESPD3, O_ESPD4,
  // сетка до 6×6 (этап 7): дорожки 5 и 6, флаги дорожек масками, сама сетка
  O_ST5, O_ST6, O_POS5, O_POS6, O_LEN5, O_LEN6, O_PK5, O_PK6, O_SPD5, O_SPD6, O_ESPD5, O_ESPD6,
  O_MUTES, O_REVS, O_STOPS, O_LOCKS, O_GRID,
  O_POOL_MB,  // фактический размер пула, МБ (0 — меняется)
  O_SESS, O_SESS_PROG,  // сессия: состояние (SessStatus) и доля записанного или прочитанного
  O_COUNT,              // счёт перед записью на активной дорожке: долей до начала (0 — нет)
  O_PSTAT,              // сдвиг высоты: 0 работает, 1 выключен из-за перегрузки, 2 недоступен
  P_COUNT
};

// Выходы по месту дорожки в парте 1..6
const uint32_t kOutSt[kMaxSlots] = {O_ST1, O_ST2, O_ST3, O_ST4, O_ST5, O_ST6};
const uint32_t kOutPos[kMaxSlots] = {O_POS1, O_POS2, O_POS3, O_POS4, O_POS5, O_POS6};
const uint32_t kOutLen[kMaxSlots] = {O_LEN1, O_LEN2, O_LEN3, O_LEN4, O_LEN5, O_LEN6};
const uint32_t kOutPk[kMaxSlots] = {O_PK1, O_PK2, O_PK3, O_PK4, O_PK5, O_PK6};
const uint32_t kOutSpd[kMaxSlots] = {O_SPD1, O_SPD2, O_SPD3, O_SPD4, O_SPD5, O_SPD6};
const uint32_t kOutEspd[kMaxSlots] = {O_ESPD1, O_ESPD2, O_ESPD3, O_ESPD4, O_ESPD5, O_ESPD6};

// Параметр grid: 0 — 2×2, 1 — 4×4, 2 — 6×6
inline int grid_n(float v) { return v < 0.5f ? 2 : (v < 1.5f ? 4 : 6); }

constexpr uint32_t kButtons = P_W_MIXER - P_REC + 1;

// Коды действий в порту «Out: Action». Те же числа в виджете.
enum Action : int {
  A_NONE, A_REC, A_UNDO, A_REDO, A_RESET_TRACK, A_RESET_ALL, A_MUTE, A_REVERSE, A_LOCK,
  A_TRK_NEXT, A_TRK_PREV, A_PART_NEXT, A_PART_PREV, A_PLAY, A_W_LOOP, A_W_MIXER
};

// Какое действие даёт тап и какое удержание. Без удержания (A_NONE): срабатывает сразу по нажатию.
// Пороги удержания — настройки hold_ms (общий) и reset_ms (Reset).
// С удержанием тап срабатывает при отпускании, иначе их не различить.
struct ButtonDef {
  Action tap, hold;
};
const ButtonDef kDefs[kButtons] = {
    {A_REC, A_NONE},
    {A_UNDO, A_REDO},
    {A_REDO, A_NONE},
    {A_RESET_TRACK, A_RESET_ALL},
    {A_MUTE, A_NONE},
    {A_REVERSE, A_NONE},
    {A_LOCK, A_NONE},
    {A_TRK_NEXT, A_TRK_PREV},
    {A_TRK_PREV, A_NONE},
    {A_PART_NEXT, A_PART_PREV},
    {A_PART_PREV, A_NONE},
    {A_PLAY, A_NONE},
    {A_W_LOOP, A_NONE},
    {A_W_MIXER, A_NONE},
};

// Настройки и микшер: LV2-параметры (patch:Set), сохраняются в снапшотах. Порядок как в eka_tape.ttl.
enum Param {
  PR_GRID,  // первым: при загрузке снапшота сетка ставится раньше микшера
  PR_REC_ORDER, PR_MODE, PR_LEN1, PR_LEN6 = PR_LEN1 + kMaxSlots - 1, PR_FADE_OUT, PR_FADE_IN, PR_REV, PR_MUTE_Q,
  PR_PART_Q,
  // дорожка k = парт × n + место (n — сетка), всего до 36
  PR_LEV0,                      // lev1..lev36: уровень дорожки, дБ
  PR_PAN0 = PR_LEV0 + kTracks,  // pan1..pan36: −100..100
  PR_CHM0 = PR_PAN0 + kTracks,  // chm1..chm36: 0 стерео, 1 только L, 2 только R
  PR_MASTER = PR_CHM0 + kTracks,  // Dry убран (2026-11-08): вход идёт через активную дорожку
  PR_LIMIT,
  PR_TMUTE,  // команда виджета: mute дорожки 1..6 текущего парта; сам возвращается в 0
  PR_TGLIDE,                    // плавная смена темпа, мс
  PR_SSNAP,                     // привязка скорости: 0 свободно, 1 полутоны, 2 октавы
  PR_SGLIDE,                    // плавный ход скорости, мс
  PR_SPD0,                      // spd1..spd36: ручная скорость дорожки
  PR_SRESET = PR_SPD0 + kTracks,  // команда виджета: сброс скорости дорожки 1..6 текущего парта
  PR_HOLD,                      // порог удержания педали, мс
  PR_RHOLD,                     // порог удержания Reset, мс
  PR_LAT,                       // компенсация задержки, мс
  PR_POOL,                      // пул памяти, МБ
  // команды виджета (этап 7б, касание), сами возвращаются в 0:
  PR_TSEL,                      // активная дорожка 1..6 текущего парта
  PR_PSEL,                      // переход в парт 1..6 по PART AT (тот же или текущий, пока ждёт, — отмена)
  PR_WSEL,                      // экран: 1 Loop, 2 Mixer (как контроллеры «Виджет Loop/Mixer»)
  PR_COUNTIN,                   // счёт перед записью основы: 0 выкл, 1 или 2 такта (этап 8)
  PR_RTHRESH,                   // старт записи основы по порогу (только Free): 0 выкл, −60..−12 дБ
  PR_PIT0,                      // pit1..pit36: сдвиг высоты дорожки, полутонов (этап 8)
  PR_PRESET = PR_PIT0 + kTracks,  // команда виджета: сдвиг дорожки 1..6 текущего парта в 0
  PR_COUNT
};
struct ParamDef {
  char sym[16];
  float def, lo, hi;
  bool is_float;
};
ParamDef kParams[PR_COUNT];

void init_params() {
  struct S { const char* sym; int def, lo, hi; };
  const S settings[PR_LEV0] = {
      {"grid", 1, 0, 2}, {"rec_order", 0, 0, 1}, {"mode", 1, 0, 1}, {"len1", 0, 0, 16}, {"len2", 0, 0, 16},
      {"len3", 0, 0, 16}, {"len4", 0, 0, 16}, {"len5", 0, 0, 16}, {"len6", 0, 0, 16}, {"fade_out", 200, 10, 4000}, {"fade_in", 200, 10, 4000}, {"rev_time", 50, 5, 1000},
      {"mute_q", 2, 0, 3}, {"part_q", 3, 0, 3}};
  for (int i = 0; i < PR_LEV0; ++i) {
    kParams[i] = ParamDef{{}, (float)settings[i].def, (float)settings[i].lo, (float)settings[i].hi, false};
    std::snprintf(kParams[i].sym, sizeof kParams[i].sym, "%s", settings[i].sym);
  }
  for (int t = 0; t < kTracks; ++t) {
    kParams[PR_LEV0 + t] = ParamDef{{}, 0.0f, kMinDb, 6.0f, true};
    std::snprintf(kParams[PR_LEV0 + t].sym, 16, "lev%d", t + 1);
    kParams[PR_PAN0 + t] = ParamDef{{}, 0.0f, -100.0f, 100.0f, false};
    std::snprintf(kParams[PR_PAN0 + t].sym, 16, "pan%d", t + 1);
    kParams[PR_CHM0 + t] = ParamDef{{}, 0.0f, 0.0f, 2.0f, false};
    std::snprintf(kParams[PR_CHM0 + t].sym, 16, "chm%d", t + 1);
  }
  kParams[PR_MASTER] = ParamDef{"mst_db", 0.0f, kMinDb, 6.0f, true};  // не "master": так называется порт
  kParams[PR_LIMIT] = ParamDef{"limiter", 1.0f, 0.0f, 1.0f, false};
  kParams[PR_TMUTE] = ParamDef{"tmute", 0.0f, 0.0f, 6.0f, false};
  kParams[PR_TGLIDE] = ParamDef{"tempo_glide", 500.0f, 100.0f, 4000.0f, false};
  kParams[PR_SSNAP] = ParamDef{"spd_snap", 0.0f, 0.0f, 2.0f, false};
  kParams[PR_SGLIDE] = ParamDef{"spd_glide", 150.0f, 0.0f, 2000.0f, false};
  for (int t = 0; t < kTracks; ++t) {
    kParams[PR_SPD0 + t] = ParamDef{{}, 1.0f, kMinSpeed, kMaxSpeed, true};
    std::snprintf(kParams[PR_SPD0 + t].sym, 16, "spd%d", t + 1);
  }
  kParams[PR_SRESET] = ParamDef{"sreset", 0.0f, 0.0f, 6.0f, false};
  kParams[PR_HOLD] = ParamDef{"hold_ms", 500.0f, 200.0f, 2000.0f, false};
  kParams[PR_RHOLD] = ParamDef{"reset_ms", 1000.0f, 500.0f, 3000.0f, false};
  kParams[PR_LAT] = ParamDef{"latency", 0.0f, 0.0f, 100.0f, false};
  kParams[PR_POOL] = ParamDef{"pool_mb", (float)kPoolMbDefault, 256.0f, 4096.0f, false};
  kParams[PR_TSEL] = ParamDef{"tsel", 0.0f, 0.0f, 6.0f, false};
  kParams[PR_PSEL] = ParamDef{"psel", 0.0f, 0.0f, 6.0f, false};
  kParams[PR_WSEL] = ParamDef{"wsel", 0.0f, 0.0f, 2.0f, false};
  kParams[PR_COUNTIN] = ParamDef{"count_in", 0.0f, 0.0f, 2.0f, false};
  kParams[PR_RTHRESH] = ParamDef{"rec_thresh", 0.0f, -60.0f, 0.0f, false};
  for (int t = 0; t < kTracks; ++t) {
    kParams[PR_PIT0 + t] = ParamDef{{}, 0.0f, -kMaxPitch, kMaxPitch, true};
    std::snprintf(kParams[PR_PIT0 + t].sym, 16, "pit%d", t + 1);
  }
  kParams[PR_PRESET] = ParamDef{"pit_reset", 0.0f, 0.0f, 6.0f, false};
}
constexpr uint32_t kReportPerRun = 4;  // ответов на patch:Get за один run()

enum TempoSrc { SRC_NONE = 0, SRC_POSITION = 1, SRC_CLOCK = 2 };

struct Uris {
  LV2_URID atom_Object, atom_Blank, atom_Float, atom_Double, atom_Int, atom_Long, atom_URID, atom_Sequence;
  LV2_URID midi_Event;
  LV2_URID time_Position, time_bar, time_barBeat, time_bpm, time_bpb, time_speed, time_frame;
  LV2_URID patch_Set, patch_Get, patch_property, patch_value;
  LV2_URID atom_String, atom_Path, sess_text, sess_dir, sess_id;
  LV2_URID param[PR_COUNT];
};

// Почтовый ящик настроек: restore() идёт в своём потоке, одновременно с run() (state:threadSafeRestore),
// поэтому значения не пишутся в ядро, а передаются в run() без блокировок (seqlock).
struct PendingParams {
  std::mutex mx;                  // писатели — только не-RT потоки
  std::atomic<uint32_t> seq{0};   // нечётное — пишется
  std::atomic<float> val[PR_COUNT];
  std::atomic<bool> has[PR_COUNT];
  uint32_t applied = 0;           // RT: последний применённый seq
};

// Сохранение сессии в фоне (своим потоком): Zynthian ждёт ответа save() не дольше 2 с, а педали
// и кнопки доходят до плагина через тот же поток jalv, что вызывает save(). Поэтому save() только
// создаёт папку и ставит задание, а ждать закрытия записи и писать WAV — здесь.
struct SaveJob {
  std::string dir, bundle, name;  // папка сессии, папка пресетов, имя папки
  uint64_t id = 0;
};
struct Saver {
  std::mutex mx;
  std::condition_variable cv;
  std::deque<SaveJob> q;
  uint64_t cur = 0;               // id задания, которое сейчас пишется (0 — никакого)
  std::atomic<bool> stop{false};
  std::thread th;
};

struct Plugin {
  void* ports[P_COUNT] = {};
  double sr = 48000.0;
  Uris u = {};
  LV2_Atom_Forge forge;
  LV2_Worker_Schedule* schedule = nullptr;
  Looper* looper = nullptr;
  PeakShm* peaks = nullptr;  // общая память с виджетом: /dev/shm/eka_tape_<pid>
  char peaks_path[64] = {};
  Button buttons[kButtons];
  TempoClock clock;
  MidiClock midi_clock;
  uint64_t now = 0;  // сэмплов от старта плагина
  int widget = 0;
  int pool_req_mb = kPoolMbDefault;  // последний запрошенный размер пула
  int last_action = A_NONE;
  uint32_t action_count = 0;
  float vals[PR_COUNT];
  bool dirty[PR_COUNT] = {};  // ядро изменило значение само: сообщить хосту
  int report_next = -1;  // следующий параметр для ответа на patch:Get, -1 = нечего
  PendingParams pend;
  Saver saver;
  // ручки страницы 5 (Level, Pan, Master) с захватом (catch): ведут значение,
  // только когда прошли через его текущую величину
  struct Catch {
    bool init = false, caught = false;
    float last = 0.0f, applied = 0.0f;
    int target = -1;
  } c_level, c_pan, c_master, c_speed;

  float in(uint32_t p) const { return ports[p] ? *static_cast<const float*>(ports[p]) : 0.0f; }
  void out(uint32_t p, float v) {
    if (ports[p]) *static_cast<float*>(ports[p]) = v;
  }
  bool clock_is_source() const { return clock.messages() == 0 && midi_clock.active(now); }

  void apply_settings() {
    Settings s;
    s.rop = vals[PR_REC_ORDER] == 1;
    s.quantize = vals[PR_MODE] == 1;
    for (int i = 0; i < kMaxSlots; ++i) s.length[i] = vals[PR_LEN1 + i];
    s.fade_out_ms = (float)vals[PR_FADE_OUT];
    s.fade_in_ms = (float)vals[PR_FADE_IN];
    s.reverse_ms = (float)vals[PR_REV];
    s.mute_q = (int)vals[PR_MUTE_Q];
    s.part_q = (int)vals[PR_PART_Q];
    s.tempo_glide_ms = vals[PR_TGLIDE];
    s.spd_snap = (int)vals[PR_SSNAP];
    s.spd_glide_ms = vals[PR_SGLIDE];
    s.latency_ms = vals[PR_LAT];
    s.count_in = (int)vals[PR_COUNTIN];
    s.rec_thresh_db = (int)vals[PR_RTHRESH];
    looper->set_settings(s);
    configure_buttons();
    // пул — только когда изменился сам параметр: смена стирает лупы
    // (если при загрузке дали меньше запрошенного, другие настройки не должны запускать замену)
    if ((int)vals[PR_POOL] != pool_req_mb) {
      pool_req_mb = (int)vals[PR_POOL];
      looper->set_pool_size((size_t)pool_req_mb << 20);
    }
    looper->set_grid(grid_n(vals[PR_GRID]));  // та же сетка — ничего не делает
  }
  // Пороги удержания из настроек: Reset — свой, остальные кнопки с удержанием — общий.
  void configure_buttons() {
    for (uint32_t i = 0; i < kButtons; ++i) {
      const ButtonDef& d = kDefs[i];
      const double ms = d.hold == A_NONE ? 0.0 : (d.tap == A_RESET_TRACK ? vals[PR_RHOLD] : vals[PR_HOLD]);
      buttons[i].configure(ms > 0, (uint32_t)(ms * sr / 1000.0));
    }
  }
  // Пользователь сменил сетку: номера дорожек теперь значат другое — микшер и скорости по умолчанию
  // (при загрузке снапшота так не делаем: его микшер относится к его сетке).
  void reset_track_params() {
    for (int i = PR_LEV0; i < PR_COUNT; ++i) {
      const bool per_track =
          i < PR_MASTER || (i >= PR_SPD0 && i < PR_SPD0 + kTracks) || (i >= PR_PIT0 && i < PR_PIT0 + kTracks);
      if (!per_track) continue;  // Master, ограничитель, настройки — общие
      if (vals[i] != kParams[i].def) {
        vals[i] = kParams[i].def;
        dirty[i] = true;
      }
      apply_mix_param(i);
    }
  }

  // параметр микшера -> ядро
  void apply_mix_param(int i) {
    if (i >= PR_LEV0 && i < PR_MASTER) {
      const int t = (i - PR_LEV0) % kTracks;
      Mix m;
      m.level_db = vals[PR_LEV0 + t];
      m.pan = vals[PR_PAN0 + t] / 100.0f;
      m.mode = (int)vals[PR_CHM0 + t];
      looper->set_mix(t, m);
    } else if (i == PR_MASTER) {
      looper->set_master_db(vals[PR_MASTER]);
    } else if (i == PR_LIMIT) {
      looper->set_limiter(vals[PR_LIMIT] >= 0.5f);
    } else if (i >= PR_SPD0 && i < PR_SPD0 + kTracks) {
      looper->set_speed(i - PR_SPD0, vals[i]);
    } else if (i >= PR_PIT0 && i < PR_PIT0 + kTracks) {
      looper->set_pitch(i - PR_PIT0, vals[i]);
    }
  }
  static bool is_setting(int i) {
    return i < PR_LEV0 || i == PR_TGLIDE || i == PR_SSNAP || i == PR_SGLIDE || (i >= PR_HOLD && i <= PR_POOL) || i == PR_COUNTIN || i == PR_RTHRESH;
  }
  void apply_all() {
    apply_settings();
    for (int i = PR_LEV0; i < PR_COUNT; ++i) apply_mix_param(i);
  }
  // ядро -> параметры: что изменилось само (сброс, Lock-копия, ручки страницы 5), пометить для хоста
  void pull_mix() {
    auto upd = [this](int i, float v) {
      if (std::fabs(vals[i] - v) > 1e-4f) {
        vals[i] = v;
        dirty[i] = true;
      }
    };
    for (int t = 0; t < kTracks; ++t) {
      const Mix& m = looper->mix(t);
      upd(PR_LEV0 + t, m.level_db);
      upd(PR_PAN0 + t, std::round(m.pan * 100.0f));
      upd(PR_CHM0 + t, (float)m.mode);
    }
    upd(PR_MASTER, looper->master_db());
    upd(PR_LIMIT, looper->limiter() ? 1.0f : 0.0f);
    for (int t = 0; t < kTracks; ++t) upd(PR_SPD0 + t, looper->speed(t));
    for (int t = 0; t < kTracks; ++t) upd(PR_PIT0 + t, looper->pitch(t));  // новая основа — с 0
  }
};

double atom_number(const Plugin* pl, const LV2_Atom* a, double def) {
  if (!a) return def;
  if (a->type == pl->u.atom_Float) return reinterpret_cast<const LV2_Atom_Float*>(a)->body;
  if (a->type == pl->u.atom_Double) return reinterpret_cast<const LV2_Atom_Double*>(a)->body;
  if (a->type == pl->u.atom_Int) return reinterpret_cast<const LV2_Atom_Int*>(a)->body;
  if (a->type == pl->u.atom_Long) return (double)reinterpret_cast<const LV2_Atom_Long*>(a)->body;
  return def;
}

int find_param(const Plugin* pl, LV2_URID key) {
  for (int i = 0; i < PR_COUNT; ++i)
    if (pl->u.param[i] == key) return i;
  return -1;
}

void set_param(Plugin* pl, int i, double v) {
  float x = kParams[i].is_float ? (float)v : (float)std::lround(v);
  if (x < kParams[i].lo) x = kParams[i].lo;
  if (x > kParams[i].hi) x = kParams[i].hi;
  pl->vals[i] = x;
}

// Не RT (restore): положить значения в почтовый ящик, run() применит их в начале блока.
void post_params(Plugin* pl, const float* val, const bool* has) {
  PendingParams& p = pl->pend;
  std::lock_guard<std::mutex> lk(p.mx);
  const uint32_t s = p.seq.load(std::memory_order_relaxed);
  p.seq.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (int i = 0; i < PR_COUNT; ++i) {
    p.val[i].store(val[i], std::memory_order_relaxed);
    p.has[i].store(has[i], std::memory_order_relaxed);
  }
  p.seq.store(s + 2, std::memory_order_release);
}

// RT, начало run(): новые значения из restore() — в параметры и ядро. Писатель как раз пишет
// (или переписал во время чтения) — попробуем в следующем блоке.
void apply_pending(Plugin* pl) {
  PendingParams& p = pl->pend;
  const uint32_t s1 = p.seq.load(std::memory_order_acquire);
  if ((s1 & 1) || s1 == p.applied) return;
  float val[PR_COUNT];
  bool has[PR_COUNT];
  for (int i = 0; i < PR_COUNT; ++i) {
    val[i] = p.val[i].load(std::memory_order_relaxed);
    has[i] = p.has[i].load(std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  if (p.seq.load(std::memory_order_relaxed) != s1) return;
  p.applied = s1;
  for (int i = 0; i < PR_COUNT; ++i)
    if (has[i]) set_param(pl, i, val[i]);
  pl->apply_all();
  pl->report_next = 0;  // сообщить хосту и виджету новые значения
}

// Ручки Level и Master: положение 0..100 со шкалой фейдера (как на экране Mixer):
// 75 = 0 дБ, выше — до +6 дБ, ниже — плавно к −∞; около 0 дБ шаг очень мелкий.
float pos_to_db(float pos) {
  const float f = std::fmin(1.0f, std::fmax(0.0f, pos / 100.0f));
  if (f >= 0.75f) return (f - 0.75f) / 0.25f * 6.0f;
  if (f <= 0.01f) return kMinDb;
  const float k = 1.0f - f / 0.75f;
  return kMinDb * k * k;
}
float db_to_pos(float db) {
  if (db <= kMinDb) return 0.0f;
  if (db >= 0.0f) return 100.0f * (0.75f + 0.25f * std::fmin(db, 6.0f) / 6.0f);
  return 100.0f * 0.75f * (1.0f - std::sqrt(db / kMinDb));
}

// Ручка страницы 5 с захватом: cur — текущее значение цели, tol — допуск «прошли через него».
// Возвращает true и новое значение, когда ручка ведёт цель.
bool catch_knob(Plugin::Catch& c, float v, int target, float cur, float tol) {
  if (!c.init) {
    c.init = true;
    c.last = v;
    return false;
  }
  if (std::fabs(v - c.last) < 1e-4f) return false;
  if (target != c.target || (c.caught && std::fabs(cur - c.applied) > tol)) {
    c.target = target;  // другая дорожка или значение изменили не ручкой: захватить заново
    c.caught = false;
  }
  if (!c.caught && std::fmin(c.last, v) <= cur + tol && std::fmax(c.last, v) >= cur - tol) c.caught = true;
  c.last = v;
  if (!c.caught) return false;
  c.applied = v;
  return true;
}

void handle_knobs(Plugin* pl) {
  Looper* lp = pl->looper;
  const int k = lp->active_index();
  Mix m = lp->mix(k);
  if (catch_knob(pl->c_level, pl->in(P_LEVEL), k, db_to_pos(m.level_db), 1.0f)) {
    m.level_db = pos_to_db(pl->c_level.applied);
    lp->set_mix(k, m);
  }
  if (catch_knob(pl->c_pan, pl->in(P_PAN), k, m.pan * 100.0f, 2.0f)) {
    m.pan = pl->c_pan.applied / 100.0f;
    lp->set_mix(k, m);
  }
  // скорость: захват в октавах (ручка логарифмическая 0,25..2)
  const float sp = std::fmax(kMinSpeed, pl->in(P_SPEED));
  if (catch_knob(pl->c_speed, std::log2(sp), k, std::log2(lp->speed(k)), 0.03f))
    lp->set_speed(k, std::exp2(pl->c_speed.applied));
  if (catch_knob(pl->c_master, pl->in(P_MASTER), 0, db_to_pos(lp->master_db()), 1.0f))
    lp->set_master_db(pos_to_db(pl->c_master.applied));
}

void handle_position(Plugin* pl, const LV2_Atom_Object* obj) {
  const LV2_Atom *bpm = nullptr, *bpb = nullptr, *speed = nullptr, *bar = nullptr, *bb = nullptr, *fr = nullptr;
  lv2_atom_object_get(obj, pl->u.time_bpm, &bpm, pl->u.time_bpb, &bpb, pl->u.time_speed, &speed,
                      pl->u.time_bar, &bar, pl->u.time_barBeat, &bb, pl->u.time_frame, &fr, 0);
  PositionMsg m;
  if (fr) { m.has_frame = true; m.frame = (int64_t)atom_number(pl, fr, 0); }
  if (bpm) { m.has_bpm = true; m.bpm = atom_number(pl, bpm, -1); }
  if (bpb) { m.has_bpb = true; m.bpb = atom_number(pl, bpb, -1); }
  if (speed) { m.has_speed = true; m.speed = atom_number(pl, speed, 0); }
  if (bar && bb) {
    m.has_pos = true;
    m.bar = atom_number(pl, bar, -1);
    m.bar_beat = atom_number(pl, bb, -1);
  }
  pl->clock.on_position(m);
}

void on_param(Plugin* pl, int i);

void handle_patch_set(Plugin* pl, const LV2_Atom_Object* obj) {
  const LV2_Atom *prop = nullptr, *val = nullptr;
  lv2_atom_object_get(obj, pl->u.patch_property, &prop, pl->u.patch_value, &val, 0);
  if (!prop || prop->type != pl->u.atom_URID || !val) return;
  int i = find_param(pl, reinterpret_cast<const LV2_Atom_URID*>(prop)->body);
  if (i < 0) return;
  set_param(pl, i, atom_number(pl, val, pl->vals[i]));
  on_param(pl, i);
}

// Параметр i получил новое значение от хоста или виджета: применить.
void on_param(Plugin* pl, int i) {
  if (i == PR_TMUTE) {
    if (pl->vals[i] >= 1.0f) pl->looper->mute_slot((int)pl->vals[i] - 1);
    pl->vals[i] = 0.0f;
    pl->dirty[i] = true;
    return;
  }
  if (i == PR_TSEL || i == PR_PSEL || i == PR_WSEL) {
    const int v = (int)pl->vals[i];
    if (v >= 1) {
      if (i == PR_TSEL) pl->looper->select_slot(v - 1);
      else if (i == PR_PSEL) pl->looper->select_part(v - 1);
      else pl->widget = v == 2 ? 1 : 0;
    }
    pl->vals[i] = 0.0f;
    pl->dirty[i] = true;
    return;
  }
  if (i == PR_SRESET) {
    if (pl->vals[i] >= 1.0f && pl->vals[i] <= pl->looper->grid())
      pl->looper->speed_reset(pl->looper->part() * pl->looper->grid() + (int)pl->vals[i] - 1);
    pl->vals[i] = 0.0f;
    pl->dirty[i] = true;
    return;
  }
  if (i == PR_PRESET) {
    if (pl->vals[i] >= 1.0f && pl->vals[i] <= pl->looper->grid())
      pl->looper->set_pitch(pl->looper->part() * pl->looper->grid() + (int)pl->vals[i] - 1, 0.0f);
    pl->vals[i] = 0.0f;
    pl->dirty[i] = true;
    return;
  }
  if (i == PR_GRID && grid_n(pl->vals[i]) != pl->looper->grid()) pl->reset_track_params();
  if (Plugin::is_setting(i)) pl->apply_settings();
  else pl->apply_mix_param(i);
}

// Запасной путь: MIDI-клок. Используется, только пока time:Position не приходил ни разу.
void handle_midi(Plugin* pl, const uint8_t* b, uint32_t size, uint64_t t) {
  if (size < 1) return;
  switch (b[0]) {
    case 0xF8: pl->midi_clock.on_tick(t); break;
    case 0xFA: pl->midi_clock.on_start(); break;
    case 0xFB: pl->midi_clock.on_continue(); break;
    case 0xFC: pl->midi_clock.on_stop(); break;
    default: return;
  }
  if (!pl->clock_is_source()) return;
  PositionMsg m;
  m.has_speed = true;
  m.speed = pl->midi_clock.running() ? 1.0 : 0.0;
  double bpm = pl->midi_clock.bpm();
  if (bpm > 0) { m.has_bpm = true; m.bpm = bpm; }
  if (b[0] == 0xFA) m.has_pos = true;  // старт: такт 0, доля 0
  pl->clock.apply_external(m);
}

void do_action(Plugin* pl, Action a) {
  Looper* lp = pl->looper;
  switch (a) {
    case A_NONE: return;
    case A_REC: lp->command(Cmd::Rec); break;
    case A_UNDO: lp->command(Cmd::Undo); break;
    case A_REDO: lp->command(Cmd::Redo); break;
    case A_RESET_TRACK: lp->command(Cmd::ResetTrack); break;
    case A_RESET_ALL: lp->command(Cmd::ResetAll); break;
    case A_MUTE: lp->command(Cmd::Mute); break;
    case A_REVERSE: lp->command(Cmd::Reverse); break;
    case A_TRK_NEXT: lp->command(Cmd::TrackNext); break;
    case A_TRK_PREV: lp->command(Cmd::TrackPrev); break;
    case A_PLAY: lp->command(Cmd::PlayStop); break;
    case A_LOCK: lp->command(Cmd::Lock); break;
    case A_PART_NEXT: lp->command(Cmd::PartNext); break;
    case A_PART_PREV: lp->command(Cmd::PartPrev); break;
    case A_W_LOOP: pl->widget = 0; break;
    case A_W_MIXER: pl->widget = 1; break;
    default: break;
  }
  pl->last_action = a;
  ++pl->action_count;
}

// После patch:Get: сообщаем хосту значения параметров (по несколько за run).
void report_params(Plugin* pl) {
  auto* seq = static_cast<LV2_Atom_Sequence*>(pl->ports[P_EVENTS_OUT]);
  if (!seq) return;
  const uint32_t cap = seq->atom.size;
  lv2_atom_forge_set_buffer(&pl->forge, reinterpret_cast<uint8_t*>(seq), cap);
  LV2_Atom_Forge_Frame frame;
  lv2_atom_forge_sequence_head(&pl->forge, &frame, 0);
  auto send = [pl](int i) {
    LV2_Atom_Forge_Frame of;
    lv2_atom_forge_frame_time(&pl->forge, 0);
    lv2_atom_forge_object(&pl->forge, &of, 0, pl->u.patch_Set);
    lv2_atom_forge_key(&pl->forge, pl->u.patch_property);
    lv2_atom_forge_urid(&pl->forge, pl->u.param[i]);
    lv2_atom_forge_key(&pl->forge, pl->u.patch_value);
    if (kParams[i].is_float) lv2_atom_forge_float(&pl->forge, pl->vals[i]);
    else lv2_atom_forge_int(&pl->forge, (int32_t)pl->vals[i]);
    lv2_atom_forge_pop(&pl->forge, &of);
  };
  uint32_t sent = 0;
  // сначала то, что ядро изменило само, потом ответ на patch:Get
  for (int i = 0; i < PR_COUNT && sent < kReportPerRun * 2; ++i)
    if (pl->dirty[i]) {
      pl->dirty[i] = false;
      send(i);
      ++sent;
    }
  for (; sent < kReportPerRun * 2 && pl->report_next >= 0; ++sent) {
    send(pl->report_next);
    pl->report_next = pl->report_next + 1 < PR_COUNT ? pl->report_next + 1 : -1;
  }
  lv2_atom_forge_pop(&pl->forge, &frame);
}

// Плагин с пулом pool_mb (instantiate — 2 ГБ по ТЗ; тесты — меньше).
void save_worker(Plugin* pl);  // поток сохранения сессий (раздел state ниже)
void stop_saver(Plugin* pl);

Plugin* create_plugin(double rate, LV2_URID_Map* map, LV2_Worker_Schedule* schedule, int pool_mb) {
  Plugin* pl = new (std::nothrow) Plugin();
  if (!pl) return nullptr;
  pl->looper = new (std::nothrow) Looper();
  if (!pl->looper || !pl->looper->init(rate, (size_t)pool_mb << 20)) {
    delete pl->looper;
    delete pl;
    return nullptr;
  }
  pl->sr = rate;
  pl->schedule = schedule;
  Uris& u = pl->u;
  auto M = [map](const char* uri) { return map->map(map->handle, uri); };
  u.atom_Object = M(LV2_ATOM__Object);
  u.atom_Blank = M(LV2_ATOM__Blank);
  u.atom_Float = M(LV2_ATOM__Float);
  u.atom_Double = M(LV2_ATOM__Double);
  u.atom_Int = M(LV2_ATOM__Int);
  u.atom_Long = M(LV2_ATOM__Long);
  u.atom_URID = M(LV2_ATOM__URID);
  u.atom_Sequence = M(LV2_ATOM__Sequence);
  u.midi_Event = M(LV2_MIDI__MidiEvent);
  u.time_Position = M(LV2_TIME__Position);
  u.time_bar = M(LV2_TIME__bar);
  u.time_barBeat = M(LV2_TIME__barBeat);
  u.time_bpm = M(LV2_TIME__beatsPerMinute);
  u.time_bpb = M(LV2_TIME__beatsPerBar);
  u.time_speed = M(LV2_TIME__speed);
  u.time_frame = M(LV2_TIME__frame);
  u.patch_Set = M(LV2_PATCH__Set);
  u.patch_Get = M(LV2_PATCH__Get);
  u.patch_property = M(LV2_PATCH__property);
  u.patch_value = M(LV2_PATCH__value);
  u.atom_String = M(LV2_ATOM__String);
  u.atom_Path = M(LV2_ATOM__Path);
  u.sess_text = M(AL_URI "#session");
  u.sess_dir = M(AL_URI "#session_dir");
  u.sess_id = M(AL_URI "#session_id");
  init_params();
  for (int i = 0; i < PR_COUNT; ++i) {
    char uri[96];
    std::snprintf(uri, sizeof uri, "%s#%.15s", AL_URI, kParams[i].sym);
    u.param[i] = M(uri);
    pl->vals[i] = kParams[i].def;
  }
  lv2_atom_forge_init(&pl->forge, map);
  pl->apply_all();

  // Пики волн для виджета: файл в /dev/shm, виджет находит его по pid процесса jalv.
  std::snprintf(pl->peaks_path, sizeof pl->peaks_path, "/dev/shm/eka_tape_%d", (int)getpid());
  int fd = open(pl->peaks_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    if (ftruncate(fd, (off_t)sizeof(PeakShm)) == 0) {
      void* mem = mmap(nullptr, sizeof(PeakShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (mem != MAP_FAILED) {
        pl->peaks = static_cast<PeakShm*>(mem);
        std::memset(pl->peaks, 0, sizeof(PeakShm));
        pl->peaks->ncols = kPeakCols;
        pl->peaks->ntracks = kMaxSlots;
        pl->peaks->nlayers = kPeakLayers;
        pl->peaks->magic = kPeakMagic;
        pl->looper->set_peaks(pl->peaks);
      }
    }
    close(fd);
  }

  pl->clock.set_sample_rate(rate);
  pl->midi_clock.set_sample_rate(rate);
  pl->apply_settings();  // пороги жестов и долей в такте — с известной частотой
  pl->saver.th = std::thread(save_worker, pl);
  return pl;
}

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features) {
  LV2_URID_Map* map = nullptr;
  LV2_Worker_Schedule* schedule = nullptr;
  for (int i = 0; features && features[i]; ++i) {
    if (!std::strcmp(features[i]->URI, LV2_URID__map)) map = static_cast<LV2_URID_Map*>(features[i]->data);
    if (!std::strcmp(features[i]->URI, LV2_WORKER__schedule))
      schedule = static_cast<LV2_Worker_Schedule*>(features[i]->data);
  }
  if (!map || !schedule) return nullptr;
  return create_plugin(rate, map, schedule, kPoolMbDefault);
}

void connect_port(LV2_Handle h, uint32_t port, void* data) {
  if (port < P_COUNT) static_cast<Plugin*>(h)->ports[port] = data;
}

void run(LV2_Handle h, uint32_t n) {
  Plugin* pl = static_cast<Plugin*>(h);
  Looper* lp = pl->looper;
  apply_pending(pl);  // настройки из restore() (пресет, снапшот)

  // Время блока — по часам на его начало (сообщения о позиции действуют со следующего блока).
  TimeInfo ti;
  ti.beats = pl->clock.beats();
  ti.beats_per_frame = pl->clock.bpm() / 60.0 / pl->sr;
  ti.bpb = pl->clock.bpb();
  ti.rolling = pl->clock.rolling();

  // События: темп, MIDI-клок, настройки. Часы двигаем кусками между событиями.
  uint32_t done = 0;
  if (const auto* seq = static_cast<const LV2_Atom_Sequence*>(pl->ports[P_EVENTS])) {
    LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
      uint32_t f = ev->time.frames < n ? (uint32_t)ev->time.frames : n;
      if (f > done) { pl->clock.advance(f - done); done = f; }
      if (ev->body.type == pl->u.midi_Event) {
        handle_midi(pl, reinterpret_cast<const uint8_t*>(&ev->body + 1), ev->body.size, pl->now + f);
      } else if (ev->body.type == pl->u.atom_Object || ev->body.type == pl->u.atom_Blank) {
        const auto* obj = reinterpret_cast<const LV2_Atom_Object*>(&ev->body);
        if (obj->body.otype == pl->u.time_Position) handle_position(pl, obj);
        else if (obj->body.otype == pl->u.patch_Set) handle_patch_set(pl, obj);
        else if (obj->body.otype == pl->u.patch_Get) pl->report_next = 0;
      }
    }
  }
  if (n > done) pl->clock.advance(n - done);
  pl->now += n;

  // Контроллеры-действия -> команды ядру (до звука, чтобы нажатие попало в этот блок).
  lp->set_time(ti);
  uint32_t held = 0;
  for (uint32_t i = 0; i < kButtons; ++i) {
    Gesture g = pl->buttons[i].process(pl->in(P_REC + i), n);
    if (g == Gesture::Tap) do_action(pl, kDefs[i].tap);
    if (g == Gesture::Hold) do_action(pl, kDefs[i].hold);
    if (pl->buttons[i].down()) held |= 1u << i;
  }

  handle_knobs(pl);
  lp->process(static_cast<const float*>(pl->ports[P_IN_L]), static_cast<const float*>(pl->ports[P_IN_R]),
              static_cast<float*>(pl->ports[P_OUT_L]), static_cast<float*>(pl->ports[P_OUT_R]), n, ti);

  WorkMsg m;
  while (lp->take_work(m)) {
    if (pl->schedule->schedule_work(pl->schedule->handle, sizeof m, &m) != LV2_WORKER_SUCCESS) {
      lp->cancel_work(m);
      break;
    }
  }

  pl->pull_mix();
  report_params(pl);

  // Выходы. Такт и доля — по сетке лупера (при стоящем транспорте он считает такты сам).
  const double gb = lp->grid_beats();
  const int bpb = pl->clock.bpb();
  const double bar = std::floor(gb / bpb);
  int src = pl->clock.messages() > 0 ? SRC_POSITION : (pl->midi_clock.active(pl->now) ? SRC_CLOCK : SRC_NONE);
  pl->out(O_TRACK, (float)lp->active());
  pl->out(O_PART, (float)lp->part());
  pl->out(O_PEND, (float)lp->pending_part());
  pl->out(O_PARTS, (float)lp->parts_mask());
  pl->out(O_WIDGET, (float)pl->widget);
  pl->out(O_ACTION, (float)pl->last_action);
  pl->out(O_ACT_CNT, (float)(pl->action_count % 1000000));
  pl->out(O_HELD, (float)held);
  pl->out(O_BPM, (float)pl->clock.bpm());
  pl->out(O_BAR, (float)std::fmod(bar, 1000000.0));
  pl->out(O_BEAT, (float)(gb - bar * bpb));
  pl->out(O_BPB, (float)bpb);
  pl->out(O_ROLLING, pl->clock.rolling() ? 1.0f : 0.0f);
  pl->out(O_TSRC, (float)src);
  pl->out(O_CLK_BPM, (float)pl->midi_clock.bpm());
  pl->out(O_POS_CNT, (float)(pl->clock.messages() % 1000000));
  // дорожки текущего парта; места за краем сетки — пустые
  const int n_grid = lp->grid();
  uint32_t flags = 0, mutes = 0, revs = 0, stops = 0, locks = 0;
  for (int i = 0; i < kMaxSlots; ++i) {
    TrackView v{};
    v.speed = v.espeed = 1.0f;
    if (i < n_grid) v = lp->view(i);
    pl->out(kOutSt[i], (float)v.state);
    pl->out(kOutPos[i], v.pos);
    pl->out(kOutLen[i], v.len_s);
    pl->out(kOutPk[i], i < n_grid ? lp->peak(i) : 0.0f);
    pl->out(kOutSpd[i], v.speed);
    pl->out(kOutEspd[i], v.espeed);
    if (v.muted) mutes |= 1u << i;
    if (v.reversed) revs |= 1u << i;
    if (v.stopped) stops |= 1u << i;
    if (v.locked) locks |= 1u << i;
    if (i < 4) {  // старый упакованный порт: дорожки 1–4
      if (v.muted) flags |= 1u << i;
      if (v.reversed) flags |= 1u << (4 + i);
      if (v.can_undo) flags |= 1u << (8 + i);
      if (v.can_redo) flags |= 1u << (12 + i);
      if (v.stopped) flags |= 1u << (16 + i);
      if (v.locked) flags |= 1u << (20 + i);
    }
  }
  if (lp->rec_after_switch()) flags |= 1u << 24;  // запись взведена на новый парт
  pl->out(O_FLAGS, (float)flags);
  pl->out(O_MUTES, (float)mutes);
  pl->out(O_REVS, (float)revs);
  pl->out(O_STOPS, (float)stops);
  pl->out(O_LOCKS, (float)locks);
  pl->out(O_GRID, (float)n_grid);
  pl->out(O_POOL_MB, (float)(lp->pool_bytes() >> 20));
  pl->out(O_SESS, (float)lp->session_status());
  pl->out(O_SESS_PROG, lp->session_progress());
  pl->out(O_COUNT, (float)lp->view(lp->active()).count);
  pl->out(O_PSTAT, lp->pitch_latency() <= 0 ? 2.0f : (lp->pitch_overload() ? 1.0f : 0.0f));
  pl->out(O_POOL, lp->pool_fill());
  pl->out(O_MPK_L, lp->master_peak(0));
  pl->out(O_MPK_R, lp->master_peak(1));
}

void cleanup(LV2_Handle h) {
  Plugin* pl = static_cast<Plugin*>(h);
  stop_saver(pl);
  if (pl->peaks) {
    munmap(pl->peaks, sizeof(PeakShm));
    unlink(pl->peaks_path);
  }
  delete pl->looper;
  delete pl;
}

// ---- рабочий поток ----
LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh,
                       uint32_t size, const void* data) {
  if (size != sizeof(WorkMsg)) return LV2_WORKER_ERR_UNKNOWN;
  WorkMsg m;
  std::memcpy(&m, data, sizeof m);
  WorkResp r;
  static_cast<Plugin*>(h)->looper->do_work(m, r);
  return respond(rh, sizeof r, &r);
}

LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void* data) {
  if (size != sizeof(WorkResp)) return LV2_WORKER_ERR_UNKNOWN;
  WorkResp r;
  std::memcpy(&r, data, sizeof r);
  static_cast<Plugin*>(h)->looper->work_done(r);
  return LV2_WORKER_SUCCESS;
}

// ---- state: настройки и сессия (звук) попадают в пресеты Zynthian ----
// Проверено пробой на Zynthian V5 (tape/CLAUDE.md): jalv вызывает save() в отдельном потоке, звук идёт;
// makePath ведёт в папку пресетов плагина (одна на все пресеты); restore() с threadSafeRestore — тоже
// в своём потоке, одновременно с run(). Снапшот хранит ссылку на пресет, save() не вызывает.

constexpr int kSaveWaitMs = 120000;   // фоновое сохранение ждёт закрытия записи и овердаба не дольше 2 минут
constexpr int kOrphanAgeS = 600;      // папку без пресета удаляем, только если ей больше 10 минут
constexpr char kSessPrefix[] = "tape_";
constexpr char kSessFile[] = "session.txt";      // описание сессии; появляется последним — сессия записана целиком
constexpr char kSessTmp[] = "session.txt.tmp";

const void* find_feature(const LV2_Feature* const* fs, const char* uri) {
  for (int i = 0; fs && fs[i]; ++i)
    if (!std::strcmp(fs[i]->URI, uri)) return fs[i]->data;
  return nullptr;
}

std::string read_file(const std::string& path, size_t max = 4u << 20) {
  std::string s;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return s;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && s.size() < max) s.append(buf, n);
  std::fclose(f);
  return s;
}

// Папка сессии: «tape_2026-11-11_06-44-31» (без пробелов: путь хранится в пресете как URI);
// k > 1 — в ту же секунду уже сохраняли: «…_2», «…_3».
std::string session_folder_name(int k) {
  char name[64];
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  std::strftime(name, sizeof name, "tape_%Y-%m-%d_%H-%M-%S", &tm);
  return k > 1 ? std::string(name) + "_" + std::to_string(k) : std::string(name);
}

// Папка сессии для человека: что в ней. Имя пресета дописывает уборка (save() его не знает).
void write_info(const std::string& dir, const SessionDesc& d, const std::string& preset) {
  std::string s = "Eka Tape session\n";
  char line[256];
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  std::strftime(line, sizeof line, "Saved: %Y-%m-%d %H:%M:%S\n", &tm);
  s += line;
  s += "Preset: " + (preset.empty() ? std::string("(not known yet)") : preset) + "\n";
  std::snprintf(line, sizeof line, "Grid: %dx%d, current part %d, active track %d\nSample rate: %.0f Hz\nTracks:\n",
                d.grid, d.grid, d.part + 1, d.active + 1, d.sr);
  s += line;
  for (int k = 0; k < d.grid * d.grid && k < kTracks; ++k) {
    const SessionTrack& t = d.t[k];
    if (!t.used) continue;
    std::snprintf(line, sizeof line, "  %-18s %7.2f s%s%s%s%s\n", t.file, (double)t.len / d.sr,
                  t.synced ? ", Quantize" : ", Free", t.muted ? ", mute" : "", t.reversed ? ", reverse" : "",
                  t.locked ? ", lock" : "");
    s += line;
    if (t.synced && t.rec_bpm > 0) {
      std::snprintf(line, sizeof line, "  %-18s %.0f beats at %.2f BPM\n", "", t.loop_beats, t.rec_bpm);
      s += line;
    }
    if (t.gain > 1.0f) {
      std::snprintf(line, sizeof line, "  %-18s louder than 0 dB: file is %.1f dB quieter\n", "",
                    20.0 * std::log10((double)t.gain));
      s += line;
    }
  }
  FILE* f = std::fopen((dir + "/info.txt").c_str(), "wb");
  if (!f) return;
  std::fwrite(s.data(), 1, s.size(), f);
  std::fclose(f);
}

// Имя пресета в info.txt (если ещё не то).
void set_info_preset(const std::string& dir, const std::string& preset) {
  const std::string path = dir + "/info.txt";
  std::string s = read_file(path);
  const size_t a = s.find("Preset: ");
  if (a == std::string::npos) return;
  const size_t b = s.find('\n', a);
  const std::string want = "Preset: " + preset;
  if (s.compare(a, b - a, want) == 0) return;
  s.replace(a, b - a, want);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fwrite(s.data(), 1, s.size(), f);
  std::fclose(f);
}

bool is_session_folder(const char* n) {
  return !std::strncmp(n, kSessPrefix, sizeof kSessPrefix - 1) && std::strlen(n) >= 24 && !std::strchr(n, '/');
}

// Удалить только то, что пишем мы: part* track*.wav, session.txt и info.txt; папку — если после этого пустая.
void remove_session_folder(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> files;
  while (dirent* e = readdir(d)) {
    const char* n = e->d_name;
    const size_t len = std::strlen(n);
    const bool ours = !std::strcmp(n, "info.txt") || !std::strcmp(n, kSessFile) || !std::strcmp(n, kSessTmp) ||
                      (!std::strncmp(n, "part", 4) && len > 4 && !std::strcmp(n + len - 4, ".wav"));
    if (ours) files.push_back(n);
  }
  closedir(d);
  for (const auto& f : files) unlink((dir + "/" + f).c_str());
  rmdir(dir.c_str());  // не пустая (там чужое) — останется
}

// Уборка в папке пресетов: папки сессий, на которые не ссылается ни один пресет (удалённые или
// перезаписанные), удаляются; у остальных в info.txt — имя пресета. keep — только что сохранённая
// (её пресет хост запишет после save()).
void cleanup_sessions(const std::string& bundle, const std::string& keep) {
  DIR* d = opendir(bundle.c_str());
  if (!d) return;
  std::vector<std::string> folders, ttls;
  while (dirent* e = readdir(d)) {
    const char* n = e->d_name;
    const size_t len = std::strlen(n);
    if (is_session_folder(n)) folders.push_back(n);
    else if (len > 4 && !std::strcmp(n + len - 4, ".ttl") && std::strcmp(n, "manifest.ttl")) ttls.push_back(n);
  }
  closedir(d);
  std::vector<std::string> texts;
  for (const auto& t : ttls) texts.push_back(read_file(bundle + "/" + t, 1u << 20));
  const std::time_t now = std::time(nullptr);
  for (const auto& f : folders) {
    if (f == keep) continue;
    const std::string dir = bundle + "/" + f;
    int ref = -1;
    for (size_t i = 0; i < texts.size() && ref < 0; ++i)
      if (texts[i].find(f) != std::string::npos) ref = (int)i;
    if (ref >= 0) {
      // rdfs:label "Имя" — имя пресета
      const std::string& tx = texts[ref];
      const size_t a = tx.find("rdfs:label \"");
      if (a != std::string::npos) {
        const size_t b = tx.find('"', a + 12);
        if (b != std::string::npos) set_info_preset(dir, tx.substr(a + 12, b - a - 12));
      }
      continue;
    }
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || now - st.st_mtime < kOrphanAgeS) continue;
    remove_session_folder(dir);
  }
}

void store_params(Plugin* pl, LV2_State_Store_Function store, LV2_State_Handle sh) {
  for (int i = 0; i < PR_COUNT; ++i) {
    if (kParams[i].is_float) {
      store(sh, pl->u.param[i], &pl->vals[i], sizeof(float), pl->u.atom_Float, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    } else {
      const int32_t v = (int32_t)pl->vals[i];
      store(sh, pl->u.param[i], &v, sizeof v, pl->u.atom_Int, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    }
  }
}

// Записать файл целиком через временный (с fsync): при сбое прежнего содержимого не будет видно наполовину.
bool write_file_atomic(const std::string& dir, const char* name, const char* tmp, const std::string& text) {
  const std::string tp = dir + "/" + tmp;
  FILE* f = std::fopen(tp.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = std::fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = std::fclose(f) == 0 && ok;
  if (!ok || std::rename(tp.c_str(), (dir + "/" + name).c_str()) != 0) {
    unlink(tp.c_str());
    return false;
  }
  return true;
}

// Одно задание: дождаться, пока закроются запись и овердаб (педали работают: поток jalv свободен),
// заморозить звук, записать WAV, затем session.txt и info.txt. Не вышло — свои файлы из папки убрать.
void run_save_job(Plugin* pl, const SaveJob& job) {
  Looper* lp = pl->looper;
  lp->session_set_status(SS_WAIT);
  lp->session_request_freeze();
  for (int ms = 0; !lp->session_frozen() && ms < kSaveWaitMs && !pl->saver.stop.load(); ms += 5)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!lp->session_frozen()) {
    lp->session_unfreeze(false, 0);
    if (!pl->saver.stop.load()) lp->session_set_status(SS_E_TIMEOUT);
    remove_session_folder(job.dir);
    return;
  }
  SessionDesc d;
  int st = lp->session_save_audio(job.dir.c_str(), d);
  d.id = job.id;  // id известен с save(): по нему restore() узнаёт свою сессию
  if (st == SS_SAVED && !write_file_atomic(job.dir, kSessFile, kSessTmp, session_to_text(d))) st = SS_E_WRITE;
  lp->session_unfreeze(st == SS_SAVED, d.id);
  if (st != SS_SAVED) {
    lp->session_set_status(st);
    remove_session_folder(job.dir);
    return;
  }
  write_info(job.dir, d, "");
  if (!job.bundle.empty()) cleanup_sessions(job.bundle, job.name);
}

void save_worker(Plugin* pl) {
  Saver& sv = pl->saver;
  for (;;) {
    SaveJob job;
    {
      std::unique_lock<std::mutex> lk(sv.mx);
      sv.cv.wait(lk, [&] { return sv.stop.load() || !sv.q.empty(); });
      if (sv.stop.load()) break;
      job = sv.q.front();
      sv.q.pop_front();
      sv.cur = job.id;
    }
    run_save_job(pl, job);
    std::lock_guard<std::mutex> lk(sv.mx);
    sv.cur = 0;
  }
}

// Плагин удаляют: начатая запись WAV дописывается, ждущие задания отменяются.
void stop_saver(Plugin* pl) {
  Saver& sv = pl->saver;
  if (!sv.th.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(sv.mx);
    sv.stop.store(true);
  }
  sv.cv.notify_all();
  sv.th.join();
  for (const auto& j : sv.q) remove_session_folder(j.dir);
  sv.q.clear();
}

// Сохранение ещё не закончено (ждёт или пишется).
bool saving_id(Plugin* pl, uint64_t id) {
  std::lock_guard<std::mutex> lk(pl->saver.mx);
  if (pl->saver.cur == id) return true;
  for (const auto& j : pl->saver.q)
    if (j.id == id) return true;
  return false;
}
[[maybe_unused]] bool saver_idle(Plugin* pl) {  // для тестов
  std::lock_guard<std::mutex> lk(pl->saver.mx);
  return pl->saver.cur == 0 && pl->saver.q.empty();
}

// save() возвращается сразу: в пресет — id сессии и путь к её папке, звук и описание (session.txt)
// пишет фоновый поток. Без makePath (другой хост) или без лупов — только настройки: такой пресет
// при загрузке лупы не трогает.
void save_session(Plugin* pl, LV2_State_Store_Function store, LV2_State_Handle sh, const LV2_Feature* const* fs) {
  const auto* mk = static_cast<const LV2_State_Make_Path*>(find_feature(fs, LV2_STATE__makePath));
  const auto* mp = static_cast<const LV2_State_Map_Path*>(find_feature(fs, LV2_STATE__mapPath));
  const auto* fp = static_cast<const LV2_State_Free_Path*>(find_feature(fs, LV2_STATE__freePath));
  if (!mk || !mp || !pl->looper->has_loops() || !pl->saver.th.joinable()) return;
  auto free_path = [fp](char* p) {
    if (!p) return;
    if (fp) fp->free_path(fp->handle, p);
    else std::free(p);
  };
  // папка: makePath даёт путь в папке пресетов; имя — по времени, своя у каждого сохранения
  std::string name;
  char* dir = nullptr;
  for (int k = 1; k < 100 && !dir; ++k) {
    name = session_folder_name(k);
    dir = mk->path(mk->handle, name.c_str());
    if (dir && mkdir(dir, 0755) != 0) {  // занята (или не создаётся) — следующее имя
      const bool busy = errno == EEXIST;
      free_path(dir);
      dir = nullptr;
      if (!busy) break;
    }
  }
  if (!dir) {
    pl->looper->session_set_status(SS_E_WRITE);
    return;
  }
  SaveJob job;
  job.dir = dir;
  job.name = name;
  job.id = session_new_id();
  const size_t sl = job.dir.rfind('/');
  job.bundle = sl == std::string::npos ? std::string() : job.dir.substr(0, sl);
  const int64_t id = (int64_t)job.id;
  store(sh, pl->u.sess_id, &id, sizeof id, pl->u.atom_Long, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
  char* apath = mp->abstract_path(mp->handle, dir);
  const char* sp = apath ? apath : dir;
  store(sh, pl->u.sess_dir, sp, std::strlen(sp) + 1, pl->u.atom_Path, LV2_STATE_IS_POD);
  free_path(apath);
  free_path(dir);
  pl->looper->session_set_status(SS_WAIT);
  {
    std::lock_guard<std::mutex> lk(pl->saver.mx);
    pl->saver.q.push_back(job);
  }
  pl->saver.cv.notify_one();
}

LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle sh, uint32_t,
                      const LV2_Feature* const* features) {
  Plugin* pl = static_cast<Plugin*>(h);
  save_session(pl, store, sh, features);
  store_params(pl, store, sh);
  return LV2_STATE_SUCCESS;
}

// Пресет или снапшот. Настройки — через почтовый ящик в run(); сессия — проверка и загрузка в фоне.
// Сессию нельзя загрузить (нет файлов, не помещается, другая частота) — не меняем ничего: ни лупы,
// ни настройки; причина — в статусе для виджета.
LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle sh, uint32_t,
                         const LV2_Feature* const* features) {
  Plugin* pl = static_cast<Plugin*>(h);
  float val[PR_COUNT];
  bool has[PR_COUNT];
  for (int i = 0; i < PR_COUNT; ++i) {
    size_t size = 0;
    uint32_t type = 0, flags = 0;
    const void* v = retrieve(sh, pl->u.param[i], &size, &type, &flags);
    has[i] = false;
    val[i] = 0.0f;
    if (v && type == pl->u.atom_Int && size == sizeof(int32_t)) {
      val[i] = (float)*static_cast<const int32_t*>(v);
      has[i] = true;
    } else if (v && type == pl->u.atom_Float && size == sizeof(float)) {
      val[i] = *static_cast<const float*>(v);
      has[i] = true;
    }
  }
  size_t size = 0;
  uint32_t type = 0, flags = 0;
  Looper* lp = pl->looper;
  // путь к папке сессии
  std::string dir;
  const void* dv = retrieve(sh, pl->u.sess_dir, &size, &type, &flags);
  if (dv && type == pl->u.atom_Path) {
    const auto* mp = static_cast<const LV2_State_Map_Path*>(find_feature(features, LV2_STATE__mapPath));
    const auto* fp = static_cast<const LV2_State_Free_Path*>(find_feature(features, LV2_STATE__freePath));
    char* abs = mp ? mp->absolute_path(mp->handle, static_cast<const char*>(dv)) : nullptr;
    dir = abs ? abs : static_cast<const char*>(dv);
    if (abs) {
      if (fp) fp->free_path(fp->handle, abs);
      else std::free(abs);
    }
  }
  SessionDesc d;
  const void* txt = retrieve(sh, pl->u.sess_text, &size, &type, &flags);
  if (txt && type == pl->u.atom_String && size > 0) {
    // первая версия 7а: описание прямо в пресете
    if (!session_from_text(static_cast<const char*>(txt), std::strlen(static_cast<const char*>(txt)), d)) {
      lp->session_set_status(SS_E_FORMAT);
      return LV2_STATE_SUCCESS;
    }
  } else {
    const void* iv = retrieve(sh, pl->u.sess_id, &size, &type, &flags);
    if (!iv || type != pl->u.atom_Long || size != sizeof(int64_t) || dir.empty()) {
      post_params(pl, val, has);  // только настройки: лупы не трогаем
      return LV2_STATE_SUCCESS;
    }
    const uint64_t id = (uint64_t)*static_cast<const int64_t*>(iv);
    if (saving_id(pl, id)) {
      // Zynthian загружает пресет сразу после сохранения: звук ещё пишется, и это он и есть
      post_params(pl, val, has);
      return LV2_STATE_SUCCESS;
    }
    const std::string text = read_file(dir + "/" + kSessFile, 1u << 20);
    if (text.empty()) {
      lp->session_set_status(SS_E_MISSING);
      return LV2_STATE_SUCCESS;
    }
    if (!session_from_text(text.data(), text.size(), d) || d.id != id) {
      lp->session_set_status(SS_E_FORMAT);
      return LV2_STATE_SUCCESS;
    }
  }
  // та же сессия, что играет, без изменений звука — ядро само ничего не загрузит; проверка всё равно дешёвая
  const int st = session_check(d, dir.c_str(), pl->sr, lp->pool_chunks());
  if (st != SS_IDLE) {
    lp->session_set_status(st);
    return LV2_STATE_SUCCESS;
  }
  post_params(pl, val, has);
  lp->session_load(d, dir.c_str());
  return LV2_STATE_SUCCESS;
}

const void* extension_data(const char* uri) {
  static const LV2_Worker_Interface worker = {work, work_response, nullptr};
  static const LV2_State_Interface state = {save, restore};
  if (!std::strcmp(uri, LV2_WORKER__interface)) return &worker;
  if (!std::strcmp(uri, LV2_STATE__interface)) return &state;
  return nullptr;
}

const LV2_Descriptor kDescriptor = {AL_URI, instantiate, connect_port, nullptr, run, nullptr, cleanup, extension_data};

}  // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  return index == 0 ? &kDescriptor : nullptr;
}
