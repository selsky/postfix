/*++
/* NAME
/*	resolve 3
/* SUMMARY
/*	mail address resolver
/* SYNOPSIS
/*	#include "trivial-rewrite.h"
/*
/*	void	resolve_init(void)
/*
/*	void	resolve_proto(stream)
/*	VSTREAM	*stream;
/*
/*	void	resolve_addr(rule, addr, result)
/*	char	*rule;
/*	char	*addr;
/*	VSTRING *result;
/* DESCRIPTION
/*	This module implements the trivial address resolving engine.
/*	It distinguishes between local and remote mail, and optionally
/*	consults one or more transport tables that map a destination
/*	to a transport, nexthop pair.
/*
/*	resolve_init() initializes data structures that are private
/*	to this module. It should be called once before using the
/*	actual resolver routines.
/*
/*	resolve_proto() implements the client-server protocol:
/*	read one address in FQDN form, reply with a (transport,
/*	nexthop, internalized recipient) triple.
/*
/*	resolve_addr() gives direct access to the address resolving
/*	engine. It resolves an internalized address to a (transport,
/*	nexthop, internalized recipient) triple.
/* STANDARDS
/* DIAGNOSTICS
/*	Problems and transactions are logged to the syslog daemon.
/* BUGS
/* SEE ALSO
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
#include <stdlib.h>
#include <string.h>

/* Utility library. */

#include <msg.h>
#include <vstring.h>
#include <vstream.h>
#include <vstring_vstream.h>
#include <split_at.h>
#include <valid_hostname.h>
#include <stringops.h>

/* Global library. */

#include <mail_params.h>
#include <mail_proto.h>
#include <mail_addr.h>
#include <rewrite_clnt.h>
#include <resolve_local.h>
#include <mail_conf.h>
#include <quote_822_local.h>
#include <tok822.h>
#include <domain_list.h>
#include <string_list.h>
#include <match_parent_style.h>
#include <maps.h>
#include <mail_addr_find.h>

/* Application-specific. */

#include "trivial-rewrite.h"
#include "transport.h"

#define STR	vstring_str

static DOMAIN_LIST *relay_domains;
static STRING_LIST *virt_alias_doms;
static STRING_LIST *virt_mailbox_doms;
static MAPS *relocated_maps;

/* resolve_addr - resolve address according to rule set */

