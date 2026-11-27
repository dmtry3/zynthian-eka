/* Background JACK Aux Send auto-connector for Eka Pads.
 * Connects <own_jackname>:aux1..4_{l,r} automatically after LV2 state_restore / patch:Set
 * using dlopen("libjack.so.0") so sends work immediately even before the Zynthian widget opens. */
#define _POSIX_C_SOURCE 200809L
#include "aux_jack.h"

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JACK_OPT_NO_START_SERVER 0x01
#define JACK_PORT_IS_INPUT       0x01UL
#define JACK_PORT_IS_OUTPUT      0x02UL
#define JACK_AUDIO_TYPE          "32 bit float mono audio"

typedef void* (*fn_jack_client_open)(const char* name, int options, int* status, ...);
typedef int (*fn_jack_client_close)(void* client);
typedef const char** (*fn_jack_get_ports)(void* client, const char* port_pat,
                                          const char* type_pat, unsigned long flags);
typedef void* (*fn_jack_port_by_name)(void* client, const char* port_name);
typedef const char** (*fn_jack_port_get_all_connections)(const void* client, const void* port);
typedef int (*fn_jack_connect)(void* client, const char* src, const char* dst);
typedef int (*fn_jack_disconnect)(void* client, const char* src, const char* dst);
typedef void (*fn_jack_free)(void* ptr);

typedef struct {
  void* dl;
  fn_jack_client_open client_open;
  fn_jack_client_close client_close;
  fn_jack_get_ports get_ports;
  fn_jack_port_by_name port_by_name;
  fn_jack_port_get_all_connections port_get_all_connections;
  fn_jack_connect connect;
  fn_jack_disconnect disconnect;
  fn_jack_free jfree;
} JackApi;

struct EpAuxJack {
  atomic_uint seq;
  atomic_int  stop;
  char routes_buf[EP_AUX_ROUTES_MAX];
  pthread_t thread;
  int thread_started;
};

