/*++
/* NAME
/*	smtp-source 8
/* SUMMARY
/*	SMTP test generator
/* SYNOPSIS
/*	smtp-source [options] host[:port]
/* DESCRIPTION
/*	smtp-source connects to the named host and port (default 25)
/*	and sends one or more little messages to it, either sequentially
/*	or in parallel.
/*
/*	Options:
/* .IP -c
/*	Display a running counter that is incremented each time
/*	an SMTP DATA command completes.
/* .IP "-C count"
/*	When a host sends RESET instead of SYN|ACK, try \fIcount\fR times
/*	before giving up. The default count is 1. Specify a larger count in
/*	order to work around a problem with TCP/IP stacks that send RESET
/*	when the listen queue is full.
/* .IP -d
/*	Don't disconnect after sending a message; send the next
/*	message over the same connection.
/* .IP "-f from"
/*	Use the specified sender address (default: <foo@myhostname>).
/* .IP -o
/*	Old mode: don't send HELO, and don't send message headers.
/* .IP "-l length"
/*	Send \fIlength\fR bytes as message payload.
/* .IP "-m message_count"
/*	Send the specified number of messages (default: 1).
/* .IP "-r recipient_count"
/*	Send the specified number of recipients per transaction (default: 1).
/*	Recipient names are generated by appending a number to the
/*	recipient address. The default is one recipient per transaction.
/* .IP "-s session_count"
/*	Run the specified number of SMTP sessions in parallel (default: 1).
/* .IP "-t to"
/*	Use the specified recipient address (default: <foo@myhostname>).
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
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <setjmp.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

/* Utility library. */

#include <msg.h>
#include <vstring.h>
#include <vstream.h>
#include <vstring_vstream.h>
#include <get_hostname.h>
#include <split_at.h>
#include <connect.h>
#include <mymalloc.h>
#include <events.h>
#include <find_inet.h>

/* Global library. */

#include <smtp_stream.h>
#include <mail_date.h>

/* Application-specific. */

 /*
  * Per-session data structure with state.
  */
typedef struct {
    int     xfer_count;			/* # of xfers in session */
    int     rcpt_count;			/* # of recipients to go */
    VSTREAM *stream;			/* open connection */
    int     fd;				/* ditto */
    int     connect_count;		/* # of connect()s to retry */
} SESSION;

 /*
  * Structure with broken-up SMTP server response.
  */
typedef struct {			/* server response */
    int     code;			/* status */
    char   *str;			/* text */
    VSTRING *buf;			/* origin of text */
} RESPONSE;

static VSTRING *buffer;
static int var_line_limit = 10240;
static int var_timeout = 300;
static const char *var_myhostname;
static int session_count;
static int message_count = 1;
static struct sockaddr_in sin;
static int recipients = 1;
static char *defaddr;
static char *recipient;
static char *sender;
static char *message_data;
static int message_length;
static int disconnect = 1;
static int count = 0;
static int counter = 0;
static int send_helo_first = 1;
static int send_headers = 1;
static int connect_count = 1;

static void connect_done(int, char *);
static void send_helo(SESSION *);
static void helo_done(int, char *);
static void send_mail(SESSION *);
static void mail_done(int, char *);
static void send_rcpt(int, char *);
static void rcpt_done(int, char *);
static void send_data(int, char *);
static void data_done(int, char *);
static void dot_done(int, char *);
static void send_quit(SESSION *);
static void quit_done(int, char *);

/* command - send an SMTP command */

static void command(VSTREAM *stream, char *fmt,...)
{
    VSTRING *buf;
    va_list ap;

    /*
     * Optionally, log the command before actually sending, so we can see
     * what the program is trying to do.
     */
    if (msg_verbose) {
	buf = vstring_alloc(100);
	va_start(ap, fmt);
	vstring_vsprintf(buf, fmt, ap);
	va_end(ap);
	msg_info("%s", vstring_str(buf));
	vstring_free(buf);
    }
    va_start(ap, fmt);
    smtp_vprintf(stream, fmt, ap);
    va_end(ap);
}

/* response - read and process SMTP server response */

