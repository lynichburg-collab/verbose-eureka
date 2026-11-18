/*
 * recorder — writes the MTplatitudes activity log, /var/log/activity.
 *
 *   recorder                     run as the log daemon (started by startup)
 *   recorder note <kind> <text>  add a line yourself (used by scripts)
 *
 * As a daemon it asks the kernel's audit system to report, for every process:
 *   - programs being run (execve), including failed and blocked attempts,
 *   - outgoing network connections (connect),
 *   - programs claiming a network port or waiting for incoming connections
 *     (bind, listen).
 * Each event becomes one line:
 *
 *   2026-11-18 16:10:02  run   uid=0 pid=812 parent=727  ls -la
 *   2026-11-18 16:10:05  net   uid=0 pid=901 (wget)  out -> 93.184.216.34:80
 *
 * Other parts of the OS add "boot", "session" and "device" lines with
 * `recorder note`. View the log with `activity`.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/netlink.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_FILE    "/var/log/activity"
#define LOG_OLD     "/var/log/activity.1"
#define LOG_MAX     (4 * 1024 * 1024)       /* rotate after 4 MB */
#define SELF        "/core/cmd/recorder"
#define BUSYBOX     "/core/cmd/busybox"
#define MAX_RECORD  8970    /* the kernel's MAX_AUDIT_MESSAGE_LENGTH */

/* ---------------------------------------------------------------- log file */

static void write_line(const char *kind, const char *fmt, ...)
{
    char stamp[32], text[1024], line[1100];
    time_t now = time(NULL);
    va_list ap;

    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof line, "%s  %-7s %s\n", stamp, kind, text);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = sizeof line - 1;

    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > LOG_MAX)
        rename(LOG_FILE, LOG_OLD);
    mkdir("/var/log", 0755);
    int fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    if (write(fd, line, (size_t)n) < 0) { /* nothing more we can do */ }
    close(fd);
}

/* ------------------------------------------------------- audit record text */

/* Find "key=" at the start of rec or after a space. Returns the value start. */
static const char *find_key(const char *rec, const char *key)
{
    size_t klen = strlen(key);
    for (const char *p = rec; (p = strstr(p, key)) != NULL; p += klen)
        if ((p == rec || p[-1] == ' ') && p[klen] == '=')
            return p + klen + 1;
    return NULL;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Copy a value. "quoted" values are copied as is. Unquoted values are copied
 * raw, or hex-decoded when decode is set (the kernel hex-encodes text that
 * contains spaces or unusual characters). */
static void get_value(const char *rec, const char *key, char *out, size_t len, int decode)
{
    const char *v = find_key(rec, key);
    size_t o = 0;
    out[0] = '\0';
    if (!v || len == 0)
        return;
    if (*v == '"') {
        v++;
        while (*v && *v != '"' && o < len - 1)
            out[o++] = *v++;
    } else {
        const char *e = v + strcspn(v, " ");
        int is_hex = decode && e > v && (e - v) % 2 == 0;
        for (const char *q = v; q < e && is_hex; q++)
            if (hexval(*q) < 0)
                is_hex = 0;
        if (is_hex) {
            for (const char *q = v; q + 1 < e && o < len - 1; q += 2) {
                char c = (char)(hexval(q[0]) * 16 + hexval(q[1]));
                out[o++] = (c >= 32 && c < 127) ? c : '?';
            }
        } else {
            while (v < e && o < len - 1)
                out[o++] = *v++;
        }
    }
    out[o] = '\0';
}

static long get_num(const char *rec, const char *key)
{
    const char *v = find_key(rec, key);
    return v ? strtol(v, NULL, 10) : -1;
}

/* --------------------------------------------------------------- events */

struct event {
    unsigned long serial;
    int used;
    long syscall, exit_code, pid, ppid, uid;
    int success, is32;
    char comm[64], exe[256], args[700], saddr[128], target[256];
};

#define SLOTS 16
static struct event events[SLOTS];

static struct event *slot_for(unsigned long serial)
{
    struct event *free_slot = NULL;
    for (int i = 0; i < SLOTS; i++) {
        if (events[i].used && events[i].serial == serial)
            return &events[i];
        if (!events[i].used && !free_slot)
            free_slot = &events[i];
    }
    if (!free_slot)                 /* table full: drop the oldest slot */
        free_slot = &events[serial % SLOTS];
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = 1;
    free_slot->serial = serial;
    free_slot->syscall = -1;
    return free_slot;
}

/* Turn the hex sockaddr into "1.2.3.4:80" or "[::1]:80". Returns 0 for
 * anything that isn't an internet address (local sockets, netlink...). */
static int decode_addr(const char *hex, char *out, size_t len)
{
    unsigned char b[64];
    size_t n = strlen(hex) / 2;
    if (n > sizeof b)
        n = sizeof b;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        b[i] = (unsigned char)(hi * 16 + lo);
    }
    if (n < 2)
        return 0;
    int family = b[0] | b[1] << 8;
    if (family == AF_INET && n >= 8) {
        snprintf(out, len, "%u.%u.%u.%u:%u", b[4], b[5], b[6], b[7], b[2] << 8 | b[3]);
        return 1;
    }
    if (family == AF_INET6 && n >= 24) {
        char *o = out;
        size_t left = len;
        int w = snprintf(o, left, "[");
        o += w; left -= (size_t)w;
        for (int i = 0; i < 8 && left > 6; i++) {
            w = snprintf(o, left, i ? ":%x" : "%x", b[8 + 2 * i] << 8 | b[9 + 2 * i]);
            o += w; left -= (size_t)w;
        }
        snprintf(o, left, "]:%u", b[2] << 8 | b[3]);
        return 1;
    }
    return 0;
}