static int hex_val(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

static void url_decode_slice(const char* src, size_t len, char* dst, size_t cap) {
  if (!dst || cap == 0) return;
  size_t w = 0;
  for (size_t i = 0; i < len && w + 1 < cap; ) {
    if (src[i] == '%' && i + 2 < len) {
      int hi = hex_val(src[i + 1]);
      int lo = hex_val(src[i + 2]);
      if (hi >= 0 && lo >= 0) {
        dst[w++] = (char)((hi << 4) | lo);
        i += 3;
        continue;
      }
    }
    dst[w++] = src[i++];
  }
  dst[w] = '\0';
}

int ep_aux_parse_routes(const char* routes, EpAuxSlotSpec out_slots[4]) {
  for (int i = 0; i < 4; i++) {
    out_slots[i].kind = EP_AUX_SLOT_OFF;
    out_slots[i].dst_l[0] = '\0';
    out_slots[i].dst_r[0] = '\0';
  }
  if (!routes || !routes[0] || strcmp(routes, "_") == 0) return 0;

  /* Guard against legacy atom:Path presets where lilv prepended a bundle directory */
  if (routes[0] == '/') {
    const char* fc = strstr(routes, "/chain:");
    const char* fj = strstr(routes, "/jack:");
    const char* found = fc;
    if (fj && (!found || fj < found)) found = fj;
    if (found) routes = found + 1;
  }

  int active = 0;
  const char* p = routes;
  for (int ai = 0; ai < 4 && *p; ai++) {
    const char* semi = strchr(p, ';');
    size_t slen = semi ? (size_t)(semi - p) : strlen(p);
    if (slen > 0) {
      const char* at = (const char*)memchr(p, '@', slen);
      if (at) {
        const char* pstr = at + 1;
        size_t plen = slen - (size_t)(pstr - p);
        const char* comma = (const char*)memchr(pstr, ',', plen);
        if (comma) {
          url_decode_slice(pstr, (size_t)(comma - pstr), out_slots[ai].dst_l, EP_AUX_PORT_MAX);
          url_decode_slice(comma + 1, plen - (size_t)(comma + 1 - pstr),
                           out_slots[ai].dst_r, EP_AUX_PORT_MAX);
        } else {
          url_decode_slice(pstr, plen, out_slots[ai].dst_l, EP_AUX_PORT_MAX);
          snprintf(out_slots[ai].dst_r, EP_AUX_PORT_MAX, "%s", out_slots[ai].dst_l);
        }
        if (out_slots[ai].dst_l[0] && out_slots[ai].dst_r[0]) {
          out_slots[ai].kind = EP_AUX_SLOT_PORTS;
        } else {
          out_slots[ai].kind = EP_AUX_SLOT_UNRESOLVED;
        }
        active++;
      } else {
        char tid[EP_AUX_PORT_MAX];
        url_decode_slice(p, slen, tid, sizeof tid);
        if (strncmp(tid, "jack:", 5) == 0 && tid[5] != '\0') {
          out_slots[ai].kind = EP_AUX_SLOT_JACK_PREFIX;
          snprintf(out_slots[ai].dst_l, EP_AUX_PORT_MAX, "%s", tid + 5);
          active++;
        } else if (tid[0] != '\0' && strcmp(tid, "_") != 0) {
          out_slots[ai].kind = EP_AUX_SLOT_UNRESOLVED;
          active++;
        }
      }
    }
    if (!semi) break;
    p = semi + 1;
  }
  return active;
}

static int load_jack_api(JackApi* api) {
  memset(api, 0, sizeof *api);
  api->dl = dlopen("libjack.so.0", RTLD_LAZY);
  if (!api->dl) api->dl = dlopen("libjack.so", RTLD_LAZY);
  if (!api->dl) return 0;

  api->client_open              = (fn_jack_client_open)dlsym(api->dl, "jack_client_open");
  api->client_close             = (fn_jack_client_close)dlsym(api->dl, "jack_client_close");
  api->get_ports                = (fn_jack_get_ports)dlsym(api->dl, "jack_get_ports");
  api->port_by_name             = (fn_jack_port_by_name)dlsym(api->dl, "jack_port_by_name");
  api->port_get_all_connections = (fn_jack_port_get_all_connections)dlsym(api->dl, "jack_port_get_all_connections");
  api->connect                  = (fn_jack_connect)dlsym(api->dl, "jack_connect");
  api->disconnect               = (fn_jack_disconnect)dlsym(api->dl, "jack_disconnect");
  api->jfree                    = (fn_jack_free)dlsym(api->dl, "jack_free");

  if (!api->client_open || !api->client_close || !api->get_ports ||
      !api->port_by_name || !api->port_get_all_connections ||
      !api->connect || !api->disconnect) {
    dlclose(api->dl);
    memset(api, 0, sizeof *api);
    return 0;
  }
  return 1;
}

static void jfree_safe(const JackApi* api, void* ptr) {
  if (!ptr) return;
  if (api->jfree) api->jfree(ptr);
  else free(ptr);
}

/* Extract jalv's JACK client name from /proc/self/cmdline (-n <name> or --jack-name <name>). */
static void detect_own_jackname(char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  int fd = open("/proc/self/cmdline", O_RDONLY);
  if (fd < 0) return;
  char buf[4096];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return;
  buf[n] = '\0';

  ssize_t i = 0;
  while (i < n) {
    const char* arg = buf + i;
    size_t alen = strlen(arg);
    i += (ssize_t)alen + 1;
    if (strcmp(arg, "-n") == 0 || strcmp(arg, "--jack-name") == 0) {
      if (i < n && buf[i] != '\0') {
        snprintf(out, cap, "%s", buf + i);
        return;
      }
    } else if (strncmp(arg, "-n", 2) == 0 && arg[2] != '\0' && arg[2] != '-') {
      snprintf(out, cap, "%s", arg + 2);
      return;
    } else if (strncmp(arg, "--jack-name=", 12) == 0 && arg[12] != '\0') {
      snprintf(out, cap, "%s", arg + 12);
      return;
    }
  }
}

static int str_ends_with(const char* s, const char* suf) {
  size_t sl = strlen(s), sufl = strlen(suf);
  return sl >= sufl && strcmp(s + sl - sufl, suf) == 0;
}

static int str_starts_with_ci(const char* s, const char* pref) {
  while (*pref) {
    if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*pref)) return 0;
    s++;
    pref++;
  }
  return 1;
}

static int find_own_aux_ports(const JackApi* api, void* jc, const char* own_jn, int aux_idx,
                              char* out_l, size_t cap_l, char* out_r, size_t cap_r) {
  char suf_l[32], suf_r[32];
  snprintf(suf_l, sizeof suf_l, ":aux%d_l", aux_idx + 1);
  snprintf(suf_r, sizeof suf_r, ":aux%d_r", aux_idx + 1);

  if (own_jn && own_jn[0]) {
    snprintf(out_l, cap_l, "%s%s", own_jn, suf_l);
    snprintf(out_r, cap_r, "%s%s", own_jn, suf_r);
    if (api->port_by_name(jc, out_l) && api->port_by_name(jc, out_r)) {
      return 1;
    }
  }

  const char** outs = api->get_ports(jc, NULL, JACK_AUDIO_TYPE, JACK_PORT_IS_OUTPUT);
  if (!outs) return 0;

  const char* pl = NULL;
  const char* pr = NULL;
  if (own_jn && own_jn[0]) {
    size_t jlen = strlen(own_jn);
    for (int i = 0; outs[i]; i++) {
      if (strncmp(outs[i], own_jn, jlen) == 0) {
        if (str_ends_with(outs[i], suf_l)) pl = outs[i];
        else if (str_ends_with(outs[i], suf_r)) pr = outs[i];
      }
    }
  }
  if (!pl || !pr) {
    /* Fallback: match unique :auxN_l / :auxN_r client if only one exists */
    int cnt_l = 0, cnt_r = 0;
    const char* fl = NULL;
    const char* fr = NULL;
    for (int i = 0; outs[i]; i++) {
      if (str_ends_with(outs[i], suf_l)) { fl = outs[i]; cnt_l++; }
      else if (str_ends_with(outs[i], suf_r)) { fr = outs[i]; cnt_r++; }
    }
    if (cnt_l == 1 && cnt_r == 1) {
      pl = fl;
      pr = fr;
    }
  }

  int ok = 0;
  if (pl && pr) {
    snprintf(out_l, cap_l, "%s", pl);
    snprintf(out_r, cap_r, "%s", pr);
    ok = 1;
  }
  jfree_safe(api, (void*)outs);
  return ok;
}

