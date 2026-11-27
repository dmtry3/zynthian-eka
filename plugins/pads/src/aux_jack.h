/* Background JACK Aux Send auto-connector for Eka Pads (zero build-time JACK dependency). */
#ifndef EP_AUX_JACK_H
#define EP_AUX_JACK_H

#define EP_AUX_ROUTES_MAX 1024
#define EP_AUX_PORT_MAX   256

typedef enum {
  EP_AUX_SLOT_OFF = 0,       /* empty slot -> disconnect aux bus on route change */
  EP_AUX_SLOT_PORTS,         /* explicit dst_l, dst_r parsed from @dst_l,dst_r */
  EP_AUX_SLOT_JACK_PREFIX,   /* legacy jack:<prefix> -> resolve via JACK input ports */
  EP_AUX_SLOT_UNRESOLVED     /* legacy chain:<id> without @ports -> leave for widget */
} EpAuxSlotKind;

typedef struct {
  EpAuxSlotKind kind;
  char dst_l[EP_AUX_PORT_MAX];
  char dst_r[EP_AUX_PORT_MAX];
} EpAuxSlotSpec;

typedef struct EpAuxJack EpAuxJack;

/* Parse semicolon-separated aux_routes string into 4 slot specs. Returns number of non-OFF slots. */
int ep_aux_parse_routes(const char* routes, EpAuxSlotSpec out_slots[4]);

/* Lifecycle (non-RT: call from instantiate / cleanup). */
EpAuxJack* ep_aux_jack_create(void);
void ep_aux_jack_destroy(EpAuxJack* aj);

/* Lock-free publish from RT (run) or state_restore thread. */
void ep_aux_jack_publish(EpAuxJack* aj, const char* routes);

#endif
