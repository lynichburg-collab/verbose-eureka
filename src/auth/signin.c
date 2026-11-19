/*
 * signin — the sign-in prompt keeper starts on each terminal.
 *
 * First start: no account exists yet, so it walks you through creating
 * yours. After that: asks for a name and password, then starts your shell.
 * root can't sign in directly; sign in as yourself and use `elevate`.
 *
 * Wrong passwords make this terminal wait before the next try (see
 * common.c). Other terminals are unaffected: Ctrl+Alt+F2 etc.
 */
#include "common.c"

#define OS_NAME_FILE "/settings/about"

static char tty[32] = "console";

static void first_start(void)
{
    /* Every terminal shows this; setup runs on the one where Enter is
     * pressed first. Others wait for it to finish. */
    char enter[8];
    puts("\nWelcome to MTplatitudes. No account exists yet.");
    if (read_line("Press Enter to create yours. ", enter, sizeof enter, 0) < 0)
        _exit(1);
    if (have_person())
        return;

    int lock = open("/run/mtp-setup.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
        puts("\nAccount setup is running on another terminal...");
        flock(lock, LOCK_EX);
    }
    if (have_person()) {
        if (lock >= 0)
            close(lock);
        return;
    }

    puts("\nYour account will be an admin: you can run things as root with 'elevate'.\n");

    char name[64], pw[256];
    for (;;) {
        if (read_line("Pick a username (lowercase letters, digits, - or _): ",
                      name, sizeof name, 0) < 0)
            _exit(1);
        if (!valid_username(name) || strcmp(name, "root") == 0) {
            puts("That won't work as a username. Try something like 'noah'.");
            continue;
        }
        break;
    }
    if (ask_new_password(pw, sizeof pw) < 0)
        _exit(1);
    if (create_person(name, pw, 1) < 0) {
        puts("Couldn't create the account. Restarting setup.");
        sleep(2);
        _exit(1);
    }
    memset(pw, 0, sizeof pw);
    printf("\nAccount '%s' created. Sign in below.\n", name);
    if (lock >= 0)
        close(lock);
}

static void wait_out_delay(void)
{
    long left;
    int shown = 0;
    while ((left = wait_remaining(tty)) > 0) {
        char t[32];
        format_wait(left, t, sizeof t);
        printf("\rToo many wrong passwords on %s. Try again in %s "
               "(other terminals: Ctrl+Alt+F2...)   ", tty, t);
        fflush(stdout);
        shown = 1;
        sleep(1);
    }
    if (shown) {
        putchar('\n');
        /* Throw away anything typed during the wait (it was echoed, and
         * shouldn't count as an attempt). */
        tcflush(0, TCIFLUSH);
    }
}

static void start_session(struct passwd *pw)
{
    char tty_path[64];
    snprintf(tty_path, sizeof tty_path, "/dev/%s", tty);
    if (chown(tty_path, pw->pw_uid, 5) < 0 || chmod(tty_path, 0620) < 0) { }

    if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 ||
        setuid(pw->pw_uid) < 0) {
        puts("Couldn't switch to your account.");
        _exit(1);
    }

    clearenv();
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell, 1);
    setenv("PATH", SAFE_PATH, 1);
    setenv("TERM", "linux", 1);
    if (chdir(pw->pw_dir) < 0 && chdir("/") < 0) { }

    execl(pw->pw_shell, "-sh", (char *)NULL);
    perror("signin: shell");
    _exit(1);
}

int main(void)
{
    if (getuid() != 0) {
        fputs("signin: only keeper runs this\n", stderr);
        return 1;
    }
    if (!at_local_keyboard(tty, sizeof tty))
        snprintf(tty, sizeof tty, "console");

    if (!have_person())
        first_start();

    char name[64], pw[256], hostname[64] = "mtplatitudes";
    gethostname(hostname, sizeof hostname);

    for (;;) {
        printf("\n%s (%s)\n", hostname, tty);
        wait_out_delay();
        if (read_line("sign in as: ", name, sizeof name, 0) < 0)
            return 0;
        if (!name[0])
            continue;
        if (read_line("password: ", pw, sizeof pw, 1) < 0)
            return 0;

        if (strcmp(name, "root") == 0) {
            memset(pw, 0, sizeof pw);
            puts("root can't sign in directly. Sign in as yourself and use 'elevate'.");
            continue;
        }

        struct passwd *user = getpwnam(name);
        int ok = user && user->pw_uid != 0 && check_password(name, pw);
        memset(pw, 0, sizeof pw);

        if (!ok) {
            int n = note_failure(tty);
            if (user) {
                struct counter u = counter_read("user", name);
                u.failures++;
                u.last = time(NULL);
                counter_write("user", name, u);
            }
            activity("signin", "wrong password for '%s' on %s (%d in a row here)", name, tty, n);
            sleep(2);
            puts("Wrong name or password.");
            continue;
        }

        note_success(tty);
        struct counter u = counter_read("user", name);
        if (u.failures > 0) {
            char when[32];
            strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&u.last));
            printf("\n\033[1;33m%d failed sign-in attempt%s for '%s' since your last sign-in "
                   "(latest %s). Details: activity | grep signin\033[0m\n",
                   u.failures, u.failures == 1 ? "" : "s", name, when);
        }
        counter_write("user", name, (struct counter){ 0, 0 });
        activity("signin", "'%s' signed in on %s", name, tty);
        start_session(user);
    }
}
