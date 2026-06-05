#ifndef BEMFA_H
#define BEMFA_H

#ifdef __cplusplus
extern "C" {
#endif

void bemfa_start(const char *uid);
void bemfa_stop(void);
void bemfa_publish_state(const char *topic, const char *msg);

#ifdef __cplusplus
}
#endif

#endif
