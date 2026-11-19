/*
 * firewall — block every incoming connection.
 *
 *   firewall on       load the rules (startup does this before the network)
 *   firewall off      remove them (for troubleshooting)
 *   firewall status   on or off, and how many incoming packets were blocked
 *
 * Talks to the kernel's nftables directly over netlink; no other tools.
 * The rules, in table "mtplatitudes":
 *
 *   incoming (input hook, policy drop)
 *     - from this machine to itself (loopback): allow
 *     - replies to connections this machine started: allow
 *     - ICMPv6 (IPv6 needs it to find routers and neighbours): allow
 *     - anything else: counted, then dropped
 *   forwarding (forward hook, policy drop): this is not a router
 *
 * Outgoing traffic is not restricted.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_conntrack_common.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netlink.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define TABLE   "mtplatitudes"
#define CHAIN   "incoming"

/* ------------------------------------------------------- message building */

static char buf[16384];
static size_t used;
static uint32_t seq = 1;
static int messages_needing_ack;

static struct nlmsghdr *begin_msg(uint16_t type, uint16_t flags, uint8_t family, uint16_t res_id)
{
    struct nlmsghdr *nh = (struct nlmsghdr *)(buf + used);
    memset(nh, 0, NLMSG_SPACE(sizeof(struct nfgenmsg)));
    nh->nlmsg_type = type;
    nh->nlmsg_flags = NLM_F_REQUEST | flags;
    nh->nlmsg_seq = seq++;
    nh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nfgenmsg));
    struct nfgenmsg *g = NLMSG_DATA(nh);
    g->nfgen_family = family;
    g->version = NFNETLINK_V0;
    g->res_id = htons(res_id);
    if (flags & NLM_F_ACK)
        messages_needing_ack++;
    return nh;
}

static void end_msg(struct nlmsghdr *nh)
{
    used += NLMSG_ALIGN(nh->nlmsg_len);
}

static struct nlattr *put(struct nlmsghdr *nh, uint16_t type, const void *data, size_t len)
{
    struct nlattr *a = (struct nlattr *)((char *)nh + NLMSG_ALIGN(nh->nlmsg_len));
    a->nla_type = type;
    a->nla_len = (uint16_t)(NLA_HDRLEN + len);
    if (len)
        memcpy((char *)a + NLA_HDRLEN, data, len);
    nh->nlmsg_len = NLMSG_ALIGN(nh->nlmsg_len) + NLA_ALIGN(a->nla_len);
    return a;
}

static void put_str(struct nlmsghdr *nh, uint16_t type, const char *s)
{
    put(nh, type, s, strlen(s) + 1);
}

static void put_be32(struct nlmsghdr *nh, uint16_t type, uint32_t v)
{
    uint32_t be = htonl(v);
    put(nh, type, &be, sizeof be);
}

static struct nlattr *nest_begin(struct nlmsghdr *nh, uint16_t type)
{
    return put(nh, type | NLA_F_NESTED, NULL, 0);
}

static void nest_end(struct nlmsghdr *nh, struct nlattr *a)
{
    a->nla_len = (uint16_t)((char *)nh + nh->nlmsg_len - (char *)a);
}

static uint16_t nft(int msg) { return (uint16_t)((NFNL_SUBSYS_NFTABLES << 8) | msg); }

/* --- expressions --- */

static struct nlattr *expr_begin(struct nlmsghdr *nh, const char *name, struct nlattr **data)
{
    struct nlattr *elem = nest_begin(nh, NFTA_LIST_ELEM);
    put_str(nh, NFTA_EXPR_NAME, name);
    *data = nest_begin(nh, NFTA_EXPR_DATA);
    return elem;
}

static void expr_end(struct nlmsghdr *nh, struct nlattr *elem, struct nlattr *data)
{
    nest_end(nh, data);
    nest_end(nh, elem);
}

static void expr_meta(struct nlmsghdr *nh, uint32_t key)
{
    struct nlattr *d, *e = expr_begin(nh, "meta", &d);
    put_be32(nh, NFTA_META_KEY, key);
    put_be32(nh, NFTA_META_DREG, NFT_REG_1);
    expr_end(nh, e, d);
}

