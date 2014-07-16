/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Tom Gundersen <teg@jklm.no>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
 ***/

#include <arpa/inet.h>
#include <resolv.h>
#include <linux/if.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <netinet/in.h>

#include "rtnl-util.h"
#include "event-util.h"
#include "network-util.h"
#include "sd-dhcp-lease.h"
#include "dhcp-lease-internal.h"
#include "network-internal.h"
#include "conf-parser.h"
#include "socket-util.h"
#include "resolved.h"

#define SEND_TIMEOUT_USEC (200 * USEC_PER_MSEC)

static int manager_process_link(sd_rtnl *rtnl, sd_rtnl_message *mm, void *userdata) {
        Manager *m = userdata;
        uint16_t type;
        Link *l;
        int ifindex, r;

        assert(rtnl);
        assert(m);
        assert(mm);

        r = sd_rtnl_message_get_type(mm, &type);
        if (r < 0)
                goto fail;

        r = sd_rtnl_message_link_get_ifindex(mm, &ifindex);
        if (r < 0)
                goto fail;

        l = hashmap_get(m->links, INT_TO_PTR(ifindex));

        switch (type) {

        case RTM_NEWLINK:
                if (!l) {
                        log_debug("Found link %i", ifindex);

                        r = link_new(m, &l, ifindex);
                        if (r < 0)
                                goto fail;
                }

                r = link_update_rtnl(l, mm);
                if (r < 0)
                        goto fail;

                break;

        case RTM_DELLINK:
                if (l) {
                        log_debug("Removing link %i", l->ifindex);
                        link_free(l);
                }

                break;
        }

        return 0;

fail:
        log_warning("Failed to process RTNL link message: %s", strerror(-r));
        return 0;
}

static int manager_process_address(sd_rtnl *rtnl, sd_rtnl_message *mm, void *userdata) {
        Manager *m = userdata;
        union in_addr_union address;
        unsigned char family;
        uint16_t type;
        int r, ifindex;
        LinkAddress *a;
        Link *l;

        assert(rtnl);
        assert(mm);
        assert(m);

        r = sd_rtnl_message_get_type(mm, &type);
        if (r < 0)
                goto fail;

        r = sd_rtnl_message_addr_get_ifindex(mm, &ifindex);
        if (r < 0)
                goto fail;

        l = hashmap_get(m->links, INT_TO_PTR(ifindex));
        if (!l)
                return 0;

        r = sd_rtnl_message_addr_get_family(mm, &family);
        if (r < 0)
                goto fail;

        switch (family) {

        case AF_INET:
                r = sd_rtnl_message_read_in_addr(mm, IFA_LOCAL, &address.in);
                if (r < 0) {
                        r = sd_rtnl_message_read_in_addr(mm, IFA_ADDRESS, &address.in);
                        if (r < 0)
                                goto fail;
                }

                break;

        case AF_INET6:
                r = sd_rtnl_message_read_in6_addr(mm, IFA_LOCAL, &address.in6);
                if (r < 0) {
                        r = sd_rtnl_message_read_in6_addr(mm, IFA_ADDRESS, &address.in6);
                        if (r < 0)
                                goto fail;
                }

                break;

        default:
                return 0;
        }

        a = link_find_address(l, family, &address);

        switch (type) {

        case RTM_NEWADDR:

                if (!a) {
                        r = link_address_new(l, &a, family, &address);
                        if (r < 0)
                                return r;
                }

                r = link_address_update_rtnl(a, mm);
                if (r < 0)
                        return r;

                break;

        case RTM_DELADDR:
                if (a)
                        link_address_free(a);
                break;
        }

        return 0;

fail:
        log_warning("Failed to process RTNL address message: %s", strerror(-r));
        return 0;
}


