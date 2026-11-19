/*
 * Shared code for signin, elevate and accounts.
 *
 * Passwords: SHA-512 crypt with many rounds, stored in /settings/shadow
 * (readable by root only).
 *
 * Wrong-password delays are counted per terminal, so nobody can lock you out
 * of every terminal at once. Consecutive failures on one terminal:
 *
 *   1-3: no wait (typos)   4: 30 s   5: 1 min   6: 2 min   7: 5 min
 *   8: 15 min   9: 30 min   10 and up: 1 hour (stays there)
 *
 * A correct password resets the count. Counts live in /var/lib/mtp-auth.
 */
#define _GNU_SOURCE
/* Each program uses a different subset of these helpers. */
#pragma GCC diagnostic ignored "-Wunused-function"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PASSWD_FILE  "/settings/passwd"
#define GROUP_FILE   "/settings/group"
#define SHADOW_FILE  "/settings/shadow"
#define STATE_DIR    "/var/lib/mtp-auth"
#define ACTIVITY_LOG "/var/log/activity"
#define ADMIN_GROUP  "admin"
#define USER_SHELL   "/core/cmd/sh"
#define SAFE_PATH    "/core/cmd:/bin:/usr/bin"
#define MIN_PASSWORD 10

/* ------------------------------------------------------------ activity */