static RESPONSE *response(VSTREAM *stream, VSTRING *buf)
{
    static RESPONSE rdata;
    int     more;
    char   *cp;

    /*
     * Initialize the response data buffer. Defend against a denial of
     * service attack by limiting the amount of multi-line text that we are
     * willing to store.
     */
    if (rdata.buf == 0) {
	rdata.buf = vstring_alloc(100);
	vstring_ctl(rdata.buf, VSTRING_CTL_MAXLEN, var_line_limit, 0);
    }

    /*
     * Censor out non-printable characters in server responses. Concatenate
     * multi-line server responses. Separate the status code from the text.
     * Leave further parsing up to the application.
     */
#define BUF ((char *) vstring_str(buf))
    VSTRING_RESET(rdata.buf);
    for (;;) {
	smtp_get(buf, stream, var_line_limit);
	for (cp = BUF; *cp != 0; cp++)
	    if (!ISPRINT(*cp) && !ISSPACE(*cp))
		*cp = '?';
	cp = BUF;
	if (msg_verbose)
	    msg_info("<<< %s", cp);
	while (ISDIGIT(*cp))
	    cp++;
	rdata.code = (cp - BUF == 3 ? atoi(BUF) : 0);
	if ((more = (*cp == '-')) != 0)
	    cp++;
	while (ISSPACE(*cp))
	    cp++;
	vstring_strcat(rdata.buf, cp);
	if (more == 0)
	    break;
	VSTRING_ADDCH(rdata.buf, '\n');
    }
    VSTRING_TERMINATE(rdata.buf);
    rdata.str = vstring_str(rdata.buf);
    return (&rdata);
}

/* exception_text - translate exceptions from the smtp_stream module */

static char *exception_text(int except)
{
    switch (except) {
	case SMTP_ERR_EOF:
	return ("lost connection");
    case SMTP_ERR_TIME:
	return ("timeout");
    default:
	msg_panic("exception_text: unknown exception %d", except);
    }
    /* NOTREACHED */
}

/* startup - connect to server but do not wait */

