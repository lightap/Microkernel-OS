#ifndef LOGIN_H
#define LOGIN_H

#include "types.h"

#define MAX_USERS 8
#define USER_NAME_LEN 16
#define USER_PASS_LEN 32

void        login_init(void);
bool        login_authenticate(const char* user, const char* pass);
bool        login_prompt(void);   /* Full login screen, returns true on success */
bool        login_add_user(const char* user, const char* pass);
bool        login_del_user(const char* user);
bool        login_change_pass(const char* user, const char* old_pass, const char* new_pass);
const char* login_current_user(void);
void        login_set_user(const char* user);
void        login_logout(void);
void        login_list_users(void);
bool        login_is_logged_in(void);

#endif