void    resolve_addr(char *addr, VSTRING *channel, VSTRING *nexthop,
		             VSTRING *nextrcpt, int *flags)
{
    char   *myname = "resolve_addr";
    VSTRING *addr_buf = vstring_alloc(100);
    TOK822 *tree;
    TOK822 *saved_domain = 0;
    TOK822 *domain = 0;
    char   *destination;
    const char *blame = 0;
    const char *rcpt_domain;

    *flags = 0;

    /*
     * The address is in internalized (unquoted) form, so we must externalize
     * it first before we can parse it.
     * 
     * While quoting the address local part, do not treat @ as a special
     * character. This allows us to detect extra @ characters and block
     * source routed relay attempts.
     * 
     * But practically, we have to look at the unquoted form so that routing
     * characters like @ remain visible, in order to stop user@domain@domain
     * relay attempts when forwarding mail to a primary Sendmail MX host.
     */
    if (var_resolve_dequoted) {
	tree = tok822_scan_addr(addr);
    } else {
	quote_822_local(addr_buf, addr);
	tree = tok822_scan_addr(vstring_str(addr_buf));
    }

    /*
     * Preliminary resolver: strip off all instances of the local domain.
     * Terminate when no destination domain is left over, or when the
     * destination domain is remote.
     */
#define RESOLVE_LOCAL(domain) \
    resolve_local(STR(tok822_internalize(addr_buf, domain, TOK822_STR_DEFL)))

    while (tree->head) {

	/*
	 * Strip trailing dot at end of domain, but not dot-dot. This merely
	 * makes diagnostics more accurate by leaving bogus addresses alone.
	 */
	if (tree->tail->type == '.'
	    && tok822_rfind_type(tree->tail, '@') != 0
	    && tree->tail->prev->type != '.')
	    tok822_free_tree(tok822_sub_keep_before(tree, tree->tail));

	/*
	 * Strip trailing @.
	 */
	if (tree->tail->type == '@') {
	    tok822_free_tree(tok822_sub_keep_before(tree, tree->tail));
	    continue;
	}

	/*
	 * A lone empty string becomes the postmaster.
	 */
	if (tree->head == tree->tail && tree->head->type == TOK822_QSTRING
	    && VSTRING_LEN(tree->head->vstr) == 0) {
	    tok822_free(tree->head);
	    tree->head = tok822_scan(MAIL_ADDR_POSTMASTER, &tree->tail);
	    rewrite_tree(REWRITE_CANON, tree);
	}

	/*
	 * Strip (and save) @domain if local.
	 */
	if ((domain = tok822_rfind_type(tree->tail, '@')) != 0) {
	    if (RESOLVE_LOCAL(domain->next) == 0)
		break;
	    tok822_sub_keep_before(tree, domain);
	    if (saved_domain)
		tok822_free_tree(saved_domain);
	    saved_domain = domain;
	}

	/*
	 * After stripping the local domain, if any, replace foo%bar by
	 * foo@bar, site!user by user@site, rewrite to canonical form, and
	 * retry.
	 * 
	 * Otherwise we're done.
	 */
	if (tok822_rfind_type(tree->tail, '@')
	    || (var_swap_bangpath && tok822_rfind_type(tree->tail, '!'))
	    || (var_percent_hack && tok822_rfind_type(tree->tail, '%'))) {
	    rewrite_tree(REWRITE_CANON, tree);
	} else {
	    domain = 0;
	    break;
	}
    }

    /*
     * If the destination is non-local, recognize routing operators in the
     * address localpart. This is needed to prevent backup MX hosts from
     * relaying third-party destinations through primary MX hosts, otherwise
     * the backup host could end up on black lists. Ignore local
     * swap_bangpath and percent_hack settings because we can't know how the
     * primary MX host is set up.
     */
    if (domain && domain->prev)
	if (tok822_rfind_type(domain->prev, '@') != 0
	    || tok822_rfind_type(domain->prev, '!') != 0
	    || tok822_rfind_type(domain->prev, '%') != 0)
	    *flags |= RESOLVE_FLAG_ROUTED;

    /*
     * Make sure the resolved envelope recipient has the user@domain form. If
     * no domain was specified in the address, assume the local machine. See
     * above for what happens with an empty address.
     */
    if (domain == 0) {
	if (saved_domain) {
	    tok822_sub_append(tree, saved_domain);
	    saved_domain = 0;
	} else {				/* Aargh! Always! */
	    tok822_sub_append(tree, tok822_alloc('@', (char *) 0));
	    tok822_sub_append(tree, tok822_scan(var_myhostname, (TOK822 **) 0));
	}
    }
    tok822_internalize(nextrcpt, tree, TOK822_STR_DEFL);

    /*
     * With relay or other non-local destinations, the relayhost setting
     * overrides the destination domain name.
     * 
     * With virtual, relay, or other non-local destinations, give the highest
     * precedence to delivery transport associated next-hop information.
     * 
     * XXX Nag if the domain is listed in multiple domain lists. The effect is
     * implementation defined, and may break when internals change.
     */
    dict_errno = 0;
    if (domain != 0) {
	tok822_internalize(nexthop, domain->next, TOK822_STR_DEFL);
	lowercase(STR(nexthop));
	if (STR(nexthop)[strspn(STR(nexthop), "[]0123456789.")] != 0
	    && valid_hostname(STR(nexthop), DONT_GRIPE) == 0)
	    *flags |= RESOLVE_FLAG_ERROR;
	if (virt_alias_doms
	    && string_list_match(virt_alias_doms, STR(nexthop))) {
	    if (virt_mailbox_doms
		&& string_list_match(virt_mailbox_doms, STR(nexthop)))
		msg_warn("do not list domain %s in BOTH %s and %s",
		  STR(nexthop), VAR_VIRT_ALIAS_DOMS, VAR_VIRT_MAILBOX_DOMS);
	    vstring_strcpy(channel, var_error_transport);
	    vstring_strcpy(nexthop, "User unknown");
	    blame = VAR_ERROR_TRANSPORT;
	    *flags |= RESOLVE_CLASS_ALIAS;
	} else if (dict_errno != 0) {
	    msg_warn("%s lookup failure", VAR_VIRT_ALIAS_DOMS);
	    *flags |= RESOLVE_FLAG_FAIL;
	} else if (virt_mailbox_doms
		   && string_list_match(virt_mailbox_doms, STR(nexthop))) {
	    vstring_strcpy(channel, var_virt_transport);
	    blame = VAR_VIRT_TRANSPORT;
	    *flags |= RESOLVE_CLASS_VIRTUAL;
	} else if (dict_errno != 0) {
	    msg_warn("%s lookup failure", VAR_VIRT_MAILBOX_DOMS);
	    *flags |= RESOLVE_FLAG_FAIL;
	} else {
	    if (relay_domains
		&& domain_list_match(relay_domains, STR(nexthop))) {
		vstring_strcpy(channel, var_relay_transport);
		blame = VAR_RELAY_TRANSPORT;
		*flags |= RESOLVE_CLASS_RELAY;
	    } else if (dict_errno != 0) {
		msg_warn("%s lookup failure", VAR_RELAY_DOMAINS);
		*flags |= RESOLVE_FLAG_FAIL;
	    } else {
		vstring_strcpy(channel, var_def_transport);
		blame = VAR_DEF_TRANSPORT;
		*flags |= RESOLVE_CLASS_DEFAULT;
	    }
	    if (*var_relayhost)
		vstring_strcpy(nexthop, var_relayhost);
	}
	if ((destination = split_at(STR(channel), ':')) != 0 && *destination)
	    vstring_strcpy(nexthop, destination);
    }

    /*
     * Local delivery. Set up the default local transport and the default
     * next-hop hostname (myself).
     * 
     * XXX Nag if the domain is listed in multiple domain lists. The effect is
     * implementation defined, and may break when internals change.
     */
    else {
	if ((rcpt_domain = strrchr(STR(nextrcpt), '@')) != 0) {
	    rcpt_domain++;
	    if (virt_alias_doms
		&& string_list_match(virt_alias_doms, rcpt_domain))
		msg_warn("do not list domain %s in BOTH %s and %s",
			 rcpt_domain, VAR_MYDEST, VAR_VIRT_ALIAS_DOMS);
	    if (virt_mailbox_doms
		&& string_list_match(virt_mailbox_doms, rcpt_domain))
		msg_warn("do not list domain %s in BOTH %s and %s",
			 rcpt_domain, VAR_MYDEST, VAR_VIRT_MAILBOX_DOMS);
	}
	vstring_strcpy(channel, var_local_transport);
	blame = VAR_LOCAL_TRANSPORT;
	if ((destination = split_at(STR(channel), ':')) == 0
	    || *destination == 0)
	    destination = var_myhostname;
	vstring_strcpy(nexthop, destination);
	*flags |= RESOLVE_CLASS_LOCAL;
    }

    /*
     * Sanity checks.
     */
    if ((*flags & RESOLVE_FLAG_FAIL) == 0) {
	if (*STR(channel) == 0) {
	    if (blame == 0)
		msg_panic("%s: null blame", myname);
	    msg_warn("file %s/%s: parameter %s: null transport is not allowed",
		     var_config_dir, MAIN_CONF_FILE, blame);
	    *flags |= RESOLVE_FLAG_FAIL;
	}
	if (*STR(nexthop) == 0)
	    msg_panic("%s: null nexthop", myname);
    }

    /*
     * Bounce recipients that have moved. We do it here instead of in the
     * local delivery agent. The benefit is that we can bounce mail for
     * virtual addresses, not just local addresses only, and that there is no
     * need to run a local delivery agent just for the sake of relocation
     * notices. The downside is that this table has no effect on local alias
     * expansion results, so that mail will have to make almost an entire
     * iteration through the mail system.
     */
#define IGNORE_ADDR_EXTENSION   ((char **) 0)

    if ((*flags & RESOLVE_FLAG_FAIL) == 0 && relocated_maps != 0) {
	const char *newloc;

	if ((newloc = mail_addr_find(relocated_maps, STR(nextrcpt),
				     IGNORE_ADDR_EXTENSION)) != 0) {
	    vstring_strcpy(channel, var_error_transport);
	    vstring_sprintf(nexthop, "user has moved to %s", newloc);
	} else if (dict_errno != 0) {
	    msg_warn("%s lookup failure", VAR_RELOCATED_MAPS);
	    *flags |= RESOLVE_FLAG_FAIL;
	}
    }

    /*
     * The transport map overrides any transport and next-hop host info that
     * is set up above.
     * 
     * XXX Don't override the error transport :-(
     */
    if ((*flags & RESOLVE_FLAG_FAIL) == 0
	&& *var_transport_maps
	&& strcmp(STR(channel), var_error_transport) != 0) {
	if (transport_lookup(STR(nextrcpt), channel, nexthop) == 0
	    && dict_errno != 0) {
	    msg_warn("%s lookup failure", VAR_TRANSPORT_MAPS);
	    *flags |= RESOLVE_FLAG_FAIL;
	}
    }

    /*
     * Clean up.
     */
    if (saved_domain)
	tok822_free_tree(saved_domain);
    tok822_free_tree(tree);
    vstring_free(addr_buf);
}