static const char *base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Syscall numbers differ between 64-bit and 32-bit programs. */
static int is_exec(const struct event *e)
{
    return e->is32 ? (e->syscall == 11 || e->syscall == 358)
                   : (e->syscall == 59 || e->syscall == 322);
}
static int is_connect(const struct event *e) { return !e->is32 && e->syscall == 42; }
static int is_bind(const struct event *e)    { return !e->is32 && e->syscall == 49; }
static int is_listen(const struct event *e)  { return !e->is32 && e->syscall == 50; }

static void emit(struct event *e)
{
    char who[160];
    snprintf(who, sizeof who, "uid=%ld pid=%ld (%s)", e->uid, e->pid,
             e->comm[0] ? e->comm : "?");

    if (is_exec(e)) {
        if (strcmp(e->exe, SELF) == 0)
            return;                                 /* our own notes */
        if (!e->success) {
            const char *why = e->exit_code == -13 ? "BLOCKED (not allowed to run here)"
                            : e->exit_code == -2  ? "failed (no such program)"
                            : "failed";
            /* A failed run has no argument record; the PATH record says
             * what it tried to start. */
            write_line("run", "uid=%ld pid=%ld parent=%ld  %s  %s",
                       e->uid, e->pid, e->ppid,
                       e->args[0] ? e->args : e->target[0] ? e->target : "?", why);
            return;
        }
        int show_exe = e->exe[0] && strcmp(e->exe, BUSYBOX) != 0 &&
                       strcmp(base(e->exe), base(e->args)) != 0;
        write_line("run", "uid=%ld pid=%ld parent=%ld  %s%s%s%s",
                   e->uid, e->pid, e->ppid, e->args,
                   show_exe ? "  (" : "", show_exe ? e->exe : "", show_exe ? ")" : "");
        return;
    }

    char addr[96];
    if (is_connect(e)) {
        if (!e->saddr[0] || !decode_addr(e->saddr, addr, sizeof addr))
            return;
        const char *result = e->success ? ""
                           : e->exit_code == -115 ? "" /* in progress: normal */
                           : e->exit_code == -111 ? "  (refused)"
                           : "  (failed)";
        write_line("net", "%s  out -> %s%s", who, addr, result);
    } else if (is_bind(e)) {
        if (!e->success || !e->saddr[0] || !decode_addr(e->saddr, addr, sizeof addr))
            return;
        if (strlen(addr) > 2 && strcmp(addr + strlen(addr) - 2, ":0") == 0)
            return;                                 /* any port: not interesting */
        write_line("net", "%s  claims port %s", who, addr);
    } else if (is_listen(e)) {
        if (e->success)
            write_line("net", "%s  is waiting for incoming connections", who);
    }
}