static void startup(SESSION *session)
{
    if (message_count-- <= 0) {
	myfree((char *) session);
	session_count--;
	return;
    }
    if (session->stream == 0) {
	if ((session->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	    msg_fatal("socket: %m");
	for (;;) {
	    if (session->connect_count == 0)
		msg_fatal("connect: %m");
	    if (!connect(session->fd, (struct sockaddr *) & sin, sizeof(sin)))
		break;
	    if (session->connect_count-- > 1)
		usleep(10);
	}
	session->stream = vstream_fdopen(session->fd, O_RDWR);
	smtp_timeout_setup(session->stream, var_timeout);
	event_enable_read(session->fd, connect_done, (char *) session);
    } else {
	send_mail(session);
    }
}

/* connect_done - send message sender info */

static void connect_done(int unused_event, char *context)
{
    SESSION *session = (SESSION *) context;
    RESPONSE *resp;
    int     except;

    /*
     * Prepare for disaster.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while reading HELO", exception_text(except));

    /*
     * Read and parse the server's SMTP greeting banner.
     */
    if (((resp = response(session->stream, buffer))->code / 100) != 2)
	msg_fatal("bad startup: %d %s", resp->code, resp->str);

    /*
     * Send helo or send the envelope sender address.
     */
    if (send_helo_first)
	send_helo(session);
    else
	send_mail(session);
}

/* send_helo - send hostname */

static void send_helo(SESSION *session)
{
    int     except;

    /*
     * Send the standard greeting with our hostname
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending HELO", exception_text(except));

    command(session->stream, "HELO %s", var_myhostname);

    /*
     * Prepare for the next event.
     */
    event_disable_readwrite(session->fd);
    event_enable_read(session->fd, helo_done, (char *) session);
}

/* helo_done - handle HELO response */

static void helo_done(int unused, char *context)
{
    SESSION *session = (SESSION *) context;
    RESPONSE *resp;
    int     except;

    /*
     * Get response to HELO command.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending HELO", exception_text(except));

    if ((resp = response(session->stream, buffer))->code / 100 != 2)
	msg_fatal("HELO rejected: %d %s", resp->code, resp->str);

    send_mail(session);
}

/* send_mail - send envelope sender */

static void send_mail(SESSION *session)
{
    int     except;

    /*
     * Send the envelope sender address.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending sender", exception_text(except));

    command(session->stream, "MAIL FROM:<%s>", sender);

    /*
     * Prepare for the next event.
     */
    event_disable_readwrite(session->fd);
    event_enable_read(session->fd, mail_done, (char *) session);
}

/* mail_done - handle MAIL response */

static void mail_done(int unused, char *context)
{
    SESSION *session = (SESSION *) context;
    RESPONSE *resp;
    int     except;

    /*
     * Get response to MAIL command.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending sender", exception_text(except));

    if ((resp = response(session->stream, buffer))->code / 100 != 2)
	msg_fatal("sender rejected: %d %s", resp->code, resp->str);

    session->rcpt_count = recipients;
    send_rcpt(unused, context);
}

/* send_rcpt - send recipient address */

static void send_rcpt(int unused_event, char *context)
{
    SESSION *session = (SESSION *) context;
    int     except;

    /*
     * Send envelope recipient address.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending recipient", exception_text(except));

    if (session->rcpt_count > 1)
	command(session->stream, "RCPT TO:<%d%s>",
		session->rcpt_count, recipient);
    else
	command(session->stream, "RCPT TO:<%s>", recipient);
    session->rcpt_count--;

    /*
     * Prepare for the next event.
     */
    event_disable_readwrite(session->fd);
    event_enable_read(session->fd, rcpt_done, (char *) session);
}

/* rcpt_done - handle RCPT completion */

static void rcpt_done(int unused, char *context)
{
    SESSION *session = (SESSION *) context;
    RESPONSE *resp;
    int     except;

    /*
     * Get response to RCPT command.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending recipient", exception_text(except));

    if ((resp = response(session->stream, buffer))->code / 100 != 2)
	msg_fatal("recipient rejected: %d %s", resp->code, resp->str);

    /*
     * Send another RCPT command or send DATA.
     */
    if (session->rcpt_count > 0)
	send_rcpt(unused, context);
    else
	send_data(unused, context);
}

/* send_data - send DATA command */

static void send_data(int unused_event, char *context)
{
    SESSION *session = (SESSION *) context;
    int     except;

    /*
     * Request data transmission.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending DATA command", exception_text(except));
    command(session->stream, "DATA");

    /*
     * Prepare for the next event.
     */
    event_disable_readwrite(session->fd);
    event_enable_read(session->fd, data_done, (char *) session);
}

/* data_done - send message content */

static void data_done(int unused_event, char *context)
{
    SESSION *session = (SESSION *) context;
    RESPONSE *resp;
    int     except;
    static const char *mydate;
    static int mypid;

    /*
     * Get response to DATA command.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending DATA command", exception_text(except));
    if ((resp = response(session->stream, buffer))->code != 354)
	msg_fatal("data %d %s", resp->code, resp->str);

    /*
     * Send basic header to keep mailers that bother to examine them happy.
     */
    if (send_headers) {
	if (mydate == 0) {
	    mydate = mail_date(time((time_t *) 0));
	    mypid = getpid();
	}
	smtp_printf(session->stream, "From: <%s>", sender);
	smtp_printf(session->stream, "To: <%s>", recipient);
	smtp_printf(session->stream, "Date: %s", mydate);
	smtp_printf(session->stream, "Message-Id: <%04x.%04x.%04x@%s>",
		    mypid, session->fd, message_count, var_myhostname);
	smtp_fputs("", 0, session->stream);
    }

    /*
     * Send some garbage.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending message", exception_text(except));
    if (message_length == 0) {
	smtp_fputs("La de da de da 1.", 17, session->stream);
	smtp_fputs("La de da de da 2.", 17, session->stream);
	smtp_fputs("La de da de da 3.", 17, session->stream);
	smtp_fputs("La de da de da 4.", 17, session->stream);
    } else {
	smtp_fputs(message_data, message_length, session->stream);
    }

    /*
     * Send end of message and process the server response.
     */
    command(session->stream, ".");

    /*
     * Update the running counter.
     */
    if (count) {
	counter++;
	vstream_printf("%d\r", counter);
	vstream_fflush(VSTREAM_OUT);
    }

    /*
     * Prepare for the next event.
     */
    event_disable_readwrite(session->fd);
    event_enable_read(session->fd, dot_done, (char *) session);
}

/* dot_done - send QUIT */

static void dot_done(int unused_event, char *context)
{
    SESSION *session = (SESSION *) context;
    RESPONSE *resp;
    int     except;

    /*
     * Get response to "." command.
     */
    if ((except = setjmp(smtp_timeout_buf)) != 0)
	msg_fatal("%s while sending message", exception_text(except));
    if ((resp = response(session->stream, buffer))->code / 100 != 2)
	msg_fatal("data %d %s", resp->code, resp->str);
    session->xfer_count++;

    /*
     * Say goodbye or send the next message.
     */
    if (disconnect || message_count < 1) {
	send_quit(session);
    } else {
	event_disable_readwrite(session->fd);
	startup(session);
    }
}

/* send_quit - send QUIT command */

static void send_quit(SESSION *session)
{
    command(session->stream, "QUIT");
    event_disable_readwrite(session->fd);
    event_enable_read(session->fd, quit_done, (char *) session);
}

/* quit_done - disconnect */

static void quit_done(int unused_event, char *context)
{
    SESSION *session = (SESSION *) context;

    (void) response(session->stream, buffer);
    event_disable_readwrite(session->fd);
    vstream_fclose(session->stream);
    session->stream = 0;
    startup(session);
}

/* usage - explain */

static void usage(char *myname)
{
    msg_fatal("usage: %s -s sess -l msglen -m msgs -c -C count -d -f from -o -t to -v host[:port]", myname);
}

/* main - parse JCL and start the machine */

int     main(int argc, char **argv)
{
    SESSION *session;
    char   *host;
    char   *port;
    int     sessions = 1;
    int     ch;
    int     i;

    signal(SIGPIPE, SIG_IGN);

    /*
     * Parse JCL.
     */
    while ((ch = GETOPT(argc, argv, "cC:df:l:m:or:s:t:v")) > 0) {
	switch (ch) {
	case 'c':
	    count++;
	    break;
	case 'C':
	    if ((connect_count = atoi(optarg)) <= 0)
		usage(argv[0]);
	    break;
	case 'd':
	    disconnect = 0;
	    break;
	case 'f':
	    sender = optarg;
	    break;
	case 'l':
	    if ((message_length = atoi(optarg)) <= 0)
		usage(argv[0]);
	    message_data = mymalloc(message_length);
	    memset(message_data, 'X', message_length);
	    for (i = 80; i < message_length; i += 80) {
		message_data[i - 2] = '\r';
		message_data[i - 1] = '\n';
	    }
	    break;
	case 'm':
	    if ((message_count = atoi(optarg)) <= 0)
		usage(argv[0]);
	    break;
	case 'o':
	    send_helo_first = 0;
	    send_headers = 0;
	    break;
	case 'r':
	    if ((recipients = atoi(optarg)) <= 0)
		usage(argv[0]);
	    break;
	case 's':
	    if ((sessions = atoi(optarg)) <= 0)
		usage(argv[0]);
	    break;
	case 't':
	    recipient = optarg;
	    break;
	case 'v':
	    msg_verbose++;
	    break;
	default:
	    usage(argv[0]);
	}
    }
    if (argc - optind != 1)
	usage(argv[0]);
    if ((port = split_at(host = argv[optind], ':')) == 0)
	port = "smtp";

    /*
     * Translate endpoint address to internal form.
     */
    memset((char *) &sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = find_inet_addr(host);
    sin.sin_port = find_inet_port(port, "tcp");

    /*
     * Make sure the SMTP server cannot run us out of memory by sending
     * never-ending lines of text.
     */
    if (buffer == 0) {
	buffer = vstring_alloc(100);
	vstring_ctl(buffer, VSTRING_CTL_MAXLEN, var_line_limit, 0);
    }

    /*
     * Make sure we have sender and recipient addresses.
     */
    var_myhostname = get_hostname();
    if (sender == 0 || recipient == 0) {
	vstring_sprintf(buffer, "foo@%s", var_myhostname);
	defaddr = mystrdup(vstring_str(buffer));
	if (sender == 0)
	    sender = defaddr;
	if (recipient == 0)
	    recipient = defaddr;
    }

    /*
     * Start sessions.
     */
    while (sessions-- > 0) {
	session = (SESSION *) mymalloc(sizeof(*session));
	session->stream = 0;
	session->xfer_count = 0;
	session->connect_count = connect_count;
	session_count++;
	startup(session);
    }
    for (;;) {
	event_loop(-1);
	if (session_count <= 0 && message_count <= 0) {
	    if (count) {
		VSTREAM_PUTC('\n', VSTREAM_OUT);
		vstream_fflush(VSTREAM_OUT);
	    }
	    exit(0);
	}
    }
}
