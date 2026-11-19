/*
 * elevate — run one command (or a shell) as root.
 *
 *   elevate <command> [args...]
 *   elevate                       root shell (type 'exit' to leave)
 *
 * Rules:
 *   - you must be in the admin group,
 *   - you must be at the machine's own keyboard, in the foreground. Remote
 *     sessions, terminal emulators and background programs are refused, so
 *     malware can neither use elevate nor trigger its wrong-password delays,
 *   - you type your own password (up to 3 tries per run).
 * Every use, refusal and wrong password goes into the activity log.
 *
 * Installed setuid root. It trusts nothing from the environment.
 */
#include "common.c"

static void describe(char *out, size_t len, int argc, char **argv)
{
    size_t used = 0;
    out[0] = '\0';
    if (argc < 2) {
        snprintf(out, len, "(root shell)");
        return;
    }
    for (int i = 1; i < argc && used < len - 1; i++) {
        int w = snprintf(out + used, len - used, "%s%s", i > 1 ? " " : "", argv[i]);
        if (w < 0)
            break;
        used += (size_t)w;
    }
}

static void become_root_and_run(int argc, char **argv)
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

    if (argc < 2) {
        setenv("PS1", "\\[\\e[1;31m\\]root@\\h\\[\\e[0m\\]:\\w# ", 1);
        execl(USER_SHELL, "sh", (char *)NULL);
    } else {
        execvp(argv[1], argv + 1);
    }
    fprintf(stderr, "elevate: %s: %s\n", argc < 2 ? USER_SHELL : argv[1], strerror(errno));
    _exit(127);
}

int main(int argc, char **argv)
{
    char what[400], tty[32];
    describe(what, sizeof what, argc, argv);

    if (geteuid() != 0) {
        fputs("elevate: not installed correctly (needs setuid root)\n", stderr);
        return 1;
    }
    uid_t uid = getuid();
    if (uid == 0)
        become_root_and_run(argc, argv);        /* already root */

    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        fputs("elevate: unknown user\n", stderr);
        return 1;
    }
    char user[64];
    snprintf(user, sizeof user, "%s", pw->pw_name);

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
            activity("elevate", "'%s' on %s ran as root: %s", user, tty, what);
            become_root_and_run(argc, argv);
        }
        int n = note_failure(tty);
        activity("elevate", "wrong password for '%s' on %s (%d in a row here)", user, tty, n);
        sleep(2);
        fputs("Wrong password.\n", stderr);
    }
    return 1;
}
