/*++
/* NAME
/*	smtp_addr 3
/* SUMMARY
/*	SMTP server address lookup
/* SYNOPSIS
/*	#include "smtp_addr.h"
/*
/*	DNS_RR *smtp_domain_addr(name, why)
/*	char	*name;
/*	VSTRING	*why;
/*
/*	DNS_RR *smtp_host_addr(name, why)
/*	char	*name;
/*	VSTRING	*why;
/* DESCRIPTION
/*	This module implements Internet address lookups. By default,
/*	lookups are done via the Internet domain name service (DNS).
/*	A reasonable number of CNAME indirections is permitted.
/*
/*	smtp_domain_addr() looks up the network addresses for mail 
/*	exchanger hosts listed for the named domain. Addresses are 
/*	returned in most-preferred first order. The result is truncated
/*	so that it contains only hosts that are more preferred than the 
/*	local mail server itself.
/*
/*	When no mail exchanger is listed in the DNS for \fIname\fR, the 
/*	request is passed to smtp_host_addr().
/*
/*	smtp_host_addr() looks up all addresses listed for the named
/*	host.  The host can be specified as a numerical Internet network
/*	address, or as a symbolic host name.
/*
/*	Results from smtp_domain_addr() or smtp_host_addr() are
/*	destroyed by dns_rr_free(), including null lists.
/* DIAGNOSTICS
/*	All routines either return a DNS_RR pointer, or return a null
/*	pointer and set the \fIsmtp_errno\fR global variable accordingly:
/* .IP SMTP_RETRY
/*	The request failed due to a soft error, and should be retried later.
/* .IP SMTP_FAIL
/*	The request attempt failed due to a hard error.
/* .PP
/*	In addition, a textual description of the problem is made available
/*	via the \fIwhy\fR argument.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <netdb.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

/* Utility library. */

#include <msg.h>
#include <vstring.h>
#include <mymalloc.h>
#include <inet_addr_list.h>

/* Global library. */

#include <mail_params.h>
#include <own_inet_addr.h>

/* DNS library. */

#include <dns.h>

/* Application-specific. */

#include "smtp.h"
#include "smtp_addr.h"

/* smtp_print_addr - print address list */

static void smtp_print_addr(char *what, DNS_RR *addr_list)
{
    DNS_RR *addr;
    struct in_addr in_addr;

    msg_info("begin %s address list", what);
    for (addr = addr_list; addr; addr = addr->next) {
	if (addr->data_len > sizeof(addr)) {
	    msg_warn("skipping address length %d", addr->data_len);
	} else {
	    memcpy((char *) &in_addr, addr->data, sizeof(in_addr));
	    msg_info("pref %4d host %s/%s",
		     addr->pref, addr->name,
		     inet_ntoa(in_addr));
	}
    }
    msg_info("end %s address list", what);
}

/* smtp_addr_one - address lookup for one host name */

static DNS_RR *smtp_addr_one(DNS_RR *addr_list, char *host, unsigned pref, VSTRING *why)
{
    char   *myname = "smtp_addr_one";
    DNS_RR *addr = 0;
    DNS_RR *rr;

    if (msg_verbose)
	msg_info("%s: host %s", myname, host);

    /*
     * Append the addresses for this host to the address list.
     */
    switch (dns_lookup(host, T_A, 0, &addr, (VSTRING *) 0, why)) {
    case DNS_OK:
	for (rr = addr; rr; rr = rr->next)
	    rr->pref = pref;
	addr_list = dns_rr_append(addr_list, addr);
	break;
    default:
	smtp_errno = SMTP_RETRY;
	break;
    case DNS_NOTFOUND:
    case DNS_FAIL:
	smtp_errno = SMTP_FAIL;
	break;
    }
    return (addr_list);
}

/* smtp_addr_list - address lookup for a list of mail exchangers */

static DNS_RR *smtp_addr_list(DNS_RR *mx_names, VSTRING *why)
{
    DNS_RR *addr_list = 0;
    DNS_RR *rr;

    /*
     * As long as we are able to look up any host address, we ignore problems
     * with DNS lookups.
     */
    for (rr = mx_names; rr; rr = rr->next) {
	if (rr->type != T_MX)
	    msg_panic("smtp_addr_list: bad resource type: %d", rr->type);
	addr_list = smtp_addr_one(addr_list, (char *) rr->data, rr->pref, why);
    }
    return (addr_list);
}