static int resolve_prefix_ports(const JackApi* api, void* jc, const char* prefix,
                                char* dst_l, size_t cap_l, char* dst_r, size_t cap_r) {
  const char** ins = api->get_ports(jc, NULL, JACK_AUDIO_TYPE, JACK_PORT_IS_INPUT);
  if (!ins) return 0;
  const char* first = NULL;
  const char* second = NULL;
  for (int i = 0; ins[i]; i++) {
    if (str_starts_with_ci(ins[i], prefix)) {
      if (!first) first = ins[i];
      else { second = ins[i]; break; }
    }
  }
  int ok = 0;
  if (first) {
    snprintf(dst_l, cap_l, "%s", first);
    snprintf(dst_r, cap_r, "%s", second ? second : first);
    ok = 1;
  }
  jfree_safe(api, (void*)ins);
  return ok;
}

static int ensure_port_conn(const JackApi* api, void* jc, const char* src_name,
                            const char* want_dst, int disconnect_others) {
  void* port = api->port_by_name(jc, src_name);
  if (!port) return 0;
  const char** conns = api->port_get_all_connections(jc, port);
  int already = 0;
  if (conns) {
    for (int i = 0; conns[i]; i++) {
      if (want_dst && strcmp(conns[i], want_dst) == 0) {
        already = 1;
      } else if (disconnect_others) {
        api->disconnect(jc, src_name, conns[i]);
      }
    }
    jfree_safe(api, (void*)conns);
  }
  if (!want_dst || !want_dst[0]) return 1;
  if (already) return 1;
  if (!api->port_by_name(jc, want_dst)) return 0;
  int rc = api->connect(jc, src_name, want_dst);
  return (rc == 0 || rc == 17 /* EEXIST */) ? 1 : 0;
}

static int sync_jack_routes(const JackApi* api, void* jc, const char* own_jn,
                            const EpAuxSlotSpec slots[4], int route_changed) {
  int all_ok = 1;
  for (int ai = 0; ai < 4; ai++) {
    const EpAuxSlotSpec* sl = &slots[ai];
    if (sl->kind == EP_AUX_SLOT_UNRESOLVED) continue;
    if (sl->kind == EP_AUX_SLOT_OFF && !route_changed) continue;

    char src_l[EP_AUX_PORT_MAX], src_r[EP_AUX_PORT_MAX];
    if (!find_own_aux_ports(api, jc, own_jn, ai, src_l, sizeof src_l, src_r, sizeof src_r)) {
      if (sl->kind != EP_AUX_SLOT_OFF) all_ok = 0;
      continue;
    }

    if (sl->kind == EP_AUX_SLOT_OFF) {
      ensure_port_conn(api, jc, src_l, NULL, 1);
      ensure_port_conn(api, jc, src_r, NULL, 1);
      continue;
    }

    char dl[EP_AUX_PORT_MAX], dr[EP_AUX_PORT_MAX];
    if (sl->kind == EP_AUX_SLOT_PORTS) {
      snprintf(dl, sizeof dl, "%s", sl->dst_l);
      snprintf(dr, sizeof dr, "%s", sl->dst_r);
    } else {
      if (!resolve_prefix_ports(api, jc, sl->dst_l, dl, sizeof dl, dr, sizeof dr)) {
        all_ok = 0;
        continue;
      }
    }

    int ok_l = ensure_port_conn(api, jc, src_l, dl, route_changed);
    int ok_r = ensure_port_conn(api, jc, src_r, dr, route_changed);
    if (!ok_l || !ok_r) all_ok = 0;
  }
  return all_ok;
}

static void sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}