static void handle_record(int type, const char *rec)
{
    /* Records start with "audit(SECONDS.MILLIS:SERIAL): " */
    const char *colon = strchr(rec, ':');
    const char *close = strchr(rec, ')');
    if (strncmp(rec, "audit(", 6) != 0 || !colon || !close || colon > close)
        return;
    unsigned long serial = strtoul(colon + 1, NULL, 10);
    const char *body = close + 1;
    while (*body == ':' || *body == ' ')
        body++;

    struct event *e = slot_for(serial);

    switch (type) {
    case AUDIT_SYSCALL:
        e->syscall = get_num(body, "syscall");
        e->exit_code = get_num(body, "exit");
        e->pid = get_num(body, "pid");
        e->ppid = get_num(body, "ppid");
        e->uid = get_num(body, "uid");
        {
            char ok[8], arch[16];
            get_value(body, "success", ok, sizeof ok, 0);
            e->success = strcmp(ok, "yes") == 0;
            get_value(body, "arch", arch, sizeof arch, 0);
            e->is32 = strcmp(arch, "40000003") == 0;
        }
        get_value(body, "comm", e->comm, sizeof e->comm, 1);
        get_value(body, "exe", e->exe, sizeof e->exe, 1);
        break;
    case AUDIT_EXECVE: {
        long argc = get_num(body, "argc");
        size_t used = 0;
        for (long i = 0; i < argc && i < 64 && used < sizeof e->args - 2; i++) {
            char key[16], val[300];
            snprintf(key, sizeof key, "a%ld", i);
            get_value(body, key, val, sizeof val, 1);
            if (!val[0] && !find_key(body, key)) {
                snprintf(val, sizeof val, "...");     /* very long argument */
            }
            int w = snprintf(e->args + used, sizeof e->args - used, "%s%s",
                             i ? " " : "", val);
            if (w < 0)
                break;
            used += (size_t)w;
            if (used >= sizeof e->args)
                used = sizeof e->args - 1;
        }
        break;
    }
    case AUDIT_PATH:
        if (!e->target[0] && get_num(body, "item") == 0)
            get_value(body, "name", e->target, sizeof e->target, 1);
        break;
    case AUDIT_SOCKADDR:
        get_value(body, "saddr", e->saddr, sizeof e->saddr, 0);
        break;
    case AUDIT_EOE:
        if (e->syscall >= 0)
            emit(e);
        e->used = 0;
        break;
    }
}

/* ------------------------------------------------------------ netlink */

static int audit_send(int fd, int type, const void *data, size_t len)
{
    char buf[NLMSG_SPACE(sizeof(struct audit_rule_data) + 64)];
    struct nlmsghdr *nh = (struct nlmsghdr *)buf;
    struct sockaddr_nl to = { .nl_family = AF_NETLINK };

    if (NLMSG_SPACE(len) > sizeof buf)
        return -1;
    memset(buf, 0, sizeof buf);
    nh->nlmsg_len = NLMSG_LENGTH(len);
    nh->nlmsg_type = type;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nh->nlmsg_seq = (unsigned)type;
    memcpy(NLMSG_DATA(nh), data, len);
    if (sendto(fd, buf, nh->nlmsg_len, 0, (struct sockaddr *)&to, sizeof to) < 0)
        return -1;

    /* Wait for the kernel's acknowledgement. */
    for (;;) {
        char reply[8192];
        ssize_t n = recv(fd, reply, sizeof reply, 0);
        if (n < 0)
            return errno == EINTR ? 0 : -1;
        for (struct nlmsghdr *r = (struct nlmsghdr *)reply; NLMSG_OK(r, (size_t)n);
             r = NLMSG_NEXT(r, n)) {
            if (r->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *err = NLMSG_DATA(r);
                if (err->error == 0 || err->error == -EEXIST)
                    return 0;
                errno = -err->error;
                return -1;
            }
        }
    }
}

