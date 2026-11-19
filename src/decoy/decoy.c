/*
 * decoy — a tripwire disguised as a common program.
 *
 * MTplatitudes keeps its real programs in /core/cmd. Scripts written for an
 * ordinary Linux system look in /bin and /usr/bin instead, so copies of this
 * program are placed there under familiar names (bash, python3, curl...).
 * Nothing legitimate on this system ever runs them.
 *
 * When one runs, it pretends to work while doing nothing. The run itself is
 * logged to /var/log/tripwire by recorder (as root, so whoever ran the decoy
 * can't hide it). The decoy adds detail it alone can see, such as commands
 * typed into the fake shell, when the log is writable.
 *
 * `tripwires` shows the log; the login shell warns when it isn't empty.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define LOG_FILE "/var/log/tripwire"

static char self[256];      /* full path we were run as, e.g. /usr/bin/curl */
static const char *name;    /* basename, e.g. curl */

static void read_cmdline(pid_t pid, char *buf, size_t len)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0)
        return;
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == '\0')
            buf[i] = ' ';
    buf[n] = '\0';
    while (n > 0 && buf[n - 1] == ' ')
        buf[--n] = '\0';
}

static void note(const char *fmt, const char *detail)
{
    char stamp[32], parent[256], line[1024];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));
    read_cmdline(getppid(), parent, sizeof parent);

    int n = snprintf(line, sizeof line, "%s  %s  uid=%d  parent=%d (%s)  ",
                     stamp, self, (int)getuid(), (int)getppid(),
                     parent[0] ? parent : "?");
    if (n < 0 || (size_t)n >= sizeof line)
        n = sizeof line - 1;
    snprintf(line + n, sizeof line - n, fmt, detail);

    int fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CLOEXEC);   /* never create it */
    if (fd < 0)
        return;
    dprintf(fd, "%s\n", line);
    close(fd);
}

static int has_arg(int argc, char **argv, const char *a)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], a) == 0)
            return 1;
    return 0;
}

static void print_version(void)
{
    if (!strcmp(name, "bash"))
        puts("GNU bash, version 5.2.21(1)-release (x86_64-pc-linux-gnu)");
    else if (!strcmp(name, "python3"))
        puts("Python 3.12.3");
    else if (!strcmp(name, "perl"))
        puts("\nThis is perl 5, version 38, subversion 2 (v5.38.2) built for x86_64-linux-gnu-thread-multi");
    else if (!strcmp(name, "curl"))
        puts("curl 8.5.0 (x86_64-pc-linux-gnu) libcurl/8.5.0 OpenSSL/3.0.13 zlib/1.3");
    else if (!strcmp(name, "wget"))
        puts("GNU Wget 1.21.4 built on linux-gnu.");
    else if (!strcmp(name, "sudo"))
        puts("Sudo version 1.9.15p5");
    else if (!strcmp(name, "env"))
        puts("env (GNU coreutils) 9.4");
}

/* A shell that accepts every command and does nothing. Each line typed is
 * logged; that is the whole point of letting someone in. */
static int fake_shell(void)
{
    char line[512];
    const char *prompt = getuid() == 0 ? "bash-5.2# " : "bash-5.2$ ";
    for (;;) {
        fputs(prompt, stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0')
            continue;
        note("typed: %s", line);
        if (!strcmp(line, "exit") || !strcmp(line, "logout"))
            break;
        if (!strcmp(line, "whoami"))
            puts(getuid() == 0 ? "root" : "user");
        else if (!strcmp(line, "id"))
            puts(getuid() == 0 ? "uid=0(root) gid=0(root) groups=0(root)"
                               : "uid=1000(user) gid=1000(user) groups=1000(user)");
        else if (!strcmp(line, "pwd"))
            puts("/root");
    }
    return 0;
}

/* sudo that always rejects the password. The password itself is never
 * logged: if you typed your real one by mistake, it goes nowhere. */
static int fake_sudo(void)
{
    struct termios old, quiet;
    int tty = isatty(0) && tcgetattr(0, &old) == 0;
    char buf[256];

    for (int attempt = 0; attempt < 3; attempt++) {
        fputs("[sudo] password for root: ", stdout);
        fflush(stdout);
        if (tty) {
            quiet = old;
            quiet.c_lflag &= ~(tcflag_t)ECHO;
            tcsetattr(0, TCSANOW, &quiet);
        }
        char *got = fgets(buf, sizeof buf, stdin);
        if (tty)
            tcsetattr(0, TCSANOW, &old);
        putchar('\n');
        if (!got)
            break;
        note("%s", "typed a password into the fake sudo (not recorded)");
        sleep(2);
        if (attempt < 2)
            puts("Sorry, try again.");
    }
    puts("sudo: 3 incorrect password attempts");
    return 1;
}

int main(int argc, char **argv)
{
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n > 0)
        self[n] = '\0';
    else
        snprintf(self, sizeof self, "%s", argv[0] ? argv[0] : "?");
    name = strrchr(self, '/') ? strrchr(self, '/') + 1 : self;

    if (has_arg(argc, argv, "--version") || has_arg(argc, argv, "-V")) {
        print_version();
        return 0;
    }
    if (!strcmp(name, "sudo"))
        return fake_sudo();
    if (!strcmp(name, "bash") && argc == 1 && isatty(0))
        return fake_shell();

    /* Everything else: a script, a download, a one-liner. Accept it, say
     * nothing, report success. Drain piped input so writers don't error. */
    if (!isatty(0)) {
        char sink[4096];
        while (read(0, sink, sizeof sink) > 0)
            ;
    }
    return 0;
}