static int manager_rtnl_listen(Manager *m) {
        _cleanup_rtnl_message_unref_ sd_rtnl_message *req = NULL, *reply = NULL;
        sd_rtnl_message *i;
        int r;

        assert(m);

        /* First, subscibe to interfaces coming and going */
        r = sd_rtnl_open(&m->rtnl, 3, RTNLGRP_LINK, RTNLGRP_IPV4_IFADDR, RTNLGRP_IPV6_IFADDR);
        if (r < 0)
                return r;

        r = sd_rtnl_attach_event(m->rtnl, m->event, 0);
        if (r < 0)
                return r;

        r = sd_rtnl_add_match(m->rtnl, RTM_NEWLINK, manager_process_link, m);
        if (r < 0)
                return r;

        r = sd_rtnl_add_match(m->rtnl, RTM_DELLINK, manager_process_link, m);
        if (r < 0)
                return r;

        r = sd_rtnl_add_match(m->rtnl, RTM_NEWADDR, manager_process_address, m);
        if (r < 0)
                return r;

        r = sd_rtnl_add_match(m->rtnl, RTM_DELADDR, manager_process_address, m);
        if (r < 0)
                return r;

        /* Then, enumerate all links */
        r = sd_rtnl_message_new_link(m->rtnl, &req, RTM_GETLINK, 0);
        if (r < 0)
                return r;

        r = sd_rtnl_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_rtnl_call(m->rtnl, req, 0, &reply);
        if (r < 0)
                return r;

        for (i = reply; i; i = sd_rtnl_message_next(i)) {
                r = manager_process_link(m->rtnl, i, m);
                if (r < 0)
                        return r;
        }

        req = sd_rtnl_message_unref(req);
        reply = sd_rtnl_message_unref(reply);

        /* Finally, enumerate all addresses, too */
        r = sd_rtnl_message_new_addr(m->rtnl, &req, RTM_GETADDR, 0, AF_UNSPEC);
        if (r < 0)
                return r;

        r = sd_rtnl_message_request_dump(req, true);
        if (r < 0)
                return r;

        r = sd_rtnl_call(m->rtnl, req, 0, &reply);
        if (r < 0)
                return r;

        for (i = reply; i; i = sd_rtnl_message_next(i)) {
                r = manager_process_address(m->rtnl, i, m);
                if (r < 0)
                        return r;
        }

        return r;
}

static int on_network_event(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;
        Iterator i;
        Link *l;
        int r;

        assert(m);

        sd_network_monitor_flush(m->network_monitor);

        HASHMAP_FOREACH(l, m->links, i) {
                r = link_update_monitor(l);
                if (r < 0)
                        log_warning("Failed to update monitor information for %i: %s", l->ifindex, strerror(-r));
        }

        r = manager_write_resolv_conf(m);
        if (r < 0)
                log_warning("Could not update resolv.conf: %s", strerror(-r));

        return 0;
}

static int manager_network_monitor_listen(Manager *m) {
        int r, fd, events;

        assert(m);

        r = sd_network_monitor_new(&m->network_monitor, NULL);
        if (r < 0)
                return r;

        fd = sd_network_monitor_get_fd(m->network_monitor);
        if (fd < 0)
                return fd;

        events = sd_network_monitor_get_events(m->network_monitor);
        if (events < 0)
                return events;

        r = sd_event_add_io(m->event, &m->network_event_source, fd, events, &on_network_event, m);
        if (r < 0)
                return r;

        return 0;
}

static int parse_dns_server_string(Manager *m, const char *string) {
        char *word, *state;
        size_t length;
        int r;

        assert(m);
        assert(string);

        FOREACH_WORD_QUOTED(word, length, string, state) {
                char buffer[length+1];
                unsigned family;
                union in_addr_union addr;

                memcpy(buffer, word, length);
                buffer[length] = 0;

                r = in_addr_from_string_auto(buffer, &family, &addr);
                if (r < 0) {
                        log_warning("Ignoring invalid DNS address '%s'", buffer);
                        continue;
                }

                /* filter out duplicates */
                if (manager_find_dns_server(m, family, &addr))
                        continue;

                r = dns_server_new(m, NULL, DNS_SERVER_SYSTEM, NULL, family, &addr);
                if (r < 0)
                        return r;
        }

        return 0;
}

int config_parse_dnsv(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Manager *m = userdata;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(m);

        /* Empty assignment means clear the list */
        if (isempty(rvalue)) {
                while (m->dns_servers)
                        dns_server_free(m->dns_servers);

                return 0;
        }

        r = parse_dns_server_string(m, rvalue);
        if (r < 0) {
                log_error("Failed to parse DNS server string");
                return r;
        }

        return 0;
}

int manager_parse_config_file(Manager *m) {
        assert(m);

        return config_parse(NULL, "/etc/systemd/resolved.conf", NULL,
                            "Resolve\0",
                            config_item_perf_lookup, resolved_gperf_lookup,
                            false, false, true, m);
}