static void* aux_jack_worker(void* arg) {
  EpAuxJack* aj = (EpAuxJack*)arg;
  JackApi api;
  memset(&api, 0, sizeof api);
  int api_tried = 0;
  int api_ok = 0;
  void* jc = NULL;
  char own_jn[128] = { 0 };
  int own_jn_checked = 0;

  EpAuxSlotSpec slots[4];
  ep_aux_parse_routes("", slots);
  uint32_t last_seq = 0;
  int active_cnt = 0;
  int has_connectable = 0;
  int all_connected = 1;
  int route_changed = 0;
  int pending_clear = 0;
  int ticks = 0;

  while (!atomic_load_explicit(&aj->stop, memory_order_acquire)) {
    /* Lock-free read of latest published aux_routes string */
    uint32_t s1 = atomic_load_explicit(&aj->seq, memory_order_acquire);
    if ((s1 & 1u) == 0 && s1 != last_seq) {
      char copy[EP_AUX_ROUTES_MAX];
      memcpy(copy, aj->routes_buf, sizeof copy);
      copy[sizeof(copy) - 1] = '\0';
      uint32_t s2 = atomic_load_explicit(&aj->seq, memory_order_acquire);
      if (s1 == s2) {
        last_seq = s1;
        int prev_conn = has_connectable;
        active_cnt = ep_aux_parse_routes(copy, slots);
        has_connectable = 0;
        for (int i = 0; i < 4; i++) {
          if (slots[i].kind == EP_AUX_SLOT_PORTS || slots[i].kind == EP_AUX_SLOT_JACK_PREFIX) {
            has_connectable = 1;
          }
        }
        route_changed = 1;
        all_connected = 0;
        if (!active_cnt && prev_conn) pending_clear = 1;
      }
    }

    int need_sync = 0;
    if (route_changed && (has_connectable || pending_clear)) {
      need_sync = 1;
    } else if (has_connectable) {
      int period = all_connected ? 20 : 8; /* 1.0s steady-state, 0.4s while waiting for target */
      if (ticks >= period) need_sync = 1;
    }

    if (need_sync) {
      ticks = 0;
      if (!api_tried) {
        api_tried = 1;
        api_ok = load_jack_api(&api);
      }
      if (!api_ok) {
        /* Host has no libjack (e.g. offline test / non-JACK DAW): stop polling. */
        break;
      }
      if (!own_jn_checked) {
        own_jn_checked = 1;
        detect_own_jackname(own_jn, sizeof own_jn);
      }
      if (!jc) {
        char cname[64];
        int status = 0;
        snprintf(cname, sizeof cname, "eka_pads_aux_%d", (int)getpid());
        jc = api.client_open(cname, JACK_OPT_NO_START_SERVER, &status);
      }
      if (jc) {
        /* Quick liveness check: if JACK restarted, reopen client cleanly */
        const char** probe = api.get_ports(jc, NULL, JACK_AUDIO_TYPE, JACK_PORT_IS_OUTPUT);
        if (!probe) {
          api.client_close(jc);
          jc = NULL;
          all_connected = 0;
        } else {
          jfree_safe(&api, (void*)probe);
          all_connected = sync_jack_routes(&api, jc, own_jn, slots, route_changed);
          route_changed = 0;
          pending_clear = 0;
        }
      } else {
        /* No JACK server running right now: avoid tight spinning */
        route_changed = 0;
        pending_clear = 0;
        all_connected = 1; /* check again in ~1s */
      }
    }

    sleep_ms(50);
    ticks++;
  }

  if (jc && api_ok) api.client_close(jc);
  if (api.dl) dlclose(api.dl);
  return NULL;
}

EpAuxJack* ep_aux_jack_create(void) {
  EpAuxJack* aj = (EpAuxJack*)calloc(1, sizeof(EpAuxJack));
  if (!aj) return NULL;
  atomic_init(&aj->seq, 0);
  atomic_init(&aj->stop, 0);
  if (pthread_create(&aj->thread, NULL, aux_jack_worker, aj) == 0) {
    aj->thread_started = 1;
  }
  return aj;
}

void ep_aux_jack_destroy(EpAuxJack* aj) {
  if (!aj) return;
  atomic_store_explicit(&aj->stop, 1, memory_order_release);
  if (aj->thread_started) {
    pthread_join(aj->thread, NULL);
    aj->thread_started = 0;
  }
  free(aj);
}

void ep_aux_jack_publish(EpAuxJack* aj, const char* routes) {
  if (!aj) return;
  atomic_fetch_add_explicit(&aj->seq, 1, memory_order_acq_rel);
  if (routes && routes[0]) {
    size_t n = strlen(routes);
    if (n >= EP_AUX_ROUTES_MAX) n = EP_AUX_ROUTES_MAX - 1;
    memcpy(aj->routes_buf, routes, n);
    aj->routes_buf[n] = '\0';
  } else {
    aj->routes_buf[0] = '\0';
  }
  atomic_fetch_add_explicit(&aj->seq, 1, memory_order_release);
}
