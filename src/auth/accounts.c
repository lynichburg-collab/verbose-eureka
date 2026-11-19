/*
 * accounts — manage people's accounts. Run as root (e.g. with elevate).
 *
 *   accounts list                  who has an account
 *   accounts add <name> [--no-admin]
 *                                  new account; asks for a password (or
 *                                  reads one line from stdin if not a terminal)
 *   accounts password <name>       change someone's password
 *   accounts remove <name>         delete an account (keeps their files)
 *   accounts reset-waits           clear every wrong-password wait
 */
#include "common.c"
#include <dirent.h>

static int get_password(char *buf, size_t len)
{
    if (isatty(0))
        return ask_new_password(buf, len);
    if (!fgets(buf, (int)len, stdin))
        return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return strlen(buf) >= MIN_PASSWORD ? 0 : -1;
}

/* Drop `user` from every group's member list and remove their own group. */
static int remove_from_groups(const char *user)
{
    FILE *g = fopen(GROUP_FILE, "r");
    char line[1024], out[8192] = "";
    size_t used = 0, ulen = strlen(user);
    if (!g)
        return -1;
    while (fgets(line, sizeof line, g) && used < sizeof out - 1) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, user, ulen) == 0 && line[ulen] == ':')
            continue;                               /* their own group */
        char *members = strrchr(line, ':');
        if (!members)
            continue;
        *members++ = '\0';
        used += (size_t)snprintf(out + used, sizeof out - used, "%s:", line);
        int first = 1;
        for (char *m = strtok(members, ","); m; m = strtok(NULL, ",")) {
            if (strcmp(m, user) == 0)
                continue;
            used += (size_t)snprintf(out + used, sizeof out - used, "%s%s", first ? "" : ",", m);
            first = 0;
        }
        used += (size_t)snprintf(out + used, sizeof out - used, "\n");
    }
    fclose(g);
    return rewrite_file(GROUP_FILE, 0644, keep_none, NULL, out);
}

static int cmd_list(void)
{
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) != NULL)
        if (pw->pw_uid >= 1000 && pw->pw_uid < 60000)
            printf("%-16s %s%s\n", pw->pw_name, pw->pw_dir,
                   in_group(pw->pw_name, ADMIN_GROUP) ? "  (admin)" : "");
    endpwent();
    return 0;
}

int main(int argc, char **argv)
{
    if (getuid() != 0 || geteuid() != 0) {
        fputs("accounts: run this as root, e.g. 'elevate accounts ...'\n", stderr);
        return 1;
    }
    if (argc < 2 || strcmp(argv[1], "list") == 0)
        return cmd_list();

    char pw[256];
    if (strcmp(argv[1], "add") == 0 && argc >= 3) {
        int admin = !(argc >= 4 && strcmp(argv[3], "--no-admin") == 0);
        if (!valid_username(argv[2]) || getpwnam(argv[2])) {
            fprintf(stderr, "accounts: '%s' is taken or not a valid name\n", argv[2]);
            return 1;
        }
        if (get_password(pw, sizeof pw) < 0) {
            fprintf(stderr, "accounts: need a password of at least %d characters\n", MIN_PASSWORD);
            return 1;
        }
        int r = create_person(argv[2], pw, admin);
        memset(pw, 0, sizeof pw);
        if (r < 0) {
            fputs("accounts: couldn't create the account\n", stderr);
            return 1;
        }
        printf("Created '%s'%s.\n", argv[2], admin ? " (admin)" : "");
        return 0;
    }
    if (strcmp(argv[1], "password") == 0 && argc >= 3) {
        if (!getpwnam(argv[2])) {
            fprintf(stderr, "accounts: no account '%s'\n", argv[2]);
            return 1;
        }
        if (get_password(pw, sizeof pw) < 0)
            return 1;
        int r = set_password(argv[2], pw);
        memset(pw, 0, sizeof pw);
        if (r < 0)
            return 1;
        activity("account", "password changed for '%s'", argv[2]);
        puts("Password changed.");
        return 0;
    }
    if (strcmp(argv[1], "remove") == 0 && argc >= 3) {
        struct passwd *pw = getpwnam(argv[2]);
        if (!pw || pw->pw_uid < 1000) {
            fprintf(stderr, "accounts: no removable account '%s'\n", argv[2]);
            return 1;
        }
        char home[256];
        snprintf(home, sizeof home, "%s", pw->pw_dir);
        if (rewrite_file(PASSWD_FILE, 0644, line_not_user, argv[2], NULL) < 0 ||
            rewrite_file(SHADOW_FILE, 0600, line_not_user, argv[2], NULL) < 0 ||
            remove_from_groups(argv[2]) < 0) {
            fputs("accounts: couldn't remove the account\n", stderr);
            return 1;
        }
        activity("account", "removed account '%s'", argv[2]);
        printf("Removed '%s'. Their files are still in %s.\n", argv[2], home);
        return 0;
    }
    if (strcmp(argv[1], "reset-waits") == 0) {
        DIR *d = opendir(STATE_DIR);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "tty-", 4) == 0) {
                char path[300];
                snprintf(path, sizeof path, STATE_DIR "/%s", e->d_name);
                unlink(path);
            }
        }
        if (d)
            closedir(d);
        activity("account", "wrong-password waits cleared");
        puts("All wrong-password waits cleared.");
        return 0;
    }
    fputs("usage: accounts list | add <name> [--no-admin] | password <name> |\n"
          "                remove <name> | reset-waits\n", stderr);
    return 2;
}