static void activity(const char *kind, const char *fmt, ...)
{
    char stamp[32], text[512];
    time_t now = time(NULL);
    va_list ap;

    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    int fd = open(ACTIVITY_LOG, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    dprintf(fd, "%s  %-7s %s\n", stamp, kind, text);
    close(fd);
}

/* ------------------------------------------------------------ terminals */

/* Is this process running in the foreground of one of the machine's own
 * terminals (the screen and keyboard, or a serial cable)? Checked on fd 0:
 * it must be our session's controlling terminal, we must be its foreground
 * process group, and it must be a real terminal device. Pseudo-terminals
 * (remote sessions, terminal emulators, programs faking a terminal) and
 * background processes don't count. On success, *name is e.g. "tty1". */
static int at_local_keyboard(char *name, size_t len)
{
    if (!isatty(0))
        return 0;
    pid_t sid = getsid(0);
    if (sid < 0 || tcgetsid(0) != sid || tcgetpgrp(0) != getpgrp())
        return 0;
    const char *path = ttyname(0);
    if (!path || strncmp(path, "/dev/", 5) != 0)
        return 0;
    const char *n = path + 5, *digits = NULL;
    if (strncmp(n, "ttyS", 4) == 0)
        digits = n + 4;
    else if (strncmp(n, "tty", 3) == 0)
        digits = n + 3;
    if (!digits || !*digits)
        return 0;
    for (const char *d = digits; *d; d++)
        if (!isdigit((unsigned char)*d))
            return 0;
    snprintf(name, len, "%s", n);
    return 1;
}

/* ------------------------------------------------------------ input */

static struct termios saved_tty;
static int tty_saved;

static void restore_echo(void)
{
    if (tty_saved)
        tcsetattr(0, TCSANOW, &saved_tty);
}

static void on_interrupt(int sig)
{
    (void)sig;
    restore_echo();
    if (write(1, "\n", 1) < 0) { }
    _exit(130);
}

static int read_line(const char *prompt, char *buf, size_t len, int hidden)
{
    fputs(prompt, stdout);
    fflush(stdout);
    if (hidden && isatty(0) && tcgetattr(0, &saved_tty) == 0) {
        struct termios quiet = saved_tty;
        quiet.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
        tty_saved = 1;
        signal(SIGINT, on_interrupt);
        tcsetattr(0, TCSANOW, &quiet);
    }
    char *got = fgets(buf, (int)len, stdin);
    if (hidden) {
        restore_echo();
        tty_saved = 0;
        if (isatty(0))
            putchar('\n');
    }
    if (!got)
        return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

/* ------------------------------------------------------------ passwords */

static int check_password(const char *user, const char *password)
{
    struct spwd *sp = getspnam(user);
    if (!sp || !sp->sp_pwdp || sp->sp_pwdp[0] != '$')
        return 0;                           /* no hash, or locked ("!") */
    const char *got = crypt(password, sp->sp_pwdp);
    return got && strcmp(got, sp->sp_pwdp) == 0;
}

static char *hash_password(const char *password)
{
    static const char chars[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char rnd[16];
    char salt[64];
    if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd)
        return NULL;
    int n = snprintf(salt, sizeof salt, "$6$rounds=200000$");
    for (size_t i = 0; i < sizeof rnd; i++)
        salt[n++] = chars[rnd[i] % 64];
    salt[n++] = '$';
    salt[n] = '\0';
    const char *h = crypt(password, salt);
    return h ? strdup(h) : NULL;
}

static int in_group(const char *user, const char *group)
{
    struct group *g = getgrnam(group);
    if (!g)
        return 0;
    for (char **m = g->gr_mem; m && *m; m++)
        if (strcmp(*m, user) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------ delays */

static long delay_after(int failures)
{
    static const long table[] = { 0, 0, 0, 0, 30, 60, 120, 300, 900, 1800 };
    if (failures < (int)(sizeof table / sizeof table[0]))
        return table[failures];
    return 3600;                            /* capped at one hour */
}

struct counter {
    int failures;
    time_t last;
};

static void counter_path(char *out, size_t len, const char *kind, const char *who)
{
    snprintf(out, len, STATE_DIR "/%s-%s", kind, who);
}

static struct counter counter_read(const char *kind, const char *who)
{
    struct counter c = { 0, 0 };
    char path[128];
    counter_path(path, sizeof path, kind, who);
    FILE *f = fopen(path, "r");
    if (f) {
        long last = 0;
        if (fscanf(f, "%d %ld", &c.failures, &last) == 2)
            c.last = (time_t)last;
        else
            c.failures = 0;
        fclose(f);
    }
    return c;
}

static void counter_write(const char *kind, const char *who, struct counter c)
{
    char path[128];
    mkdir("/var/lib", 0755);
    mkdir(STATE_DIR, 0700);
    counter_path(path, sizeof path, kind, who);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%d %ld\n", c.failures, (long)c.last);
    fclose(f);
}

/* Seconds until this terminal may try again (0 = now). */
static long wait_remaining(const char *tty)
{
    struct counter c = counter_read("tty", tty);
    long until = (long)c.last + delay_after(c.failures);
    long left = until - (long)time(NULL);
    return left > 0 ? left : 0;
}

static int note_failure(const char *tty)
{
    struct counter c = counter_read("tty", tty);
    c.failures++;
    c.last = time(NULL);
    counter_write("tty", tty, c);
    return c.failures;
}

static void note_success(const char *tty)
{
    struct counter c = { 0, 0 };
    counter_write("tty", tty, c);
}

static void format_wait(long secs, char *out, size_t len)
{
    if (secs >= 60)
        snprintf(out, len, "%ld:%02ld", secs / 60, secs % 60);
    else
        snprintf(out, len, "%ld s", secs);
}

/* ------------------------------------------------------------ accounts */

static int valid_username(const char *name)
{
    size_t n = strlen(name);
    if (n < 1 || n > 32 || !(islower((unsigned char)name[0]) || name[0] == '_'))
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!(islower((unsigned char)name[i]) || isdigit((unsigned char)name[i]) ||
              name[i] == '_' || name[i] == '-'))
            return 0;
    return 1;
}

static int have_person(void)
{
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) != NULL)
        if (pw->pw_uid >= 1000 && pw->pw_uid < 60000) {
            endpwent();
            return 1;
        }
    endpwent();
    return 0;
}

/* Rewrite a settings file: copy every line for which keep() returns true,
 * then append extra. Written to a temporary file and renamed into place. */
static int rewrite_file(const char *path, mode_t mode,
                        int (*keep)(const char *line, void *ctx), void *ctx,
                        const char *extra)
{
    char tmp[128], line[1024];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *in = fopen(path, "r");
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) {
        if (in)
            fclose(in);
        return -1;
    }
    FILE *out = fdopen(fd, "w");
    if (in) {
        while (fgets(line, sizeof line, in))
            if (!keep || keep(line, ctx))
                fputs(line, out);
        fclose(in);
    }
    if (extra)
        fputs(extra, out);
    fflush(out);
    fsync(fd);
    fclose(out);
    chmod(tmp, mode);
    return rename(tmp, path);
}

