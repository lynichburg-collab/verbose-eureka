/*
 * elevate — run one command as root, with your password, one at a time.
 *
 *   elevate <command> [args...]   run it as root
 *   elevate --stop                end the root command that is running
 *
 * Rules:
 *   - you must be in the admin group,
 *   - you must be at the machine's own keyboard, in the foreground. Remote
 *     sessions, terminal emulators and background programs are refused, so
 *     malware can neither use elevate nor trigger its wrong-password delays,
 *   - you type your own password every time (up to 3 tries per run);
 *     nothing is remembered between commands,
 *   - only ONE root command may run at a time, system-wide. A second
 *     elevate is refused until the first finishes (or `elevate --stop`),
 *   - no root shells: elevate won't start a shell or a command launcher,
 *     since everything typed into one would run as root without a password.
 * Every use, refusal, stop and wrong password goes into the activity log.
 *
 * The one-at-a-time rule is a lock (/run/elevate.lock) that elevate takes
 * and hands to the root command it starts: the kernel releases it when that
 * command, and anything it left running, has ended. Only root can open the
 * lock file, so a normal user can't hold it to block you.
 *
 * Installed setuid root. It trusts nothing from the environment.
 */
#include "common.c"

#define ROOT_LOCK "/run/elevate.lock"

/* Shells and launchers that would turn one password into a root shell. */
static const char *const no_shells[] = {
    "sh", "ash", "bash", "dash", "zsh", "ksh", "mksh", "fish", "busybox",
    "su", "login", "env", "chpst", "setsid", "nohup", "nice", "timeout",
    "xargs", "script", "elevate", NULL
};

static int lock_fd = -1;

static void describe(char *out, size_t len, int argc, char **argv)
{
    size_t used = 0;
    out[0] = '\0';
    for (int i = 1; i < argc && used < len - 1; i++) {
        int w = snprintf(out + used, len - used, "%s%s", i > 1 ? " " : "", argv[i]);
        if (w < 0)
            break;
        used += (size_t)w;
    }
}

static int is_shell(const char *cmd)
{
    const char *base = strrchr(cmd, '/') ? strrchr(cmd, '/') + 1 : cmd;
    for (int i = 0; no_shells[i]; i++)
        if (strcmp(base, no_shells[i]) == 0)
            return 1;
    return 0;
}

/* --------------------------------------------------------- the root lock */

struct holder {
    long pid;
    char user[64], tty[32], what[400];
    time_t since;
};

static void read_holder(int fd, struct holder *h)
{
    char buf[600];
    memset(h, 0, sizeof *h);
    ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    if (n <= 0)
        return;
    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    /* pid|user|tty|since|what */
    char *f[5] = { 0 }, *p = buf;
    for (int i = 0; i < 5 && p; i++) {
        f[i] = p;
        p = i < 4 ? strchr(p, '|') : NULL;
        if (p)
            *p++ = '\0';
    }
    if (f[0]) h->pid = strtol(f[0], NULL, 10);
    if (f[1]) snprintf(h->user, sizeof h->user, "%s", f[1]);
    if (f[2]) snprintf(h->tty, sizeof h->tty, "%s", f[2]);
    if (f[3]) h->since = (time_t)strtol(f[3], NULL, 10);
    if (f[4]) snprintf(h->what, sizeof h->what, "%s", f[4]);
}

static void write_holder(const char *user, const char *tty, const char *what)
{
    char line[600];
    int n = snprintf(line, sizeof line, "%ld|%s|%s|%ld|%s\n",
                     (long)getpid(), user, tty, (long)time(NULL), what);
    if (n < 0)
        return;
    if (ftruncate(lock_fd, 0) < 0 || pwrite(lock_fd, line, (size_t)n, 0) < 0) { }
}

/* Take the lock. 1 = ours; 0 = held by someone (*h describes them). The fd
 * is deliberately NOT close-on-exec: the root command inherits it. */
static int take_lock(struct holder *h)
{
    lock_fd = open(ROOT_LOCK, O_RDWR | O_CREAT, 0600);
    if (lock_fd < 0)
        return -1;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) == 0)
        return 1;
    read_holder(lock_fd, h);
    close(lock_fd);
    lock_fd = -1;
    return 0;
}

static void describe_holder(const struct holder *h, char *out, size_t len)
{
    char when[16] = "?";
    if (h->since)
        strftime(when, sizeof when, "%H:%M", localtime(&h->since));
    snprintf(out, len, "'%s' (started by %s on %s at %s)",
             h->what[0] ? h->what : "?", h->user[0] ? h->user : "?",
             h->tty[0] ? h->tty : "?", when);
}