int manager_new(Manager **ret) {
        _cleanup_(manager_freep) Manager *m = NULL;
        int r;

        assert(ret);

        m = new0(Manager, 1);
        if (!m)
                return -ENOMEM;

        m->dns_ipv4_fd = m->dns_ipv6_fd = -1;

        r = parse_dns_server_string(m, /* "172.31.0.125 2001:4860:4860::8888 2001:4860:4860::8889" */ DNS_SERVERS);
        if (r < 0)
                return r;

        r = sd_event_default(&m->event);
        if (r < 0)
                return r;

        sd_event_add_signal(m->event, NULL, SIGTERM, NULL,  NULL);
        sd_event_add_signal(m->event, NULL, SIGINT, NULL, NULL);

        sd_event_set_watchdog(m->event, true);

        r = dns_scope_new(m, &m->unicast_scope, DNS_SCOPE_DNS);
        if (r < 0)
                return r;

        r = manager_network_monitor_listen(m);
        if (r < 0)
                return r;

        r = manager_rtnl_listen(m);
        if (r < 0)
                return r;

        r = manager_connect_bus(m);
        if (r < 0)
                return r;

        *ret = m;
        m = NULL;

        return 0;
}

Manager *manager_free(Manager *m) {
        Link *l;

        if (!m)
                return NULL;

        while (m->dns_queries)
                dns_query_free(m->dns_queries);

        hashmap_free(m->dns_query_transactions);

        while ((l = hashmap_first(m->links)))
               link_free(l);
        hashmap_free(m->links);

        dns_scope_free(m->unicast_scope);

        while (m->dns_servers)
                dns_server_free(m->dns_servers);

        sd_event_source_unref(m->network_event_source);
        sd_network_monitor_unref(m->network_monitor);

        sd_event_source_unref(m->dns_ipv4_event_source);
        sd_event_source_unref(m->dns_ipv6_event_source);

        safe_close(m->dns_ipv4_fd);
        safe_close(m->dns_ipv6_fd);

        sd_event_source_unref(m->bus_retry_event_source);
        sd_bus_unref(m->bus);

        sd_event_unref(m->event);
        free(m);

        return NULL;
}

static void write_resolve_conf_server(DnsServer *s, FILE *f, unsigned *count) {
        _cleanup_free_ char *t  = NULL;
        int r;

        assert(s);
        assert(f);
        assert(count);

        r = in_addr_to_string(s->family, &s->address, &t);
        if (r < 0) {
                log_warning("Invalid DNS address. Ignoring.");
                return;
        }

        if (*count == MAXNS)
                fputs("# Too many DNS servers configured, the following entries may be ignored\n", f);

        fprintf(f, "nameserver %s\n", t);
        (*count) ++;
}

int manager_write_resolv_conf(Manager *m) {
        const char *path = "/run/systemd/resolve/resolv.conf";
        _cleanup_free_ char *temp_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        unsigned count = 0;
        DnsServer *s;
        Iterator i;
        Link *l;
        int r;

        assert(m);

        r = fopen_temporary(path, &f, &temp_path);
        if (r < 0)
                return r;

        fchmod(fileno(f), 0644);

        fputs("# This file is managed by systemd-resolved(8). Do not edit.\n#\n"
              "# Third party programs must not access this file directly, but\n"
              "# only through the symlink at /etc/resolv.conf. To manage\n"
              "# resolv.conf(5) in a different way, replace the symlink by a\n"
              "# static file or a different symlink.\n\n", f);

        HASHMAP_FOREACH(l, m->links, i) {
                LIST_FOREACH(servers, s, l->link_dns_servers)
                        write_resolve_conf_server(s, f, &count);

                LIST_FOREACH(servers, s, l->dhcp_dns_servers)
                        write_resolve_conf_server(s, f, &count);
        }

        LIST_FOREACH(servers, s, m->dns_servers)
                write_resolve_conf_server(s, f, &count);

        r = fflush_and_check(f);
        if (r < 0)
                goto fail;

        if (rename(temp_path, path) < 0) {
                r = -errno;
                goto fail;
        }

        return 0;

fail:
        unlink(path);
        unlink(temp_path);
        return r;
}

