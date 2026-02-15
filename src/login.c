#include "login.h"
#include "vga.h"
#include "keyboard.h"

static struct {
    char name[USER_NAME_LEN];
    uint32_t pass_hash;
    bool active;
    bool is_admin;
} users[MAX_USERS];

static char current_user[USER_NAME_LEN] = "";
static bool logged_in = false;

/* Simple DJB2 hash */
static uint32_t hash_pass(const char* s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (uint8_t)*s++;
    return h;
}

void login_init(void) {
    memset(users, 0, sizeof(users));
    /* Default accounts */
    users[0].active = true;
    strcpy(users[0].name, "root");
    users[0].pass_hash = hash_pass("root");
    users[0].is_admin = true;

    users[1].active = true;
    strcpy(users[1].name, "user");
    users[1].pass_hash = hash_pass("user");
    users[1].is_admin = false;

    users[2].active = true;
    strcpy(users[2].name, "guest");
    users[2].pass_hash = hash_pass("");
    users[2].is_admin = false;
}

bool login_authenticate(const char* user, const char* pass) {
    uint32_t h = hash_pass(pass);
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].active && strcmp(users[i].name, user) == 0 && users[i].pass_hash == h)
            return true;
    return false;
}

static void read_password(char* buf, size_t max) {
    size_t len = 0;
    while (1) {
        char c = keyboard_getchar();
        if (c == '\n') { terminal_putchar('\n'); buf[len] = '\0'; return; }
        if (c == '\b' && len > 0) { len--; terminal_backspace(); continue; }
        if (c >= 32 && c != 127 && len < max - 1) {
            buf[len++] = c;
            terminal_putchar('*');
        }
    }
}

bool login_prompt(void) {
    char user[USER_NAME_LEN], pass[USER_PASS_LEN];

    for (int attempts = 0; attempts < 3; attempts++) {
        terminal_print_colored("\n  microkernel login: ", 0x0F);
        keyboard_readline(user, USER_NAME_LEN);

        terminal_print_colored("  Password: ", 0x0F);
        read_password(pass, USER_PASS_LEN);

        if (login_authenticate(user, pass)) {
            strcpy(current_user, user);
            logged_in = true;
            terminal_print_colored("\n  Welcome, ", 0x0A);
            terminal_print_colored(user, 0x0A);
            terminal_print_colored("!\n\n", 0x0A);
            return true;
        }
        terminal_print_colored("  Login incorrect.\n", 0x0C);
    }
    terminal_print_colored("  Too many failed attempts.\n", 0x0C);
    return false;
}

bool login_add_user(const char* user, const char* pass) {
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].active && strcmp(users[i].name, user) == 0)
            return false;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].active) {
            users[i].active = true;
            strncpy(users[i].name, user, USER_NAME_LEN - 1);
            users[i].pass_hash = hash_pass(pass);
            users[i].is_admin = false;
            return true;
        }
    }
    return false;
}

bool login_del_user(const char* user) {
    if (strcmp(user, "root") == 0) return false;
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].active && strcmp(users[i].name, user) == 0)
            { users[i].active = false; return true; }
    return false;
}

bool login_change_pass(const char* user, const char* old_pass, const char* new_pass) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active && strcmp(users[i].name, user) == 0) {
            if (users[i].pass_hash == hash_pass(old_pass)) {
                users[i].pass_hash = hash_pass(new_pass);
                return true;
            }
            return false;
        }
    }
    return false;
}

const char* login_current_user(void) { return current_user; }
void login_set_user(const char* user) { strncpy(current_user, user, USER_NAME_LEN - 1); logged_in = true; }
void login_logout(void) { current_user[0] = '\0'; logged_in = false; }
bool login_is_logged_in(void) { return logged_in; }

void login_list_users(void) {
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].active)
            kprintf("  %-12s %s\n", users[i].name, users[i].is_admin ? "(admin)" : "");
}
