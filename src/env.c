#include "env.h"
#include "vga.h"

static struct { char key[ENV_KEY_LEN]; char val[ENV_VAL_LEN]; bool active; } vars[ENV_MAX_VARS];

void env_init(void) {
    memset(vars, 0, sizeof(vars));
    env_set("PATH", "/bin");
    env_set("HOME", "/home");
    env_set("USER", "root");
    env_set("HOSTNAME", "microkernel");
    env_set("SHELL", "/bin/sh");
    env_set("PS1", "\\u@\\h:\\w$ ");
    env_set("TERM", "vga");
    env_set("EDITOR", "edit");
    env_set("VERSION", "0.2.0");
}

const char* env_get(const char* key) {
    for (int i = 0; i < ENV_MAX_VARS; i++)
        if (vars[i].active && strcmp(vars[i].key, key) == 0)
            return vars[i].val;
    return NULL;
}

void env_set(const char* key, const char* value) {
    /* Update existing */
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (vars[i].active && strcmp(vars[i].key, key) == 0) {
            strncpy(vars[i].val, value, ENV_VAL_LEN - 1);
            return;
        }
    }
    /* Add new */
    for (int i = 0; i < ENV_MAX_VARS; i++) {
        if (!vars[i].active) {
            vars[i].active = true;
            strncpy(vars[i].key, key, ENV_KEY_LEN - 1);
            strncpy(vars[i].val, value, ENV_VAL_LEN - 1);
            return;
        }
    }
}

void env_unset(const char* key) {
    for (int i = 0; i < ENV_MAX_VARS; i++)
        if (vars[i].active && strcmp(vars[i].key, key) == 0)
            { vars[i].active = false; return; }
}

void env_list(void) {
    for (int i = 0; i < ENV_MAX_VARS; i++)
        if (vars[i].active)
            kprintf("  %s=%s\n", vars[i].key, vars[i].val);
}

uint32_t env_count(void) {
    uint32_t c = 0;
    for (int i = 0; i < ENV_MAX_VARS; i++) if (vars[i].active) c++;
    return c;
}

void env_expand(const char* input, char* output, size_t max) {
    size_t oi = 0;
    while (*input && oi < max - 1) {
        if (*input == '$') {
            input++;
            /* ${VAR} or $VAR */
            bool braced = (*input == '{');
            if (braced) input++;
            char name[ENV_KEY_LEN];
            int ni = 0;
            while (*input && ni < ENV_KEY_LEN - 1) {
                if (braced && *input == '}') { input++; break; }
                if (!braced && !isalpha(*input) && *input != '_' && !isdigit(*input)) break;
                name[ni++] = *input++;
            }
            name[ni] = '\0';
            const char* val = env_get(name);
            if (val) {
                while (*val && oi < max - 1) output[oi++] = *val++;
            }
        } else {
            output[oi++] = *input++;
        }
    }
    output[oi] = '\0';
}
