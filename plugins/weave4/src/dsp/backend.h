/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EKA_BACKEND_H
#define EKA_BACKEND_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct EkaBackend EkaBackend;
EkaBackend *eka_backend_create(unsigned rate);
void eka_backend_destroy(EkaBackend *p);
void eka_backend_reset(EkaBackend *p);
void eka_backend_pitch(EkaBackend *p, double ratio);
unsigned eka_backend_block(const EkaBackend *p);
unsigned eka_backend_tracking_age(const EkaBackend *p);
unsigned eka_backend_delay(const EkaBackend *p);
unsigned eka_backend_faults(const EkaBackend *p);
void eka_backend_process(EkaBackend *p, const float *in, float *out);
#ifdef __cplusplus
}
#endif
#endif
