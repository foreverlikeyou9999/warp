﻿/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * Daytime broadcast service.
 *
 * Copyright (c) 2010 Miru Limited.
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

#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef _WIN32
#	include <unistd.h>
#	include <pthread.h>
#else
#	include <process.h>
#	include <wchar.h>
#	include "getopt.h"
#	define snprintf		_snprintf
#endif
#include <pgm/pgm.h>


/* globals */
#define TIME_FORMAT	"%a, %d %b %Y %H:%M:%S %z"

static int		port = 0;
static const char*	network = "";
static bool		use_multicast_loop = FALSE;
static int		udp_encap_port = 0;

static int		max_tpdu = 1500;
static int		max_rte = 400*1000;		/* very conservative rate, 2.5mb/s */
static int		sqns = 100;

static bool		use_fec = FALSE;
static bool		use_ondemand_parity = FALSE;
static int		proactive_packets = 0;
static int		rs_k = 8;
static int		rs_n = 255;

static pgm_sock_t*	sock = NULL;
static bool		is_terminated = FALSE;

#ifndef _WIN32
static pthread_t	nak_thread;
static int		terminate_pipe[2];
static void on_signal (int);
static void* nak_routine (void*);
static void usage (const char*) __attribute__((__noreturn__));
#else
static HANDLE		nak_thread;
static HANDLE		terminate_event;
static BOOL on_console_ctrl (DWORD);
static unsigned __stdcall nak_routine (void*);
static void usage (const char*);
#endif

static bool on_startup (void);
static bool create_sock (void);
static bool create_nak_thread (void);


static void
usage (
	const char*	bin
	)
{
	fprintf (stderr, "Usage: %s [options]\n", bin);
	fprintf (stderr, "  -n <network>    : Multicast group or unicast IP address\n");
	fprintf (stderr, "  -s <port>       : IP port\n");
	fprintf (stderr, "  -p <port>       : Encapsulate PGM in UDP on IP port\n");
	fprintf (stderr, "  -r <rate>       : Regulate to rate bytes per second\n");
	fprintf (stderr, "  -f <type>       : Enable FEC: proactive, ondemand, or both\n");
	fprintf (stderr, "  -N <n>          : Reed-Solomon block size (255)\n");
	fprintf (stderr, "  -K <k>          : Reed-Solomon group size (8)\n");
	fprintf (stderr, "  -P <count>      : Number of pro-active parity packets (h)\n");
	fprintf (stderr, "  -l              : Enable multicast loopback and address sharing\n");
	fprintf (stderr, "  -i              : List available interfaces\n");
	exit (EXIT_SUCCESS);
}

