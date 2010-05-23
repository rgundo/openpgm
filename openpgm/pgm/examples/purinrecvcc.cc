/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * プリン PGM receiver
 *
 * Copyright (c) 2006-2010 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <cassert>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#	include <unistd.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#else
#	include "getopt.h"
#	define snprintf		_snprintf
#endif
#include <pgm/pgm.hh>


/* globals */

static int		port = 0;
static const char*	network = "";
static bool		use_multicast_loop = FALSE;
static int		udp_encap_port = 0;

static int		max_tpdu = 1500;
static int		sqns = 100;

static bool		use_fec = FALSE;
static int		rs_k = 8;
static int		rs_n = 255;

static ip::pgm::endpoint* endpoint = NULL;
static ip::pgm::socket*	sock = NULL;
static bool		is_terminated = FALSE;

#ifndef _WIN32
static int		terminate_pipe[2];
static void on_signal (int);
#else
static HANDLE		terminate_event;
static BOOL on_console_ctrl (DWORD);
#endif
#ifndef _MSC_VER
static void usage (const char*) __attribute__((__noreturn__));
#else
static void usage (const char*);
#endif

static bool on_startup (void);
static int on_data (const void*, size_t, const ip::pgm::endpoint&);


static void
usage (
	const char*	bin
	)
{
	fprintf (stderr, "Usage: %s [options]\n", bin);
	fprintf (stderr, "  -n <network>    : Multicast group or unicast IP address\n");
	fprintf (stderr, "  -s <port>       : IP port\n");
	fprintf (stderr, "  -p <port>       : Encapsulate PGM in UDP on IP port\n");
	fprintf (stderr, "  -f <type>       : Enable FEC with either proactive or ondemand parity\n");
	fprintf (stderr, "  -K <k>          : Configure Reed-Solomon code (n, k)\n");
	fprintf (stderr, "  -N <n>\n");
	fprintf (stderr, "  -l              : Enable multicast loopback and address sharing\n");
	fprintf (stderr, "  -i              : List available interfaces\n");
	exit (EXIT_SUCCESS);
}