/* Is anyone holding the lock right now? */
static int lock_is_held(void)
{
    int fd = open(ROOT_LOCK, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return 0;
    int held = flock(fd, LOCK_EX | LOCK_NB) != 0;
    if (!held)
        flock(fd, LOCK_UN);
    close(fd);
    return held;
}

/* End the running root command: politely, then firmly. */
static int stop_holder(const char *user, const char *tty)
{
    int fd = open(ROOT_LOCK, O_RDONLY | O_CLOEXEC);
    struct holder h;
    if (fd < 0)
        return 1;
    read_holder(fd, &h);
    close(fd);
    char desc[600];
    describe_holder(&h, desc, sizeof desc);

    pid_t pid = (pid_t)h.pid, pgid = pid > 1 ? getpgid(pid) : -1;
    /* Kill its whole job, unless that group is a shell's own session. */
    int whole_group = pgid > 1 && pgid != getsid(pid);
    if (pid > 1)
        whole_group ? kill(-pgid, SIGTERM) : kill(pid, SIGTERM);
    for (int i = 0; i < 30 && lock_is_held(); i++)
        usleep(100000);
    if (lock_is_held() && pid > 1) {
        whole_group ? kill(-pgid, SIGKILL) : kill(pid, SIGKILL);
        for (int i = 0; i < 20 && lock_is_held(); i++)
            usleep(100000);
    }
    if (lock_is_held()) {
        activity("elevate", "'%s' on %s could not fully stop %s", user, tty, desc);
        fprintf(stderr, "elevate: stopped %s, but something it started is still holding the\n"
                        "         root lock. Restarting the machine will clear it.\n", desc);
        return 1;
    }
    activity("elevate", "'%s' on %s stopped the root command %s", user, tty, desc);
    printf("Stopped %s.\n", desc);
    return 0;
}

/* --------------------------------------------------------- running it */

static void become_root_and_run(char **argv)
{
    if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0) {
        puts("elevate: couldn't switch to root");
        _exit(1);
    }
    const char *term = getenv("TERM");
    char term_copy[64];
    snprintf(term_copy, sizeof term_copy, "%s", term ? term : "linux");
    clearenv();
    setenv("PATH", SAFE_PATH, 1);
    setenv("HOME", "/users/root", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("SHELL", USER_SHELL, 1);
    setenv("TERM", term_copy, 1);
    execvp(argv[0], argv);
    fprintf(stderr, "elevate: %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

static void usage(void)
{
    fputs("usage: elevate <command> [args...]   run one command as root\n"
          "       elevate --stop                end the root command that is running\n"
          "Each root command needs your password, and only one can run at a time.\n",
          stderr);
}

int main(int argc, char **argv)
{
    char what[400], tty[32];
    describe(what, sizeof what, argc, argv);

    if (geteuid() != 0) {
        fputs("elevate: not installed correctly (needs setuid root)\n", stderr);
        return 1;
    }
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return 2;
    }
    if (getuid() == 0) {
        if (strcmp(argv[1], "--stop") == 0)
            return stop_holder("root", "system");
        become_root_and_run(argv + 1);              /* the system itself */
    }

    int stop = strcmp(argv[1], "--stop") == 0;
    struct passwd *pw = getpwuid(getuid());
    if (!pw) {
        fputs("elevate: unknown user\n", stderr);
        return 1;
    }
    char user[64];
    snprintf(user, sizeof user, "%s", pw->pw_name);

    if (!stop && is_shell(argv[1])) {
        activity("elevate", "refused '%s' (would start a shell): %s", user, what);
        fprintf(stderr, "elevate: won't run '%s'. It would start a shell or another command,\n"
                        "         and everything after that would run as root without asking.\n"
                        "         Run each root command with its own elevate.\n", argv[1]);
        return 2;
    }
    if (!in_group(user, ADMIN_GROUP)) {
        activity("elevate", "refused '%s' (not an admin): %s", user, what);
        fprintf(stderr, "elevate: %s isn't an admin.\n", user);
        return 1;
    }
    if (!at_local_keyboard(tty, sizeof tty)) {
        activity("elevate", "refused '%s' (not at the keyboard): %s", user, what);
        fputs("elevate: only works at this machine's own keyboard, in the foreground.\n"
              "         (Not from remote sessions, terminal windows or background programs.)\n",
              stderr);
        return 1;
    }

    if (stop) {
        if (!lock_is_held()) {
            puts("No root command is running.");
            return 0;
        }
    } else {
        struct holder h;
        int got = take_lock(&h);
        if (got < 0) {
            fputs("elevate: can't open the root lock\n", stderr);
            return 1;
        }
        if (got == 0) {
            char desc[600];
            describe_holder(&h, desc, sizeof desc);
            activity("elevate", "refused '%s' on %s (another root command is running: %s): %s",
                     user, tty, desc, what);
            fprintf(stderr, "elevate: only one root command at a time, and %s\n"
                            "         is still running. Wait for it, or end it with: elevate --stop\n",
                    desc);
            return 1;
        }
        /* Held while we ask for the password, so nobody can slip in. */
        write_holder(user, tty, "(waiting for a password)");
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        long left = wait_remaining(tty);
        if (left > 0) {
            char t[32];
            format_wait(left, t, sizeof t);
            fprintf(stderr, "elevate: too many wrong passwords on %s. Try again in %s,\n"
                            "         or from another terminal (Ctrl+Alt+F2...).\n", tty, t);
            return 1;
        }

        char prompt[96], password[256];
        snprintf(prompt, sizeof prompt, "elevate: password for %s: ", user);
        if (read_line(prompt, password, sizeof password, 1) < 0)
            return 1;
        int ok = check_password(user, password);
        memset(password, 0, sizeof password);

        if (ok) {
            note_success(tty);
            if (stop)
                return stop_holder(user, tty);
            write_holder(user, tty, what);
            activity("elevate", "'%s' on %s ran as root: %s", user, tty, what);
            become_root_and_run(argv + 1);
        }
        int n = note_failure(tty);
        activity("elevate", "wrong password for '%s' on %s (%d in a row here)", user, tty, n);
        sleep(2);
        fputs("Wrong password.\n", stderr);
    }
    return 1;
}