int
main (
	int	argc,
	char   *argv[]
	)
{
	pgm_error_t* pgm_err = NULL;

	setlocale (LC_ALL, "");

	puts ("PGM daytime service");

	if (!pgm_init (&pgm_err)) {
		fprintf (stderr, "Unable to start PGM engine: %s\n", pgm_err->message);
		pgm_error_free (pgm_err);
		return EXIT_FAILURE;
	}

/* parse program arguments */
	const char* binary_name = strrchr (argv[0], '/');
	int c;
	while ((c = getopt (argc, argv, "s:n:p:r:f:N:K:P:lih")) != -1)
	{
		switch (c) {
		case 'n':	network = optarg; break;
		case 's':	port = atoi (optarg); break;
		case 'p':	udp_encap_port = atoi (optarg); break;
		case 'r':	max_rte = atoi (optarg); break;
		case 'f':
			use_fec = TRUE;
			switch (optarg[0]) {
			case 'p':
			case 'P':
				proactive_packets = 1;
				break;
			case 'b':
			case 'B':
				proactive_packets = 1;
			case 'o':
			case 'O':
				use_ondemand_parity = TRUE;
				break;
			}
			break;
		case 'N':	rs_n = atoi (optarg); break;
		case 'K':	rs_k = atoi (optarg); break;
		case 'P':	proactive_packets = atoi (optarg); break;

		case 'l':	use_multicast_loop = TRUE; break;

		case 'i':
			pgm_if_print_all();
			return EXIT_SUCCESS;

		case 'h':
		case '?':
			usage (binary_name);
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

/* service loop */
	do {
		time_t now;
		time (&now);
		const struct tm* time_ptr = localtime(&now);
#ifndef _WIN32
		char s[1024];
		const size_t slen = strftime (s, sizeof(s), TIME_FORMAT, time_ptr);
		const int status = pgm_send (sock, s, slen + 1, NULL);
#else
		char s[1024];
		const size_t slen = strftime (s, sizeof(s), TIME_FORMAT, time_ptr);
		wchar_t ws[1024];
		size_t wslen = MultiByteToWideChar (CP_ACP, 0, s, slen, ws, 1024);
		char us[1024];
		size_t uslen = WideCharToMultiByte (CP_UTF8, 0, ws, wslen + 1, us, sizeof(us), NULL, NULL);
		const int status = pgm_send (sock, us, uslen + 1, NULL);
#endif
	        if (PGM_IO_STATUS_NORMAL != status) {
			fprintf (stderr, "pgm_send() failed.\n");
		}
#ifndef _WIN32
		sleep (1);
#else
		Sleep (1 * 1000);
#endif
	} while (!is_terminated);

/* cleanup */
	puts ("Waiting for NAK thread.");
#ifndef _WIN32
	pthread_join (nak_thread, NULL);
	close (terminate_pipe[0]);
	close (terminate_pipe[1]);
#else
	WaitForSingleObject (nak_thread, INFINITE);
	CloseHandle (nak_thread);
	CloseHandle (terminate_event);
#endif /* !_WIN32 */

	if (sock) {
		puts ("Closing PGM sock.");
		pgm_close (sock, TRUE);
		sock = NULL;
	}

	puts ("PGM engine shutdown.");
	pgm_shutdown();
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
	bool status = (create_sock() && create_nak_thread());
	if (status)
		puts ("Startup complete.");
	return status;
}

static
bool
create_sock (void)
{
	struct pgm_addrinfo_t* res = NULL;
	pgm_error_t* pgm_err = NULL;
	sa_family_t sa_family = AF_UNSPEC;

/* parse network parameter into sock address structure */
	if (!pgm_getaddrinfo (network, NULL, &res, &pgm_err)) {
		fprintf (stderr, "Parsing network parameter: %s\n", pgm_err->message);
		goto err_abort;
	}

	sa_family = res->ai_send_addrs[0].gsr_group.ss_family;

	puts ("Create PGM socket.");
	if (udp_encap_port) {
		if (!pgm_socket (&sock, sa_family, SOCK_SEQPACKET, IPPROTO_UDP, &pgm_err)) {
			fprintf (stderr, "Creating PGM/UDP socket: %s\n", pgm_err->message);
			goto err_abort;
		}
		pgm_setsockopt (sock, PGM_UDP_ENCAP_UCAST_PORT, &udp_encap_port, sizeof(udp_encap_port));
		pgm_setsockopt (sock, PGM_UDP_ENCAP_MCAST_PORT, &udp_encap_port, sizeof(udp_encap_port));
	} else {
		if (!pgm_socket (&sock, sa_family, SOCK_SEQPACKET, IPPROTO_PGM, &pgm_err)) {
			fprintf (stderr, "Creating PGM/IP socket: %s\n", pgm_err->message);
			goto err_abort;
		}
	}

/* Use RFC 2113 tagging for PGM Router Assist */
	const int no_router_assist = 0;
	pgm_setsockopt (sock, PGM_IP_ROUTER_ALERT, &no_router_assist, sizeof(no_router_assist));

	pgm_drop_superuser();

/* set PGM parameters */
	const int send_only = 1,
		  ambient_spm = pgm_secs (30),
		  heartbeat_spm[] = { pgm_msecs (100),
				      pgm_msecs (100),
                                      pgm_msecs (100),
				      pgm_msecs (100),
				      pgm_msecs (1300),
				      pgm_secs  (7),
				      pgm_secs  (16),
				      pgm_secs  (25),
				      pgm_secs  (30) };

	pgm_setsockopt (sock, PGM_SEND_ONLY, &send_only, sizeof(send_only));
	pgm_setsockopt (sock, PGM_MTU, &max_tpdu, sizeof(max_tpdu));
	pgm_setsockopt (sock, PGM_TXW_SQNS, &sqns, sizeof(sqns));
	pgm_setsockopt (sock, PGM_TXW_MAX_RTE, &max_rte, sizeof(max_rte));
	pgm_setsockopt (sock, PGM_AMBIENT_SPM, &ambient_spm, sizeof(ambient_spm));
	pgm_setsockopt (sock, PGM_HEARTBEAT_SPM, &heartbeat_spm, sizeof(heartbeat_spm));
	if (use_fec) {
		struct pgm_fecinfo_t fecinfo; 
		fecinfo.block_size		= rs_n;
		fecinfo.proactive_packets	= proactive_packets;
		fecinfo.group_size		= rs_k;
		fecinfo.ondemand_parity_enabled	= use_ondemand_parity;
		fecinfo.var_pktlen_enabled	= TRUE;
		pgm_setsockopt (sock, PGM_USE_FEC, &fecinfo, sizeof(fecinfo));
	}

/* create global session identifier */
	struct pgm_sockaddr_t addr;
	memset (&addr, 0, sizeof(addr));
	addr.sa_port = port ? port : DEFAULT_DATA_DESTINATION_PORT;
	addr.sa_addr.sport = DEFAULT_DATA_SOURCE_PORT;
	if (!pgm_gsi_create_from_hostname (&addr.sa_addr.gsi, &pgm_err)) {
		fprintf (stderr, "Creating GSI: %s\n", pgm_err->message);
		goto err_abort;
	}

/* assign socket to specified address */
	if (!pgm_bind (sock, &addr, sizeof(addr), &pgm_err)) {
		fprintf (stderr, "Binding PGM socket: %s\n", pgm_err->message);
		goto err_abort;
	}

/* join IP multicast groups */
	for (unsigned i = 0; i < res->ai_recv_addrs_len; i++)
		pgm_setsockopt (sock, PGM_JOIN_GROUP, &res->ai_recv_addrs[i], sizeof(struct group_req));
	pgm_setsockopt (sock, PGM_SEND_GROUP, &res->ai_send_addrs[0], sizeof(struct group_req));
	pgm_freeaddrinfo (res);

/* set IP parameters */
	const int blocking = 0,
		  multicast_loop = use_multicast_loop ? 1 : 0,
		  multicast_hops = 16,
		  dscp = 0x2e << 2;		/* Expedited Forwarding PHB for network elements, no ECN. */

	pgm_setsockopt (sock, PGM_MULTICAST_LOOP, &multicast_loop, sizeof(multicast_loop));
	pgm_setsockopt (sock, PGM_MULTICAST_HOPS, &multicast_hops, sizeof(multicast_hops));
	pgm_setsockopt (sock, PGM_TOS, &dscp, sizeof(dscp));
	pgm_setsockopt (sock, PGM_NOBLOCK, &blocking, sizeof(blocking));

	if (!pgm_connect (sock, &pgm_err)) {
		fprintf (stderr, "Connecting PGM socket: %s\n", pgm_err->message);
		goto err_abort;
	}

	puts ("Startup complete.");
	return TRUE;

err_abort:
	if (NULL != sock) {
		pgm_close (sock, FALSE);
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
	if (NULL != sock) {
		pgm_close (sock, FALSE);
		sock = NULL;
	}
	return FALSE;
}

static
bool
create_nak_thread (void)
{
#ifndef _WIN32
	const int status = pthread_create (&nak_thread, NULL, &nak_routine, sock);
	if (0 != status) {
		fprintf (stderr, "Creating new thread: %s\n", strerror (status));
		return FALSE;
	}
#else
	nak_thread = (HANDLE)_beginthreadex (NULL, 0, &nak_routine, sock, 0, NULL);
	const int save_errno = errno;
	if (0 == nak_thread) {
		fprintf (stderr, "Creating new thread: %s\n", strerror (save_errno));
		return FALSE;
	}
#endif /* _WIN32 */
	return TRUE;
}

static
#ifndef _WIN32
void*
#else
unsigned
__stdcall
#endif
nak_routine (
	void*		arg
	)
{
/* dispatch loop */
	pgm_sock_t* nak_sock = (pgm_sock_t*)arg;
#ifndef _WIN32
	int fds;
	fd_set readfds;
#else
	int n_handles = 4, recv_sock, repair_sock, pending_sock;
	HANDLE waitHandles[ 4 ];
	DWORD dwTimeout, dwEvents;
	WSAEVENT recvEvent, repairEvent, pendingEvent;

	recvEvent = WSACreateEvent ();
	pgm_getsockopt (nak_sock, PGM_RECV_SOCK, &recv_sock, sizeof(recv_sock));
	WSAEventSelect (recv_sock, recvEvent, FD_READ);
	repairEvent = WSACreateEvent ();
	pgm_getsockopt (nak_sock, PGM_REPAIR_SOCK, &repair_sock, sizeof(repair_sock));
	WSAEventSelect (repair_sock, repairEvent, FD_READ);
	pendingEvent = WSACreateEvent ();
	pgm_getsockopt (nak_sock, PGM_PENDING_SOCK, &pending_sock, sizeof(pending_sock));
	WSAEventSelect (pending_sock, pendingEvent, FD_READ);

	waitHandles[0] = terminate_event;
	waitHandles[1] = recvEvent;
	waitHandles[2] = repairEvent;
	waitHandles[3] = pendingEvent;
#endif /* !_WIN32 */
	do {
		struct timeval tv;
		char buf[4064];
		pgm_error_t* pgm_err = NULL;
		const int status = pgm_recv (nak_sock, buf, sizeof(buf), 0, NULL, &pgm_err);
		switch (status) {
		case PGM_IO_STATUS_TIMER_PENDING:
			pgm_getsockopt (sock, PGM_TIME_REMAIN, &tv, sizeof(tv));
			goto block;
		case PGM_IO_STATUS_RATE_LIMITED:
			pgm_getsockopt (sock, PGM_RATE_REMAIN, &tv, sizeof(tv));
		case PGM_IO_STATUS_WOULD_BLOCK:
block:
#ifndef _WIN32
			fds = terminate_pipe[0] + 1;
			FD_ZERO(&readfds);
			FD_SET(terminate_pipe[0], &readfds);
			pgm_select_info (nak_sock, &readfds, NULL, &fds);
			fds = select (fds, &readfds, NULL, NULL, PGM_IO_STATUS_WOULD_BLOCK == status ? NULL : &tv);
#else
			dwTimeout = PGM_IO_STATUS_WOULD_BLOCK == status ? INFINITE : (DWORD)((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
			dwEvents = WaitForMultipleObjects (n_handles, waitHandles, FALSE, dwTimeout);
			switch (dwEvents) {
			case WAIT_OBJECT_0+1: WSAResetEvent (recvEvent); break;
			case WAIT_OBJECT_0+2: WSAResetEvent (repairEvent); break;
			case WAIT_OBJECT_0+3: WSAResetEvent (pendingEvent); break;
			default: break;
			}
#endif /* !_WIN32 */
			break;

		default:
			if (pgm_err) {
				fprintf (stderr, "%s\n", pgm_err->message ? pgm_err->message : "(null)");
				pgm_error_free (pgm_err);
				pgm_err = NULL;
			}
			if (PGM_IO_STATUS_ERROR == status)
				break;
		}
	} while (!is_terminated);
#ifndef _WIN32
	return NULL;
#else
	WSACloseEvent (recvEvent);
	WSACloseEvent (repairEvent);
	WSACloseEvent (pendingEvent);
	_endthread();
	return 0;
#endif
}

/* eof */
