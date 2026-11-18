/*
 * keeper — process 1 for MTplatitudes.
 *
 * The kernel starts this program first, and it runs until the machine turns
 * off. It:
 *   1. mounts the kernel filesystems (/proc, /sys, /dev, /run, /tmp),
 *   2. runs the boot script /core/boot/startup (policy lives there,
 *      so changing what happens at boot does not need a recompile),
 *   3. starts a program on each terminal listed in /settings/terminals and
 *      restarts it when it exits,
 *   4. reaps orphaned processes,
 *   5. shuts the machine down when asked:
 *        SIGUSR1 = halt, SIGUSR2 = power off, SIGTERM/SIGINT = reboot
 *      (the same signals BusyBox's halt/poweroff/reboot send to PID 1;
 *       SIGINT is what the kernel sends on Ctrl-Alt-Del).
 *
 * Signals are blocked and handled synchronously with sigtimedwait(), so there
 * is no async signal-handler state to get wrong.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHELL        "/core/cmd/sh"
#define STARTUP      "/core/boot/startup"
#define SHUTDOWN     "/core/boot/shutdown"
#define TERMINALS     "/settings/terminals"
#define MAX_TERMINALS 8
#define MAX_ARGS     16

struct terminal {
    char tty[32];
    char line[256];
    char *argv[MAX_ARGS];
    pid_t pid;
    time_t started;
    time_t next_start;
    int quick_exits;
};

static struct terminal terminals[MAX_TERMINALS];
static int n_terminals;
static sigset_t handled;

static char *const child_env[] = {
    "PATH=/core/cmd:/bin:/usr/bin",       /* decoys last: see src/decoy */
    "HOME=/users/root",
    "TERM=linux",
    "SHELL=" SHELL,
    NULL,
};

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("keeper: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Add a line to the activity log (same format as `recorder note`). */
static void activity(const char *kind, const char *fmt, ...)
{
    char stamp[32], text[256];
    time_t now = time(NULL);
    va_list ap;

    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    int fd = open("/var/log/activity", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    dprintf(fd, "%s  %-7s %s\n", stamp, kind, text);
    close(fd);
}

static void mount_fs(const char *src, const char *dst, const char *type,
                     unsigned long flags, const char *opts)
{
    mkdir(dst, 0755);
    if (mount(src, dst, type, flags, opts) != 0 && errno != EBUSY)
        say("mount %s on %s failed: %s", type, dst, strerror(errno));
}

static void mount_kernel_filesystems(void)
{
    const unsigned long safe = MS_NOSUID | MS_NODEV | MS_NOEXEC;

    mount_fs("proc", "/proc", "proc", safe, NULL);
    mount_fs("sysfs", "/sys", "sysfs", safe, NULL);
    mount_fs("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    mount_fs("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC,
             "gid=5,mode=0620,ptmxmode=0666");
    mount_fs("shm", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    /* Nothing may run from these: downloads and scratch files land here. */
    mount_fs("run", "/run", "tmpfs", safe, "mode=0755");
    mount_fs("tmp", "/tmp", "tmpfs", safe, "mode=1777");
}

/* Make sure fds 0, 1 and 2 point at the system console. */
static void open_console(void)
{
    int fd = open("/dev/console", O_RDWR | O_NOCTTY);
    if (fd < 0)
        return;
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2)
        close(fd);
}

/* Prepare a freshly forked child: default signal state, own session. */
static void child_setup(void)
{
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    setsid();
}

/* Run a script to completion, reaping anything else that exits meanwhile. */
static void run_script(const char *path)
{
    if (access(path, R_OK) != 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        child_setup();
        execle(SHELL, "sh", path, (char *)NULL, child_env);
        say("cannot run %s: %s", path, strerror(errno));
        _exit(127);
    }
    if (pid < 0) {
        say("fork failed: %s", strerror(errno));
        return;
    }
    for (;;) {
        int status;
        pid_t done = waitpid(-1, &status, 0);
        if (done == pid || (done < 0 && errno == ECHILD))
            break;
    }
}

static void load_terminals(void)
{
    FILE *f = fopen(TERMINALS, "r");
    char buf[256];

    n_terminals = 0;
    if (!f) {
        /* No settings file: a shell on the kernel console, so the system is usable. */
        struct terminal *c = &terminals[n_terminals++];
        snprintf(c->tty, sizeof c->tty, "console");
        snprintf(c->line, sizeof c->line, SHELL " -l");
    } else {
        while (n_terminals < MAX_TERMINALS && fgets(buf, sizeof buf, f)) {
            char *p = buf + strspn(buf, " \t");
            p[strcspn(p, "\r\n")] = '\0';
            if (*p == '\0' || *p == '#')
                continue;
            struct terminal *c = &terminals[n_terminals];
            char *rest = p + strcspn(p, " \t");
            if (*rest == '\0')
                continue;                       /* tty with no command */
            *rest++ = '\0';
            rest += strspn(rest, " \t");
            snprintf(c->tty, sizeof c->tty, "%s", p);
            snprintf(c->line, sizeof c->line, "%s", rest);
            n_terminals++;
        }
        fclose(f);
    }

    for (int i = 0; i < n_terminals; i++) {
        struct terminal *c = &terminals[i];
        int argc = 0;
        for (char *tok = strtok(c->line, " \t"); tok && argc < MAX_ARGS - 1;
             tok = strtok(NULL, " \t"))
            c->argv[argc++] = tok;
        c->argv[argc] = NULL;
        c->pid = 0;
        c->next_start = 0;
    }
}

static void start_terminal(struct terminal *c)
{
    char dev[64];
    snprintf(dev, sizeof dev, "/dev/%.31s", c->tty);

    if (access(dev, F_OK) != 0) {
        c->next_start = time(NULL) + 30;        /* device not present (yet) */
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        child_setup();
        int fd = open(dev, O_RDWR);
        if (fd < 0)
            _exit(126);
        ioctl(fd, TIOCSCTTY, 1);
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
        execve(c->argv[0], c->argv, child_env);
        _exit(127);
    }
    if (pid > 0) {
        c->pid = pid;
        c->started = time(NULL);
        activity("session", "%s started on %s (pid %d)", c->argv[0], c->tty, (int)pid);
    } else {
        c->next_start = time(NULL) + 5;
    }
}

static void child_exited(pid_t pid)
{
    for (int i = 0; i < n_terminals; i++) {
        struct terminal *c = &terminals[i];
        if (c->pid != pid)
            continue;
        c->pid = 0;
        activity("session", "%s on %s ended (pid %d)", c->argv[0], c->tty, (int)pid);
        time_t now = time(NULL);
        /* Back off if the program keeps dying right away. */
        if (now - c->started < 2) {
            if (c->quick_exits < 15)
                c->quick_exits++;
            c->next_start = now + 2 * c->quick_exits;
        } else {
            c->quick_exits = 0;
            c->next_start = now;
        }
    }
}

static void reap(void)
{
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        child_exited(pid);
}

static void shutdown_system(unsigned int how)
{
    const char *what = how == RB_POWER_OFF ? "power off"
                     : how == RB_HALT_SYSTEM ? "halt" : "reboot";
    say("system going down (%s)", what);
    activity("boot", "system going down (%s)", what);

    run_script(SHUTDOWN);

    kill(-1, SIGTERM);
    for (int i = 0; i < 20; i++) {              /* up to 2 s to exit cleanly */
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        if (kill(-1, 0) != 0 && errno == ESRCH)
            break;
        usleep(100000);
    }
    kill(-1, SIGKILL);
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    sync();
    /* Once / is a real disk, make it read-only so nothing is half-written. */
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
    sync();
    reboot(how);
    for (;;)
        pause();
}

static void handle_signal(int sig)
{
    switch (sig) {
    case SIGCHLD: reap(); break;
    case SIGUSR1: shutdown_system(RB_HALT_SYSTEM); break;
    case SIGUSR2: shutdown_system(RB_POWER_OFF); break;
    case SIGTERM:
    case SIGINT:  shutdown_system(RB_AUTOBOOT); break;
    case SIGHUP:  break;                        /* ignored; PID 1 has no terminal */
    }
}

int main(void)
{
    if (getpid() != 1) {
        fprintf(stderr, "keeper: must run as process 1\n");
        return 1;
    }

    sigemptyset(&handled);
    sigaddset(&handled, SIGCHLD);
    sigaddset(&handled, SIGUSR1);
    sigaddset(&handled, SIGUSR2);
    sigaddset(&handled, SIGTERM);
    sigaddset(&handled, SIGINT);
    sigaddset(&handled, SIGHUP);
    sigprocmask(SIG_BLOCK, &handled, NULL);

    reboot(RB_DISABLE_CAD);                     /* Ctrl-Alt-Del -> SIGINT */
    umask(022);
    mount_kernel_filesystems();
    open_console();

    run_script(STARTUP);
    load_terminals();

    for (;;) {
        siginfo_t info;
        struct timespec poll = {0, 0}, tick = {1, 0};
        int sig;

        /* Handle everything already pending before starting terminals, so a
         * shutdown requested during boot happens immediately. */
        while ((sig = sigtimedwait(&handled, &info, &poll)) > 0)
            handle_signal(sig);
        reap();

        time_t now = time(NULL);
        for (int i = 0; i < n_terminals; i++)
            if (terminals[i].pid == 0 && now >= terminals[i].next_start)
                start_terminal(&terminals[i]);

        /* Sleep until a signal arrives or a second passes (for restarts
         * that are waiting out a back-off delay). */
        sig = sigtimedwait(&handled, &info, &tick);
        if (sig > 0)
            handle_signal(sig);
    }
}
