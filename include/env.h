#ifndef ENV_H
#define ENV_H

#include "types.h"

#define ENV_MAX_VARS   64
#define ENV_KEY_LEN    32
#define ENV_VAL_LEN    128

void        env_init(void);
const char* env_get(const char* key);
void        env_set(const char* key, const char* value);
void        env_unset(const char* key);
void        env_list(void);
void        env_expand(const char* input, char* output, size_t max);
uint32_t    env_count(void);

#endif