/* Static, so they can be used by the network protocol interface only. */

static VSTRING *channel;
static VSTRING *nexthop;
static VSTRING *nextrcpt;
static VSTRING *query;

/* resolve_proto - read request and send reply */

int     resolve_proto(VSTREAM *stream)
{
    int     flags;

    if (attr_scan(stream, ATTR_FLAG_STRICT,
		  ATTR_TYPE_STR, MAIL_ATTR_ADDR, query,
		  ATTR_TYPE_END) != 1)
	return (-1);

    resolve_addr(STR(query), channel, nexthop, nextrcpt, &flags);

    if (msg_verbose)
	msg_info("%s -> (`%s' `%s' `%s' `%d')", STR(query), STR(channel),
		 STR(nexthop), STR(nextrcpt), flags);

    attr_print(stream, ATTR_FLAG_NONE,
	       ATTR_TYPE_STR, MAIL_ATTR_TRANSPORT, STR(channel),
	       ATTR_TYPE_STR, MAIL_ATTR_NEXTHOP, STR(nexthop),
	       ATTR_TYPE_STR, MAIL_ATTR_RECIP, STR(nextrcpt),
	       ATTR_TYPE_NUM, MAIL_ATTR_FLAGS, flags,
	       ATTR_TYPE_END);

    if (vstream_fflush(stream) != 0) {
	msg_warn("write resolver reply: %m");
	return (-1);
    }
    return (0);
}

/* resolve_init - module initializations */

void    resolve_init(void)
{
    query = vstring_alloc(100);
    channel = vstring_alloc(100);
    nexthop = vstring_alloc(100);
    nextrcpt = vstring_alloc(100);

    if (*var_virt_alias_doms)
	virt_alias_doms =
	    string_list_init(MATCH_FLAG_NONE, var_virt_alias_doms);

    if (*var_virt_mailbox_doms)
	virt_mailbox_doms =
	    string_list_init(MATCH_FLAG_NONE, var_virt_mailbox_doms);

    if (*var_relay_domains)
	relay_domains =
	    domain_list_init(match_parent_style(VAR_RELAY_DOMAINS),
			     var_relay_domains);

    if (*var_relocated_maps)
	relocated_maps =
	    maps_create(VAR_RELOCATED_MAPS, var_relocated_maps,
			DICT_FLAG_LOCK);
}