int
main (
	int		argc,
	char*		argv[]
	)
{
	cpgm::pgm_error_t* pgm_err = NULL;

	setlocale (LC_ALL, "");

#ifndef _WIN32
	puts ("プリン プリン");
#else
	_putws (L"プリン プリン");
#endif

	if (!pgm_init (&pgm_err)) {
		fprintf (stderr, "Unable to start PGM engine: %s\n", pgm_err->message);
		pgm_error_free (pgm_err);
		return EXIT_FAILURE;
	}

/* parse program arguments */
	const char* binary_name = std::strrchr (argv[0], '/');
	int c;
	while ((c = getopt (argc, argv, "s:n:p:f:K:N:lih")) != -1)
	{
		switch (c) {
		case 'n':	network = optarg; break;
		case 's':	port = atoi (optarg); break;
		case 'p':	udp_encap_port = atoi (optarg); break;
		case 'f':	use_fec = TRUE; break;
		case 'K':	rs_k = atoi (optarg); break;
		case 'N':	rs_n = atoi (optarg); break;
		case 'l':	use_multicast_loop = TRUE; break;

		case 'i':
			cpgm::pgm_if_print_all();
			return EXIT_SUCCESS;

		case 'h':
		case '?': usage (binary_name);
		}
	}

	if (use_fec && ( !rs_n || !rs_k )) {
		fprintf (stderr, "Invalid Reed-Solomon parameters RS(%d,%d).\n", rs_n, rs_k);
		usage (binary_name);
	}

/* setup signal handlers */
#ifdef SIGHUP
	signal (SIGHUP,  SIG_IGN);
#endif
#ifndef _WIN32
	int e = pipe (terminate_pipe);
	assert (0 == e);
	signal (SIGINT,  on_signal);
	signal (SIGTERM, on_signal);
#else
	terminate_event = CreateEvent (NULL, TRUE, FALSE, TEXT("TerminateEvent"));
	SetConsoleCtrlHandler ((PHANDLER_ROUTINE)on_console_ctrl, TRUE);
#endif /* !_WIN32 */

	if (!on_startup()) {
		fprintf (stderr, "Startup failed\n");
		return EXIT_FAILURE;
	}

/* dispatch loop */
#ifndef _WIN32
	int fds;
	fd_set readfds;
#else
	int n_handles = 3, recv_sock, pending_sock;
	HANDLE waitHandles[ 3 ];
	DWORD dwTimeout, dwEvents;
	WSAEVENT recvEvent, pendingEvent;

	recvEvent = WSACreateEvent ();
	sock->get_option (cpgm::PGM_RECV_SOCK, &recv_sock, sizeof(recv_sock));
	WSAEventSelect (recv_sock, recvEvent, FD_READ);
	pendingEvent = WSACreateEvent ();
	sock->get_option (cpgm::PGM_PENDING_SOCK, &pending_sock, sizeof(pending_sock));
	WSAEventSelect (pending_sock, pendingEvent, FD_READ);

	waitHandles[0] = terminate_event;
	waitHandles[1] = recvEvent;
	waitHandles[2] = pendingEvent;
#endif /* !_WIN32 */
	puts ("Entering PGM message loop ... ");
	do {
		socklen_t optlen;
		struct timeval tv;
		char buffer[4096];
		size_t len;
		ip::pgm::endpoint from;
		const int status = sock->receive_from (buffer,
						       sizeof(buffer),
						       0,
						       &len,
						       &from,
						       &pgm_err);
		switch (status) {
		case cpgm::PGM_IO_STATUS_NORMAL:
			on_data (buffer, len, from);
			break;
		case cpgm::PGM_IO_STATUS_TIMER_PENDING:
			optlen = sizeof (tv);
			sock->get_option (cpgm::PGM_TIME_REMAIN, &tv, &optlen);
			goto block;
		case cpgm::PGM_IO_STATUS_RATE_LIMITED:
			optlen = sizeof (tv);
			sock->get_option (cpgm::PGM_RATE_REMAIN, &tv, &optlen);
		case cpgm::PGM_IO_STATUS_WOULD_BLOCK:
/* select for next event */
block:
#ifndef _WIN32
			fds = terminate_pipe[0] + 1;
			FD_ZERO(&readfds);
			FD_SET(terminate_pipe[0], &readfds);
			pgm_select_info (sock->native(), &readfds, NULL, &fds);
			fds = select (fds, &readfds, NULL, NULL, cpgm::PGM_IO_STATUS_WOULD_BLOCK == status ? NULL : &tv);
#else
			dwTimeout = cpgm::PGM_IO_STATUS_WOULD_BLOCK == status ? INFINITE : (DWORD)((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
			dwEvents = WaitForMultipleObjects (n_handles, waitHandles, FALSE, dwTimeout);
			switch (dwEvents) {
			case WAIT_OBJECT_0+1: WSAResetEvent (recvEvent); break;
			case WAIT_OBJECT_0+2: WSAResetEvent (pendingEvent); break;
			default: break;
			}
#endif /* !_WIN32 */
			break;

		default:
			if (pgm_err) {
				fprintf (stderr, "%s\n", pgm_err->message);
				pgm_error_free (pgm_err);
				pgm_err = NULL;
			}
			if (cpgm::PGM_IO_STATUS_ERROR == status)
				break;
		}
	} while (!is_terminated);

	puts ("Message loop terminated, cleaning up.");

/* cleanup */
#ifndef _WIN32
	close (terminate_pipe[0]);
	close (terminate_pipe[1]);
#else
	WSACloseEvent (recvEvent);
	WSACloseEvent (pendingEvent);
	CloseHandle (terminate_event);
#endif /* !_WIN32 */

	if (sock) {
		puts ("Closing PGM socket.");
		sock->close (TRUE);
		sock = NULL;
	}

	puts ("PGM engine shutdown.");
	cpgm::pgm_shutdown ();
	puts ("finished.");
	return EXIT_SUCCESS;
}

#ifndef _WIN32
static
void
on_signal (
	int		signum
	)
{
	printf ("on_signal (signum:%d)\n", signum);
	is_terminated = TRUE;
	const char one = '1';
	const size_t writelen = write (terminate_pipe[1], &one, sizeof(one));
	assert (sizeof(one) == writelen);
}
#else
static
BOOL
on_console_ctrl (
	DWORD		dwCtrlType
	)
{
	printf ("on_console_ctrl (dwCtrlType:%lu)\n", (unsigned long)dwCtrlType);
	is_terminated = TRUE;
	SetEvent (terminate_event);
	return TRUE;
}
#endif /* !_WIN32 */

static
bool
on_startup (void)
{
	struct cpgm::pgm_addrinfo_t* res = NULL;
	cpgm::pgm_error_t* pgm_err = NULL;
	sa_family_t sa_family = AF_UNSPEC;

/* parse network parameter into PGM socket address structure */
	if (!pgm_getaddrinfo (network, NULL, &res, &pgm_err)) {
		fprintf (stderr, "Parsing network parameter: %s\n", pgm_err->message);
		goto err_abort;
	}

	sa_family = res->ai_send_addrs[0].gsr_group.ss_family;

	sock = new ip::pgm::socket();

	if (udp_encap_port) {
		puts ("Create PGM/UDP socket.");
		if (!sock->open (sa_family, SOCK_SEQPACKET, IPPROTO_UDP, &pgm_err)) {
			fprintf (stderr, "Creating PGM/UDP socket: %s\n", pgm_err->message);
			goto err_abort;
		}
		sock->set_option (cpgm::PGM_UDP_ENCAP_UCAST_PORT, &udp_encap_port, sizeof(udp_encap_port));
		sock->set_option (cpgm::PGM_UDP_ENCAP_MCAST_PORT, &udp_encap_port, sizeof(udp_encap_port));
	} else {
		puts ("Create PGM/IP socket.");
		if (!sock->open (sa_family, SOCK_SEQPACKET, IPPROTO_PGM, &pgm_err)) {
			fprintf (stderr, "Creating PGM/IP socket: %s\n", pgm_err->message);
			goto err_abort;
		}
	}

	{
/* Use RFC 2113 tagging for PGM Router Assist */
		const int no_router_assist = 0;
		sock->set_option (cpgm::PGM_IP_ROUTER_ALERT, &no_router_assist, sizeof(no_router_assist));
	}

	cpgm::pgm_drop_superuser();

	{
/* set PGM parameters */
		const int recv_only = 1,
			  passive = 0,
			  peer_expiry = (300*1000000UL) /* pgm_secs (300) */,
			  spmr_expiry = (250*1000UL) /* pgm_msecs (250) */,
			  nak_bo_ivl = (50*1000UL) /* pgm_msecs (50) */,
			  nak_rpt_ivl = (2*1000000UL) /* pgm_secs (2) */,
			  nak_rdata_ivl = (2*1000000UL) /* pgm_secs (2) */,
			  nak_data_retries = 50,
			  nak_ncf_retries = 50;

		sock->set_option (cpgm::PGM_RECV_ONLY, &recv_only, sizeof(recv_only));
		sock->set_option (cpgm::PGM_PASSIVE, &passive, sizeof(passive));
		sock->set_option (cpgm::PGM_MTU, &max_tpdu, sizeof(max_tpdu));
		sock->set_option (cpgm::PGM_RXW_SQNS, &sqns, sizeof(sqns));
		sock->set_option (cpgm::PGM_PEER_EXPIRY, &peer_expiry, sizeof(peer_expiry));
		sock->set_option (cpgm::PGM_SPMR_EXPIRY, &spmr_expiry, sizeof(spmr_expiry));
		sock->set_option (cpgm::PGM_NAK_BO_IVL, &nak_bo_ivl, sizeof(nak_bo_ivl));
		sock->set_option (cpgm::PGM_NAK_RPT_IVL, &nak_rpt_ivl, sizeof(nak_rpt_ivl));
		sock->set_option (cpgm::PGM_NAK_RDATA_IVL, &nak_rdata_ivl, sizeof(nak_rdata_ivl));
		sock->set_option (cpgm::PGM_NAK_DATA_RETRIES, &nak_data_retries, sizeof(nak_data_retries));
		sock->set_option (cpgm::PGM_NAK_NCF_RETRIES, &nak_ncf_retries, sizeof(nak_ncf_retries));
	}
	if (use_fec) {
		struct cpgm::pgm_fecinfo_t fecinfo;
		fecinfo.block_size		= rs_n;
		fecinfo.proactive_packets	= 0;
		fecinfo.group_size		= rs_k;
		fecinfo.ondemand_parity_enabled	= TRUE;
		fecinfo.var_pktlen_enabled	= FALSE;
		sock->set_option (cpgm::PGM_USE_FEC, &fecinfo, sizeof(fecinfo));
	}

/* create global session identifier */
	endpoint = new ip::pgm::endpoint (DEFAULT_DATA_DESTINATION_PORT);

/* assign socket to specified address */
	if (!sock->bind (*endpoint, &pgm_err)) {
		fprintf (stderr, "Binding PGM socket: %s\n", pgm_err->message);
		goto err_abort;
	}

/* join IP multicast groups */
	for (unsigned i = 0; i < res->ai_recv_addrs_len; i++)
		sock->set_option (cpgm::PGM_JOIN_GROUP, &res->ai_recv_addrs[i], sizeof(struct group_req));
	sock->set_option (cpgm::PGM_SEND_GROUP, &res->ai_send_addrs[0], sizeof(struct group_req));
	pgm_freeaddrinfo (res);

	{
/* set IP parameters */
		const int nonblocking = 1,
			  multicast_loop = use_multicast_loop ? 1 : 0,
			  multicast_hops = 16,
			  dscp = 0x2e << 2;		/* Expedited Forwarding PHB for network elements, no ECN. */

		sock->set_option (cpgm::PGM_MULTICAST_LOOP, &multicast_loop, sizeof(multicast_loop));
		sock->set_option (cpgm::PGM_MULTICAST_HOPS, &multicast_hops, sizeof(multicast_hops));
		sock->set_option (cpgm::PGM_TOS, &dscp, sizeof(dscp));
		sock->set_option (cpgm::PGM_NOBLOCK, &nonblocking, sizeof(nonblocking));
	}

	if (!sock->connect (&pgm_err)) {
		fprintf (stderr, "Connecting PGM socket: %s\n", pgm_err->message);
		goto err_abort;
	}

	puts ("Startup complete.");
	return TRUE;

err_abort:
	if (NULL != sock) {
		sock->close (FALSE);
		sock = NULL;
	}
	if (NULL != res) {
		pgm_freeaddrinfo (res);
		res = NULL;
	}
	if (NULL != pgm_err) {
		pgm_error_free (pgm_err);
		pgm_err = NULL;
	}
	return FALSE;
}

static
int
on_data (
	const void*       		data,
	size_t				len,
	const ip::pgm::endpoint&	from
	)
{
/* protect against non-null terminated strings */
	char buf[1024], tsi[PGM_TSISTRLEN];
	const size_t buflen = MIN(sizeof(buf) - 1, len);
	strncpy (buf, (char*)data, buflen);
	buf[buflen] = '\0';
	cpgm::pgm_tsi_print_r (from.address(), tsi, sizeof(tsi));
#ifndef _MSC_VER
	printf ("\"%s\" (%zu bytes from %s)\n",
			buf, len, tsi);
#else
/* Microsoft CRT will crash on %zu */
	printf ("\"%s\" (%u bytes from %s)\n",
			buf, (unsigned)len, tsi);
#endif
	return 0;
}

/* eof */