static void expr_ct_state(struct nlmsghdr *nh)
{
    struct nlattr *d, *e = expr_begin(nh, "ct", &d);
    put_be32(nh, NFTA_CT_KEY, NFT_CT_STATE);
    put_be32(nh, NFTA_CT_DREG, NFT_REG_1);
    expr_end(nh, e, d);
}

static void put_data(struct nlmsghdr *nh, uint16_t type, const void *v, size_t len)
{
    struct nlattr *n = nest_begin(nh, type);
    put(nh, NFTA_DATA_VALUE, v, len);
    nest_end(nh, n);
}

static void expr_cmp(struct nlmsghdr *nh, uint32_t op, const void *v, size_t len)
{
    struct nlattr *d, *e = expr_begin(nh, "cmp", &d);
    put_be32(nh, NFTA_CMP_SREG, NFT_REG_1);
    put_be32(nh, NFTA_CMP_OP, op);
    put_data(nh, NFTA_CMP_DATA, v, len);
    expr_end(nh, e, d);
}

static void expr_and_mask(struct nlmsghdr *nh, uint32_t mask)
{
    uint32_t zero = 0;
    struct nlattr *d, *e = expr_begin(nh, "bitwise", &d);
    put_be32(nh, NFTA_BITWISE_SREG, NFT_REG_1);
    put_be32(nh, NFTA_BITWISE_DREG, NFT_REG_1);
    put_be32(nh, NFTA_BITWISE_LEN, sizeof mask);
    put_data(nh, NFTA_BITWISE_MASK, &mask, sizeof mask);
    put_data(nh, NFTA_BITWISE_XOR, &zero, sizeof zero);
    expr_end(nh, e, d);
}

static void expr_accept(struct nlmsghdr *nh)
{
    struct nlattr *d, *e = expr_begin(nh, "immediate", &d);
    put_be32(nh, NFTA_IMMEDIATE_DREG, NFT_REG_VERDICT);
    struct nlattr *data = nest_begin(nh, NFTA_IMMEDIATE_DATA);
    struct nlattr *verdict = nest_begin(nh, NFTA_DATA_VERDICT);
    put_be32(nh, NFTA_VERDICT_CODE, NF_ACCEPT);
    nest_end(nh, verdict);
    nest_end(nh, data);
    expr_end(nh, e, d);
}

static void expr_counter(struct nlmsghdr *nh)
{
    struct nlattr *d, *e = expr_begin(nh, "counter", &d);
    expr_end(nh, e, d);
}

/* --- objects --- */

static void msg_table(int msg, uint16_t flags)
{
    struct nlmsghdr *nh = begin_msg(nft(msg), flags | NLM_F_ACK, NFPROTO_INET, 0);
    put_str(nh, NFTA_TABLE_NAME, TABLE);
    end_msg(nh);
}

static void msg_chain(const char *name, uint32_t hook)
{
    struct nlmsghdr *nh = begin_msg(nft(NFT_MSG_NEWCHAIN), NLM_F_CREATE | NLM_F_ACK,
                                    NFPROTO_INET, 0);
    put_str(nh, NFTA_CHAIN_TABLE, TABLE);
    put_str(nh, NFTA_CHAIN_NAME, name);
    struct nlattr *h = nest_begin(nh, NFTA_CHAIN_HOOK);
    put_be32(nh, NFTA_HOOK_HOOKNUM, hook);
    put_be32(nh, NFTA_HOOK_PRIORITY, 0);
    nest_end(nh, h);
    put_be32(nh, NFTA_CHAIN_POLICY, NF_DROP);
    put_str(nh, NFTA_CHAIN_TYPE, "filter");
    end_msg(nh);
}

static struct nlmsghdr *rule_begin(struct nlattr **exprs)
{
    struct nlmsghdr *nh = begin_msg(nft(NFT_MSG_NEWRULE),
                                    NLM_F_CREATE | NLM_F_APPEND | NLM_F_ACK, NFPROTO_INET, 0);
    put_str(nh, NFTA_RULE_TABLE, TABLE);
    put_str(nh, NFTA_RULE_CHAIN, CHAIN);
    *exprs = nest_begin(nh, NFTA_RULE_EXPRESSIONS);
    return nh;
}

static void rule_end(struct nlmsghdr *nh, struct nlattr *exprs)
{
    nest_end(nh, exprs);
    end_msg(nh);
}