int manager_dns_ipv4_recv(Manager *m, DnsPacket **ret) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        struct msghdr mh = {};
        int fd, ms = 0, r;
        struct iovec iov;
        ssize_t l;

        assert(m);
        assert(ret);

        fd = manager_dns_ipv4_fd(m);
        if (fd < 0)
                return fd;

        r = ioctl(fd, FIONREAD, &ms);
        if (r < 0)
                return -errno;
        if (ms < 0)
                return -EIO;

        r = dns_packet_new(&p, ms);
        if (r < 0)
                return r;

        iov.iov_base = DNS_PACKET_DATA(p);
        iov.iov_len = p->allocated;

        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;

        l = recvmsg(fd, &mh, 0);
        if (l < 0) {
                if (errno == EAGAIN || errno == EINTR)
                        return 0;

                return -errno;
        }

        if (l <= 0)
                return -EIO;

        p->size = (size_t) l;

        *ret = p;
        p = NULL;

        return 1;
}

int manager_dns_ipv6_recv(Manager *m, DnsPacket **ret) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        struct msghdr mh = {};
        struct iovec iov;
        int fd, ms = 0, r;
        ssize_t l;

        assert(m);
        assert(ret);

        fd = manager_dns_ipv6_fd(m);
        if (fd < 0)
                return fd;

        r = ioctl(fd, FIONREAD, &ms);
        if (r < 0)
                return -errno;
        if (ms < 0)
                return -EIO;

        r = dns_packet_new(&p, ms);
        if (r < 0)
                return r;

        iov.iov_base = DNS_PACKET_DATA(p);
        iov.iov_len = p->allocated;

        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;

        l = recvmsg(fd, &mh, 0);
        if (l < 0) {
                if (errno == EAGAIN || errno == EINTR)
                        return 0;

                return -errno;
        }

        if (l <= 0)
                return -EIO;

        p->size = (size_t) l;

        *ret = p;
        p = NULL;

        return 1;
}

static int on_dns_ipv4_packet(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        DnsQueryTransaction *t = NULL;
        Manager *m = userdata;
        int r;

        r = manager_dns_ipv4_recv(m, &p);
        if (r <= 0)
                return r;

        t = hashmap_get(m->dns_query_transactions, UINT_TO_PTR(DNS_PACKET_ID(p)));
        if (!t)
                return 0;

        dns_query_transaction_reply(t, p);
        return 0;
}

static int on_dns_ipv6_packet(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        DnsQueryTransaction *t = NULL;
        Manager *m = userdata;
        int r;

        r = manager_dns_ipv6_recv(m, &p);
        if (r <= 0)
                return r;

        t = hashmap_get(m->dns_query_transactions, UINT_TO_PTR(DNS_PACKET_ID(p)));
        if (!t)
                return 0;

        dns_query_transaction_reply(t, p);
        return 0;
}

int manager_dns_ipv4_fd(Manager *m) {
        int r;

        assert(m);

        if (m->dns_ipv4_fd >= 0)
                return m->dns_ipv4_fd;

        m->dns_ipv4_fd = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (m->dns_ipv4_fd < 0)
                return -errno;

        r = sd_event_add_io(m->event, &m->dns_ipv4_event_source, m->dns_ipv4_fd, EPOLLIN, on_dns_ipv4_packet, m);
        if (r < 0)
                return r;

        return m->dns_ipv4_fd;
}