static int add_rule(int fd, unsigned arch, const int *syscalls, int count)
{
    struct audit_rule_data rule;
    memset(&rule, 0, sizeof rule);
    rule.flags = AUDIT_FILTER_EXIT;
    rule.action = AUDIT_ALWAYS;
    for (int i = 0; i < count; i++)
        rule.mask[AUDIT_WORD(syscalls[i])] |= AUDIT_BIT(syscalls[i]);
    rule.field_count = 1;
    rule.fields[0] = AUDIT_ARCH;
    rule.values[0] = arch;
    rule.fieldflags[0] = AUDIT_EQUAL;
    return audit_send(fd, AUDIT_ADD_RULE, &rule, sizeof rule);
}

static int run_daemon(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_AUDIT);
    if (fd < 0) {
        perror("recorder: audit socket");
        return 1;
    }

    struct audit_status st;
    memset(&st, 0, sizeof st);
    st.mask = AUDIT_STATUS_ENABLED | AUDIT_STATUS_PID | AUDIT_STATUS_BACKLOG_LIMIT;
    st.enabled = 1;
    st.pid = (unsigned)getpid();
    st.backlog_limit = 8192;
    if (audit_send(fd, AUDIT_SET, &st, sizeof st) < 0) {
        perror("recorder: enable audit");
        return 1;
    }

    /* 64-bit programs: execve, execveat, connect, bind, listen. */
    static const int x64[] = { 59, 322, 42, 49, 50 };
    /* 32-bit programs, so they can't slip past: execve, execveat. */
    static const int x32[] = { 11, 358 };
    if (add_rule(fd, AUDIT_ARCH_X86_64, x64, 5) < 0 ||
        add_rule(fd, AUDIT_ARCH_I386, x32, 2) < 0) {
        perror("recorder: add audit rules");
        return 1;
    }

    write_line("boot", "recorder started: logging programs and network connections");

    /* Debugging aid: RECORDER_RAW=/path also dumps every raw kernel record. */
    int raw_fd = -1;
    if (getenv("RECORDER_RAW"))
        raw_fd = open(getenv("RECORDER_RAW"), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);

    char buf[MAX_RECORD + NLMSG_HDRLEN + 16];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
        if (n < 0) {
            if (errno == EINTR || errno == ENOBUFS)
                continue;                   /* ENOBUFS: some events were lost */
            perror("recorder: recv");
            return 1;
        }
        /* The kernel sends each audit record as its own message. Its
         * nlmsg_len is unreliable for these (it leaves out the header), so
         * the record length comes from what was actually received. */
        if ((size_t)n <= NLMSG_HDRLEN)
            continue;
        struct nlmsghdr *nh = (struct nlmsghdr *)buf;
        size_t len = strnlen((char *)NLMSG_DATA(nh), (size_t)n - NLMSG_HDRLEN);
        char rec[MAX_RECORD + 1];
        if (len > MAX_RECORD)
            len = MAX_RECORD;
        memcpy(rec, NLMSG_DATA(nh), len);
        rec[len] = '\0';
        if (raw_fd >= 0)
            dprintf(raw_fd, "%d %s\n", nh->nlmsg_type, rec);
        handle_record(nh->nlmsg_type, rec);
    }
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "note") == 0) {
        char text[900] = "";
        size_t used = 0;
        for (int i = 3; i < argc && used < sizeof text - 1; i++) {
            int w = snprintf(text + used, sizeof text - used, "%s%s", i > 3 ? " " : "", argv[i]);
            if (w < 0)
                break;
            used += (size_t)w;
        }
        write_line(argv[2], "%s", text);
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: recorder                     run the activity log daemon\n"
                        "       recorder note <kind> <text>  add a line to the log\n");
        return 2;
    }
    return run_daemon();
}