static void batch(uint16_t type)
{
    struct nlmsghdr *nh = begin_msg(type, 0, 0 /* AF_UNSPEC */, NFNL_SUBSYS_NFTABLES);
    end_msg(nh);
}

/* ------------------------------------------------------- talking to kernel */

static int nl_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_NETFILTER);
    if (fd < 0) {
        perror("firewall: netlink");
        return -1;
    }
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static int send_batch(int fd)
{
    struct sockaddr_nl to = { .nl_family = AF_NETLINK };
    if (sendto(fd, buf, used, 0, (struct sockaddr *)&to, sizeof to) < 0) {
        perror("firewall: send");
        return -1;
    }
    int acks = 0;
    while (acks < messages_needing_ack) {
        char reply[8192];
        ssize_t n = recv(fd, reply, sizeof reply, 0);
        if (n < 0) {
            perror("firewall: no answer from the kernel");
            return -1;
        }
        for (struct nlmsghdr *r = (struct nlmsghdr *)reply; NLMSG_OK(r, (size_t)n);
             r = NLMSG_NEXT(r, n)) {
            if (r->nlmsg_type != NLMSG_ERROR)
                continue;
            struct nlmsgerr *err = NLMSG_DATA(r);
            if (err->error != 0) {
                fprintf(stderr, "firewall: the kernel rejected the rules: %s\n",
                        strerror(-err->error));
                return -1;
            }
            acks++;
        }
    }
    return 0;
}

/* ------------------------------------------------------- commands */

static int cmd_on(int fd)
{
    static const uint32_t loopback = 1;         /* "lo" is always interface 1 */
    static const uint8_t icmpv6 = 58;
    const uint32_t est_rel = NF_CT_STATE_BIT(IP_CT_ESTABLISHED) |
                             NF_CT_STATE_BIT(IP_CT_RELATED);
    const uint32_t zero = 0;
    struct nlattr *x;
    struct nlmsghdr *r;

    batch(NFNL_MSG_BATCH_BEGIN);
    /* Make sure the table exists, delete it, then build it fresh: replaces
     * any older version of the rules in one step. */
    msg_table(NFT_MSG_NEWTABLE, NLM_F_CREATE);
    msg_table(NFT_MSG_DELTABLE, 0);
    msg_table(NFT_MSG_NEWTABLE, NLM_F_CREATE);
    msg_chain(CHAIN, NF_INET_LOCAL_IN);
    msg_chain("forwarding", NF_INET_FORWARD);

    r = rule_begin(&x);                          /* loopback: allow */
    expr_meta(r, NFT_META_IIF);
    expr_cmp(r, NFT_CMP_EQ, &loopback, sizeof loopback);
    expr_accept(r);
    rule_end(r, x);

    r = rule_begin(&x);                          /* replies: allow */
    expr_ct_state(r);
    expr_and_mask(r, est_rel);
    expr_cmp(r, NFT_CMP_NEQ, &zero, sizeof zero);
    expr_accept(r);
    rule_end(r, x);

    r = rule_begin(&x);                          /* ICMPv6: allow */
    expr_meta(r, NFT_META_L4PROTO);
    expr_cmp(r, NFT_CMP_EQ, &icmpv6, sizeof icmpv6);
    expr_accept(r);
    rule_end(r, x);

    r = rule_begin(&x);                          /* everything else: count */
    expr_counter(r);
    rule_end(r, x);
    batch(NFNL_MSG_BATCH_END);

    if (send_batch(fd) < 0)
        return 1;
    puts("Firewall on: incoming connections are blocked.");
    return 0;
}

static int cmd_off(int fd)
{
    batch(NFNL_MSG_BATCH_BEGIN);
    msg_table(NFT_MSG_NEWTABLE, NLM_F_CREATE);
    msg_table(NFT_MSG_DELTABLE, 0);
    batch(NFNL_MSG_BATCH_END);
    if (send_batch(fd) < 0)
        return 1;
    puts("Firewall off: incoming connections are NOT blocked. 'firewall on' to restore.");
    return 0;
}