/* smtp_find_self - spot myself in a crowd of mail exchangers */

static DNS_RR *smtp_find_self(DNS_RR *addr_list)
{
    char   *myname = "smtp_find_self";
    INET_ADDR_LIST *self;
    DNS_RR *addr;
    int     i;

    /*
     * Find the first address that lists any address that this mail system is
     * supposed to be listening on.
     */
#define INADDRP(x) ((struct in_addr *) (x))

    self = own_inet_addr_list();
    for (addr = addr_list; addr; addr = addr->next) {
	for (i = 0; i < self->used; i++)
	    if (INADDRP(addr->data)->s_addr == self->addrs[i].s_addr) {
		if (msg_verbose)
		    msg_info("%s: found at pref %d", myname, addr->pref);
		return (addr);
	    }
    }

    /*
     * Didn't find myself.
     */
    if (msg_verbose)
	msg_info("%s: not found", myname);
    return (0);
}

/* smtp_truncate_self - truncate address list at self and equivalents */

static DNS_RR *smtp_truncate_self(DNS_RR *addr_list, unsigned pref,
				          char *name, VSTRING *why)
{
    DNS_RR *addr;
    DNS_RR *last;

    for (last = 0, addr = addr_list; addr; last = addr, addr = addr->next) {
	if (pref == addr->pref) {
	    if (msg_verbose)
		smtp_print_addr("truncated", addr);
	    dns_rr_free(addr);
	    if (last == 0) {
		vstring_sprintf(why, "mail for %s loops back to myself", name);
		smtp_errno = SMTP_FAIL;
		addr_list = 0;
	    } else {
		last->next = 0;
	    }
	    break;
	}
    }
    return (addr_list);
}

/* smtp_compare_mx - compare resource records by preference */

static int smtp_compare_mx(DNS_RR *a, DNS_RR *b)
{
    return (a->pref - b->pref);
}

/* smtp_domain_addr - mail exchanger address lookup */

DNS_RR *smtp_domain_addr(char *name, VSTRING *why)
{
    DNS_RR *mx_names;
    DNS_RR *addr_list = 0;
    DNS_RR *self;

    /*
     * Look up the mail exchanger hosts listed for this name. Sort the
     * results by preference. Look up the corresponding host addresses, and
     * truncate the list so that it contains only hosts that are more
     * preferred than myself. When no MX resource records exist, look up the
     * addresses listed for this name.
     */
    switch (dns_lookup(name, T_MX, 0, &mx_names, (VSTRING *) 0, why)) {
    default:
	smtp_errno = SMTP_RETRY;
	break;
    case DNS_FAIL:
	smtp_errno = SMTP_FAIL;
	break;
    case DNS_OK:
	mx_names = dns_rr_sort(mx_names, smtp_compare_mx);
	addr_list = smtp_addr_list(mx_names, why);
	dns_rr_free(mx_names);
	if (msg_verbose)
	    smtp_print_addr(name, addr_list);
	if ((self = smtp_find_self(addr_list)) != 0)
	    addr_list = smtp_truncate_self(addr_list, self->pref, name, why);
	break;
    case DNS_NOTFOUND:
	addr_list = smtp_host_addr(name, why);
	break;
    }

    /*
     * Clean up.
     */
    return (addr_list);
}

/* smtp_host_addr - direct host lookup */

DNS_RR *smtp_host_addr(char *host, VSTRING *why)
{
    DNS_FIXED fixed;
    DNS_RR *addr_list;
    struct in_addr addr;

    /*
     * If the host is specified by numerical address, just convert the
     * address to internal form. Otherwise, the host is specified by name.
     */
#define PREF0	0
    if (ISDIGIT(host[0]) && (addr.s_addr = inet_addr(host)) != INADDR_NONE) {
	fixed.type = fixed.class = fixed.ttl = fixed.length = 0;
	addr_list = dns_rr_create(host, &fixed, PREF0,
				  (char *) &addr, sizeof(addr));
    } else {
	addr_list = smtp_addr_one((DNS_RR *) 0, host, PREF0, why);
    }
    if (msg_verbose)
	smtp_print_addr(host, addr_list);
    return (addr_list);
}
