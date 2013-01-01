/*++
/* NAME
/*	postconf_master 3
/* SUMMARY
/*	support for master.cf
/* SYNOPSIS
/*	#include <postconf.h>
/*
/*	const char daemon_options_expecting_value[];
/*
/*	void	read_master(fail_on_open)
/*	int	fail_on_open;
/*
/*	void	show_master(fp, mode, filters)
/*	VSTREAM	*fp;
/*	int	mode;
/*	char	**filters;
/* DESCRIPTION
/*	read_master() reads entries from master.cf into memory.
/*
/*	show_master() writes the entries in the master.cf file
/*	to the specified stream.
/*
/*	daemon_options_expecting_value[] is an array of master.cf
/*	daemon command-line options that expect an option value.
/*
/*	Arguments
/* .IP fail_on_open
/*	Specify FAIL_ON_OPEN if open failure is a fatal error,
/*	WARN_ON_OPEN if a warning should be logged instead.
/* .IP fp
/*	Output stream.
/* .IP mode
/*	If the FOLD_LINE flag is set, show_master() wraps long
/*	output lines.
/* .IP filters
/*	A list of zero or more expressions in master_service(3)
/*	format. If no list is specified, show_master() outputs
/*	all master.cf entries in the specified order.
/* DIAGNOSTICS
/*	Problems are reported to the standard error stream.
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
#include <string.h>

/* Utility library. */

#include <msg.h>
#include <mymalloc.h>
#include <vstring.h>
#include <argv.h>
#include <vstream.h>
#include <readlline.h>
#include <stringops.h>

/* Global library. */

#include <mail_params.h>
#include <match_service.h>

/* Application-specific. */

#include <postconf.h>

const char daemon_options_expecting_value[] = "o";

#define STR(x) vstring_str(x)

/* normalize_options - bring options into canonical form */

static void normalize_options(ARGV *argv)
{
    int     field;
    char   *arg;
    char   *cp;
    char   *junk;

    /*
     * Normalize options to simplify later processing.
     */
    for (field = PC_MASTER_MIN_FIELDS; argv->argv[field] != 0; field++) {
	arg = argv->argv[field];
	if (arg[0] != '-' || strcmp(arg, "--") == 0)
	    break;
	for (cp = arg + 1; *cp; cp++) {
	    if (strchr(daemon_options_expecting_value, *cp) != 0
		&& cp > arg + 1) {
		/* Split "-stuffo" into "-stuff" and "-o". */
		junk = concatenate("-", cp, (char *) 0);
		argv_insert_one(argv, field + 1, junk);
		myfree(junk);
		*cp = 0;
		break;
	    }
	}
	if (strchr(daemon_options_expecting_value, arg[1]) == 0)
	    /* Option requires no value. */
	    continue;
	if (arg[2] != 0) {
	    /* Split "-oname=value" into "-o" "name=value". */
	    argv_insert_one(argv, field + 1, arg + 2);
	    arg[2] = 0;
	    field += 1;
	} else if (argv->argv[field + 1] != 0) {
	    /* Already in "-o" "name=value" form. */
	    field += 1;
	}
    }
}

/* read_master - read and digest the master.cf file */

void    read_master(int fail_on_open_error)
{
    const char *myname = "read_master";
    char   *path;
    VSTRING *buf;
    ARGV   *argv;
    VSTREAM *fp;
    int     entry_count = 0;
    int     line_count = 0;

    /*
     * Sanity check.
     */
    if (master_table != 0)
	msg_panic("%s: master table is already initialized", myname);

    /*
     * Get the location of master.cf.
     */
    if (var_config_dir == 0)
	set_config_dir();
    path = concatenate(var_config_dir, "/", MASTER_CONF_FILE, (char *) 0);

    /*
     * We can't use the master daemon's master_ent routines in their current
     * form. They convert everything to internal form, and they skip disabled
     * services.
     * 
     * The postconf command needs to show default fields as "-", and needs to
     * know about all service names so that it can generate service-dependent
     * parameter names (transport-dependent etc.).
     */
#define MASTER_BLANKS	" \t\r\n"		/* XXX */

    /*
     * Initialize the in-memory master table.
     */
    master_table = (PC_MASTER_ENT *) mymalloc(sizeof(*master_table));

    /*
     * Skip blank lines and comment lines. Degrade gracefully if master.cf is
     * not available, and master.cf is not the primary target.
     */
    if ((fp = vstream_fopen(path, O_RDONLY, 0)) == 0) {
	if (fail_on_open_error)
	    msg_fatal("open %s: %m", path);
	msg_warn("open %s: %m", path);
    } else {
	buf = vstring_alloc(100);
	while (readlline(buf, fp, &line_count) != 0) {
	    master_table = (PC_MASTER_ENT *) myrealloc((char *) master_table,
				 (entry_count + 2) * sizeof(*master_table));
	    argv = argv_split(STR(buf), MASTER_BLANKS);
	    if (argv->argc < PC_MASTER_MIN_FIELDS)
		msg_fatal("file %s: line %d: bad field count",
			  path, line_count);
	    normalize_options(argv);
	    master_table[entry_count].name_space =
		concatenate(argv->argv[0], ".", argv->argv[1], (char *) 0);
	    master_table[entry_count].argv = argv;
	    master_table[entry_count].valid_names = 0;
	    master_table[entry_count].all_params = 0;
	    entry_count += 1;
	}
	vstream_fclose(fp);
	vstring_free(buf);
    }

    /*
     * Null-terminate the master table and clean up.
     */
    master_table[entry_count].argv = 0;
    myfree(path);
}