/* Find attribute `type` among the attributes in [start, start+len). */
static struct nlattr *find_attr(void *start, size_t len, uint16_t type)
{
    char *p = start, *end = p + len;
    while (p + NLA_HDRLEN <= end) {
        struct nlattr *a = (struct nlattr *)p;
        if (a->nla_len < NLA_HDRLEN || p + a->nla_len > end)
            break;
        if ((a->nla_type & NLA_TYPE_MASK) == type)
            return a;
        p += NLA_ALIGN(a->nla_len);
    }
    return NULL;
}

static void *attr_data(struct nlattr *a) { return (char *)a + NLA_HDRLEN; }
static size_t attr_len(struct nlattr *a) { return a->nla_len - NLA_HDRLEN; }

static int cmd_status(int fd)
{
    struct nlmsghdr *nh = begin_msg(nft(NFT_MSG_GETRULE), NLM_F_DUMP, NFPROTO_INET, 0);
    put_str(nh, NFTA_RULE_TABLE, TABLE);
    put_str(nh, NFTA_RULE_CHAIN, CHAIN);
    end_msg(nh);
    struct sockaddr_nl to = { .nl_family = AF_NETLINK };
    if (sendto(fd, buf, used, 0, (struct sockaddr *)&to, sizeof to) < 0) {
        perror("firewall: send");
        return 1;
    }

    int rules = 0, done = 0, error = 0;
    unsigned long long blocked = 0;
    while (!done) {
        char reply[16384];
        ssize_t n = recv(fd, reply, sizeof reply, 0);
        if (n < 0)
            break;
        for (struct nlmsghdr *m = (struct nlmsghdr *)reply; NLMSG_OK(m, (size_t)n);
             m = NLMSG_NEXT(m, n)) {
            if (m->nlmsg_type == NLMSG_ERROR) {
                error = -((struct nlmsgerr *)NLMSG_DATA(m))->error;
                done = 1;
                break;
            }
            if (m->nlmsg_type == NLMSG_DONE) {
                done = 1;
                break;
            }
            rules++;
            size_t hdr = NLMSG_SPACE(sizeof(struct nfgenmsg));
            struct nlattr *exprs = find_attr((char *)m + hdr, m->nlmsg_len - hdr,
                                             NFTA_RULE_EXPRESSIONS);
            if (!exprs)
                continue;
            char *p = attr_data(exprs), *end = p + attr_len(exprs);
            while (p + NLA_HDRLEN <= end) {
                struct nlattr *elem = (struct nlattr *)p;
                if (elem->nla_len < NLA_HDRLEN)
                    break;
                struct nlattr *name = find_attr(attr_data(elem), attr_len(elem), NFTA_EXPR_NAME);
                struct nlattr *data = find_attr(attr_data(elem), attr_len(elem), NFTA_EXPR_DATA);
                if (name && data && strcmp(attr_data(name), "counter") == 0) {
                    struct nlattr *pk = find_attr(attr_data(data), attr_len(data),
                                                  NFTA_COUNTER_PACKETS);
                    if (pk && attr_len(pk) == 8) {
                        uint64_t be;
                        memcpy(&be, attr_data(pk), 8);
                        blocked = ((uint64_t)ntohl((uint32_t)be) << 32) | ntohl((uint32_t)(be >> 32));
                    }
                }
                p += NLA_ALIGN(elem->nla_len);
            }
        }
    }

    if (error == EPERM || error == EACCES) {
        puts("Firewall: only root can read the rules. Try: elevate firewall status");
        return 4;
    }
    if (rules == 0) {
        puts("Firewall: OFF. Incoming connections are not blocked. ('firewall on')");
        return 3;
    }
    printf("Firewall: on. Incoming connections are blocked.\n"
           "Blocked so far this boot: %llu packet%s.\n", blocked, blocked == 1 ? "" : "s");
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "status";
    if (strcmp(cmd, "status") != 0 && geteuid() != 0) {
        fputs("firewall: changing it needs root ('elevate firewall ...')\n", stderr);
        return 1;
    }
    int fd = nl_open();
    if (fd < 0)
        return 1;
    if (strcmp(cmd, "on") == 0)
        return cmd_on(fd);
    if (strcmp(cmd, "off") == 0)
        return cmd_off(fd);
    if (strcmp(cmd, "status") == 0)
        return cmd_status(fd);
    fputs("usage: firewall on | off | status\n", stderr);
    return 2;
}