static int keep_none(const char *line, void *ctx)
{
    (void)line; (void)ctx;
    return 0;
}

static int line_not_user(const char *line, void *ctx)
{
    const char *user = ctx;
    size_t n = strlen(user);
    return !(strncmp(line, user, n) == 0 && line[n] == ':');
}

static int set_password(const char *user, const char *password)
{
    char *hash = hash_password(password);
    if (!hash)
        return -1;
    char entry[512];
    snprintf(entry, sizeof entry, "%s:%s:%ld:0:99999:7:::\n", user, hash,
             (long)(time(NULL) / 86400));
    free(hash);
    return rewrite_file(SHADOW_FILE, 0600, line_not_user, (void *)user, entry);
}

static int create_person(const char *user, const char *password, int admin)
{
    if (!valid_username(user) || getpwnam(user))
        return -1;

    /* Next free id from 1000. */
    unsigned id = 1000;
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) != NULL)
        if (pw->pw_uid >= id && pw->pw_uid < 60000)
            id = pw->pw_uid + 1;
    endpwent();

    char home[128], entry[512];
    snprintf(home, sizeof home, "/users/%s", user);

    snprintf(entry, sizeof entry, "%s:x:%u:%u::%s:%s\n", user, id, id, home, USER_SHELL);
    if (rewrite_file(PASSWD_FILE, 0644, NULL, NULL, entry) < 0)
        return -1;

    /* Own group, and membership of the admin group if asked. */
    char rebuilt[8192], line[1024];
    size_t rlen = 0;
    int found_admin = 0;
    FILE *g = fopen(GROUP_FILE, "r");
    if (g) {
        while (fgets(line, sizeof line, g) && rlen < sizeof rebuilt - 1) {
            line[strcspn(line, "\n")] = '\0';
            if (admin && strncmp(line, ADMIN_GROUP ":", strlen(ADMIN_GROUP) + 1) == 0) {
                found_admin = 1;
                size_t l = strlen(line);
                const char *sep = (l && line[l - 1] == ':') ? "" : ",";
                rlen += (size_t)snprintf(rebuilt + rlen, sizeof rebuilt - rlen,
                                         "%s%s%s\n", line, sep, user);
            } else {
                rlen += (size_t)snprintf(rebuilt + rlen, sizeof rebuilt - rlen, "%s\n", line);
            }
        }
        fclose(g);
    }
    if (rlen < sizeof rebuilt - 1)
        rlen += (size_t)snprintf(rebuilt + rlen, sizeof rebuilt - rlen, "%s:x:%u:\n", user, id);
    if (admin && !found_admin && rlen < sizeof rebuilt - 1)
        snprintf(rebuilt + rlen, sizeof rebuilt - rlen, ADMIN_GROUP ":x:10:%s\n", user);
    if (rewrite_file(GROUP_FILE, 0644, keep_none, NULL, rebuilt) < 0)
        return -1;

    if (set_password(user, password) < 0)
        return -1;

    mkdir("/users", 0755);
    mkdir(home, 0700);
    if (chown(home, id, id) < 0)
        return -1;
    activity("account", "created account '%s'%s", user, admin ? " (admin)" : "");
    return 0;
}

/* Ask for a new password twice. Returns 0 and fills buf when they match. */
static int ask_new_password(char *buf, size_t len)
{
    char again[256];
    for (;;) {
        if (read_line("Choose a password: ", buf, len, 1) < 0)
            return -1;
        if (strlen(buf) < MIN_PASSWORD) {
            printf("Use at least %d characters. A few unrelated words works well.\n",
                   MIN_PASSWORD);
            continue;
        }
        if (read_line("Type it again: ", again, sizeof again, 1) < 0)
            return -1;
        if (strcmp(buf, again) == 0) {
            memset(again, 0, sizeof again);
            return 0;
        }
        puts("Those didn't match. Let's try again.");
    }
}