/* print_master_line - print one master line */

static void print_master_line(VSTREAM *fp, int mode, PC_MASTER_ENT *masterp)
{
    char  **argv = masterp->argv->argv;
    const char *arg;
    const char *aval;
    int     arg_len;
    int     line_len;
    int     field;
    int     in_daemon_options;
    static int column_goal[] = {
	0,				/* service */
	11,				/* type */
	17,				/* private */
	25,				/* unpriv */
	33,				/* chroot */
	41,				/* wakeup */
	49,				/* maxproc */
	57,				/* command */
    };

#define ADD_TEXT(text, len) do { \
        vstream_fputs(text, fp); line_len += len; } \
    while (0)
#define ADD_SPACE ADD_TEXT(" ", 1)

    /*
     * Show the standard fields at their preferred column position. Use at
     * least one-space column separation.
     */
    for (line_len = 0, field = 0; field < PC_MASTER_MIN_FIELDS; field++) {
	arg = argv[field];
	if (line_len > 0) {
	    do {
		ADD_SPACE;
	    } while (line_len < column_goal[field]);
	}
	ADD_TEXT(arg, strlen(arg));
    }

    /*
     * Format the daemon command-line options and non-option arguments. Here,
     * we have no data-dependent preference for column positions, but we do
     * have argument grouping preferences.
     */
    in_daemon_options = 1;
    for ( /* void */ ; (arg = argv[field]) != 0; field++) {
	arg_len = strlen(arg);
	aval = 0;
	if (in_daemon_options) {

	    /*
	     * Try to show the generic options (-v -D) on the first line, and
	     * non-options on a later line.
	     */
	    if (arg[0] != '-' || strcmp(arg, "--") == 0) {
		in_daemon_options = 0;
		if ((mode & FOLD_LINE)
		    && line_len > column_goal[PC_MASTER_MIN_FIELDS - 1]) {
		    /* Force line wrap. */
		    line_len = LINE_LIMIT;
		}
	    }

	    /*
	     * Special processing for options that require a value.
	     */
	    else if (strchr(daemon_options_expecting_value, arg[1]) != 0
		     && (aval = argv[field + 1]) != 0) {

		/*
		 * Optionally, expand $name in parameter value.
		 */
		if (strcmp(arg, "-o") == 0
		    && (mode & SHOW_EVAL) != 0)
		    aval = expand_parameter_value((VSTRING *) 0, mode,
						  aval, masterp);

		/*
		 * Keep option and value on the same line.
		 */
		arg_len += strlen(aval) + 1;
	    }
	}

	/*
	 * Insert a line break when the next item won't fit.
	 */
	if (line_len > INDENT_LEN) {
	    if ((mode & FOLD_LINE) == 0
		|| line_len + 1 + arg_len < LINE_LIMIT) {
		ADD_SPACE;
	    } else {
		vstream_fputs("\n" INDENT_TEXT, fp);
		line_len = INDENT_LEN;
	    }
	}
	ADD_TEXT(arg, strlen(arg));
	if (aval) {
	    ADD_SPACE;
	    ADD_TEXT(aval, strlen(aval));
	    field += 1;
	}
    }
    vstream_fputs("\n", fp);
}

/* show_master - show master.cf entries */

void    show_master(VSTREAM *fp, int mode, char **filters)
{
    PC_MASTER_ENT *masterp;
    ARGV   *service_filter = 0;

    /*
     * Initialize the service filter.
     */
    if (filters[0])
	service_filter = match_service_init_argv(filters);

    /*
     * Iterate over the master table.
     */
    for (masterp = master_table; masterp->argv != 0; masterp++)
	if ((service_filter == 0
	     || match_service_match(service_filter, masterp->name_space))
	    && ((mode & SHOW_NONDEF) == 0 || masterp->all_params != 0))
	    print_master_line(fp, mode, masterp);

    /*
     * Cleanup.
     */
    if (service_filter != 0)
	argv_free(service_filter);
}