int manager_dns_ipv6_fd(Manager *m) {
        int r;

        assert(m);

        if (m->dns_ipv6_fd >= 0)
                return m->dns_ipv6_fd;

        m->dns_ipv6_fd = socket(AF_INET6, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (m->dns_ipv6_fd < 0)
                return -errno;

        r = sd_event_add_io(m->event, &m->dns_ipv6_event_source, m->dns_ipv6_fd, EPOLLIN, on_dns_ipv6_packet, m);
        if (r < 0)
                return r;

        return m->dns_ipv6_fd;
}

static int sendmsg_loop(int fd, struct msghdr *mh, int flags) {
        int r;

        assert(fd >= 0);
        assert(mh);

        for (;;) {
                if (sendmsg(fd, mh, flags) >= 0)
                        return 0;

                if (errno == EINTR)
                        continue;

                if (errno != EAGAIN)
                        return -errno;

                r = fd_wait_for_event(fd, POLLOUT, SEND_TIMEOUT_USEC);
                if (r < 0)
                        return r;
                if (r == 0)
                        return -ETIMEDOUT;
        }
}

int manager_dns_ipv4_send(Manager *m, DnsServer *srv, int ifindex, DnsPacket *p) {
        union sockaddr_union sa = {
                .in.sin_family = AF_INET,
                .in.sin_port = htobe16(53),
        };
        struct msghdr mh = {};
        struct iovec iov;
        uint8_t control[CMSG_SPACE(sizeof(struct in_pktinfo))];
        int fd;

        assert(m);
        assert(srv);
        assert(p);

        fd = manager_dns_ipv4_fd(m);
        if (fd < 0)
                return fd;

        iov.iov_base = DNS_PACKET_DATA(p);
        iov.iov_len = p->size;

        sa.in.sin_addr = srv->address.in;

        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_name = &sa.sa;
        mh.msg_namelen = sizeof(sa.in);

        if (ifindex > 0) {
                struct cmsghdr *cmsg;
                struct in_pktinfo *pi;

                zero(control);

                mh.msg_control = control;
                mh.msg_controllen = CMSG_LEN(sizeof(struct in_pktinfo));

                cmsg = CMSG_FIRSTHDR(&mh);
                cmsg->cmsg_len = mh.msg_controllen;
                cmsg->cmsg_level = IPPROTO_IP;
                cmsg->cmsg_type = IP_PKTINFO;

                pi = (struct in_pktinfo*) CMSG_DATA(cmsg);
                pi->ipi_ifindex = ifindex;
        }

        return sendmsg_loop(fd, &mh, 0);
}

int manager_dns_ipv6_send(Manager *m, DnsServer *srv, int ifindex, DnsPacket *p) {
        union sockaddr_union sa = {
                .in6.sin6_family = AF_INET6,
                .in6.sin6_port = htobe16(53),
        };

        struct msghdr mh = {};
        struct iovec iov;
        uint8_t control[CMSG_SPACE(sizeof(struct in6_pktinfo))];
        int fd;

        assert(m);
        assert(srv);
        assert(p);

        fd = manager_dns_ipv6_fd(m);
        if (fd < 0)
                return fd;

        iov.iov_base = DNS_PACKET_DATA(p);
        iov.iov_len = p->size;

        sa.in6.sin6_addr = srv->address.in6;
        sa.in6.sin6_scope_id = ifindex;

        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_name = &sa.sa;
        mh.msg_namelen = sizeof(sa.in6);

        if (ifindex > 0) {
                struct cmsghdr *cmsg;
                struct in6_pktinfo *pi;

                zero(control);

                mh.msg_control = control;
                mh.msg_controllen = CMSG_LEN(sizeof(struct in6_pktinfo));

                cmsg = CMSG_FIRSTHDR(&mh);
                cmsg->cmsg_len = mh.msg_controllen;
                cmsg->cmsg_level = IPPROTO_IPV6;
                cmsg->cmsg_type = IPV6_PKTINFO;

                pi = (struct in6_pktinfo*) CMSG_DATA(cmsg);
                pi->ipi6_ifindex = ifindex;
        }

        return sendmsg_loop(fd, &mh, 0);
}

DnsServer* manager_find_dns_server(Manager *m, unsigned char family, union in_addr_union *in_addr) {
        DnsServer *s;

        assert(m);
        assert(in_addr);

        LIST_FOREACH(servers, s, m->dns_servers) {

                if (s->family == family &&
                    in_addr_equal(family, &s->address, in_addr))
                        return s;
        }

        return NULL;
}

DnsServer *manager_get_dns_server(Manager *m) {
        assert(m);

        if (!m->current_dns_server)
                m->current_dns_server = m->dns_servers;

        return m->current_dns_server;
}

void manager_next_dns_server(Manager *m) {
        assert(m);

        if (!m->current_dns_server) {
                m->current_dns_server = m->dns_servers;
                return;
        }

        if (!m->current_dns_server)
                return;

        if (m->current_dns_server->servers_next) {
                m->current_dns_server = m->current_dns_server->servers_next;
                return;
        }

        m->current_dns_server = m->dns_servers;
}

uint32_t manager_find_mtu(Manager *m) {
        uint32_t mtu = 0;
        Link *l;
        Iterator i;

        /* If we don't know on which link a DNS packet would be
         * delivered, let's find the largest MTU that works on all
         * interfaces we know of */

        HASHMAP_FOREACH(l, m->links, i) {
                if (l->mtu <= 0)
                        continue;

                if (mtu <= 0 || l->mtu < mtu)
                        mtu = l->mtu;
        }

        return mtu;
}
