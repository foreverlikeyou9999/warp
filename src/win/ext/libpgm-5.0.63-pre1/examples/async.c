﻿/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * Asynchronous queue for receiving packets in a separate managed thread.
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

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef _WIN32
#	include <fcntl.h>
#	include <unistd.h>
#	include <pthread.h>
#else
#	include <process.h>
#endif
#include <pgm/pgm.h>

#include "async.h"


/* locals */

struct async_event_t {
	struct async_event_t   *next, *prev;
	size_t			len;
	pgm_tsi_t		tsi;
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
	char			data[];
#elif defined(__cplusplus)
	char			data[1];
#else
	char			data[0];
#endif
};


static void on_data (async_t*const restrict, void*restrict, size_t, pgm_tsi_t*restrict);


/* queued data is stored as async_event_t objects
 */

static inline
struct async_event_t*
async_event_alloc (
	size_t			len
	)
{
	struct async_event_t* async_event;
	async_event = (struct async_event_t*)calloc (1, len + sizeof(struct async_event_t));
	async_event->len = len;
	return async_event;
}

static inline
void
async_event_unref (
        struct async_event_t* const async_event
        )
{
	free (async_event);
}

/* async_t implements a queue
 */

static inline
void
async_push_event (
	async_t*		restrict async,
	struct async_event_t*   restrict async_event
	)
{
	async_event->next = async->head;
	if (NULL != async->head)
		async->head->prev = async_event;
	else
		async->tail = async_event;
	async->head = async_event;
	async->length++;
}

static inline
struct async_event_t*
async_pop_event (
	async_t*		async
	)
{
	assert (NULL != async);
	if (NULL != async->tail)
	{
		struct async_event_t *async_event = async->tail;

		async->tail = async_event->prev;
		if (NULL != async->tail)
		{
			async->tail->next = NULL;
			async_event->prev = NULL;
		}
		else
			async->head = NULL;
		async->length--;

		return async_event;
	}

	return NULL;
}

/* asynchronous receiver thread, sits in a loop processing incoming packets
 */

static
#ifndef _WIN32
void*
#else
unsigned
__stdcall
#endif
receiver_routine (
	void*		arg
	)
{
	assert (NULL != arg);
	async_t* async = (async_t*)arg;
	assert (NULL != async->sock);
#ifndef _WIN32
	int fds;
	fd_set readfds;
#else
	int n_handles = 3, recv_sock, pending_sock;
	HANDLE waitHandles[ 3 ];
	DWORD dwTimeout, dwEvents;
	WSAEVENT recvEvent, pendingEvent;

	recvEvent = WSACreateEvent ();
	pgm_getsockopt (async->sock, PGM_RECV_SOCK, &recv_sock, sizeof(recv_sock));
	WSAEventSelect (recv_sock, recvEvent, FD_READ);
	pendingEvent = WSACreateEvent ();
	pgm_getsockopt (async->sock, PGM_PENDING_SOCK, &pending_sock, sizeof(pending_sock));
	WSAEventSelect (pending_sock, pendingEvent, FD_READ);

	waitHandles[0] = async->destroy_event;
	waitHandles[1] = recvEvent;
	waitHandles[2] = pendingEvent;
#endif /* !_WIN32 */

/* dispatch loop */
	do {
		struct timeval tv;
		char buffer[4096];
		size_t len;
		pgm_tsi_t from;
		const int status = pgm_recvfrom (async->sock,
						 buffer,
						 sizeof(buffer),
						 0,
						 &len,
						 &from,
						 NULL);
		switch (status) {
		case PGM_IO_STATUS_NORMAL:
			on_data (async, buffer, len, &from);
			break;
		case PGM_IO_STATUS_TIMER_PENDING:
			pgm_getsockopt (async->sock, PGM_TIME_REMAIN, &tv, sizeof(tv));
			goto block;
		case PGM_IO_STATUS_RATE_LIMITED:
			pgm_getsockopt (async->sock, PGM_RATE_REMAIN, &tv, sizeof(tv));
		case PGM_IO_STATUS_WOULD_BLOCK:
/* select for next event */
block:
#ifndef _WIN32
			fds = async->destroy_pipe[0] + 1;
			FD_ZERO(&readfds);
			FD_SET(async->destroy_pipe[0], &readfds);
			pgm_select_info (async->sock, &readfds, NULL, &fds);
			fds = select (fds, &readfds, NULL, NULL, PGM_IO_STATUS_WOULD_BLOCK == status ? NULL : &tv);
#else
			dwTimeout = PGM_IO_STATUS_WOULD_BLOCK == status ? INFINITE : (DWORD)((tv.tv_sec * 1000) + (tv.
tv_usec / 1000));
			dwEvents = WaitForMultipleObjects (n_handles, waitHandles, FALSE, dwTimeout);
			switch (dwEvents) {
			case WAIT_OBJECT_0+1: WSAResetEvent (recvEvent); break;
			case WAIT_OBJECT_0+2: WSAResetEvent (pendingEvent); break;
			default: break;
			}
#endif /* !_WIN32 */
			break;

		default:
			if (PGM_IO_STATUS_ERROR == status)
				break;
		}
	} while (!async->is_destroyed);

/* cleanup */
#ifndef _WIN32
	return NULL;
#else
	WSACloseEvent (recvEvent);
	WSACloseEvent (pendingEvent);
	_endthread();
	return 0;
#endif /* !_WIN32 */
}

/* enqueue a new data event.
 */

static
void
on_data (
	async_t*const     restrict async,
	void*	   	  restrict data,
	size_t			   len,
	pgm_tsi_t* 	  restrict from
	)
{
	struct async_event_t* async_event = async_event_alloc (len);
	memcpy (&async_event->tsi, from, sizeof(pgm_tsi_t));
	memcpy (&async_event->data, data, len);
#ifndef _WIN32
	pthread_mutex_lock (&async->pthread_mutex);
	async_push_event (async, async_event);
	if (1 == async->length) {
		const char one = '1';
		const size_t writelen = write (async->notify_pipe[1], &one, sizeof(one));
		assert (sizeof(one) == writelen);
	}
	pthread_mutex_unlock (&async->pthread_mutex);
#else
	WaitForSingleObject (async->win32_mutex, INFINITE);
	async_push_event (async, async_event);
	if (1 == async->length) {
		SetEvent (async->notify_event);
	}
	ReleaseMutex (async->win32_mutex);
#endif /* _WIN32 */
}

/* create asynchronous thread handler from bound PGM sock.
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set appropriately.
 */

int
async_create (
	async_t**         restrict async,
	pgm_sock_t* const restrict sock
	)
{
	async_t* new_async;

	if (NULL == async || NULL == sock) {
		errno = EINVAL;
		return -1;
	}

	new_async = (async_t*)calloc (1, sizeof(async_t));
	new_async->sock = sock;
#ifndef _WIN32
	int e;
	e = pthread_mutex_init (&new_async->pthread_mutex, NULL);
	if (0 != e) goto err_destroy;
	e = pipe (new_async->notify_pipe);
	const int flags = fcntl (new_async->notify_pipe[0], F_GETFL);
	fcntl (new_async->notify_pipe[0], F_SETFL, flags | O_NONBLOCK);
	if (0 != e) goto err_destroy;
	e = pipe (new_async->destroy_pipe);
	if (0 != e) goto err_destroy;
	const int status = pthread_create (&new_async->thread, NULL, &receiver_routine, new_async);
	if (0 != status) goto err_destroy;
#else
	new_async->win32_mutex = CreateMutex (NULL, FALSE, NULL);
	new_async->notify_event = CreateEvent (NULL, TRUE, FALSE, TEXT("AsyncNotify"));
	new_async->destroy_event = CreateEvent (NULL, TRUE, FALSE, TEXT("AsyncDestroy"));
	new_async->thread = (HANDLE)_beginthreadex (NULL, 0, &receiver_routine, new_async, 0, NULL);
	if (0 == new_async->thread) goto err_destroy;
#endif /* _WIN32 */

/* return new object */
	*async = new_async;
	return 0;

err_destroy:
#ifndef _WIN32
	close (new_async->destroy_pipe[0]);
	close (new_async->destroy_pipe[1]);
	close (new_async->notify_pipe[0]);
	close (new_async->notify_pipe[1]);
	pthread_mutex_destroy (&new_async->pthread_mutex);
#else
	CloseHandle (new_async->destroy_event);
	CloseHandle (new_async->notify_event);
	CloseHandle (new_async->win32_mutex);
#endif /* _WIN32 */
	if (new_async)
		free (new_async);
	return -1;
}

/* Destroy asynchronous receiver, there must be no active queue consumer.
 *
 * on success, 0 is returned, on error -1 is returned and errno set appropriately.
 */

int
async_destroy (
	async_t* const	async
	)
{
	if (NULL == async || async->is_destroyed) {
		errno = EINVAL;
		return -1;
	}

	async->is_destroyed = TRUE;
#ifndef _WIN32
	const char one = '1';
	const size_t writelen = write (async->destroy_pipe[1], &one, sizeof(one));
	assert (sizeof(one) == writelen);
	pthread_join (async->thread, NULL);
	close (async->destroy_pipe[0]);
	close (async->destroy_pipe[1]);
	close (async->notify_pipe[0]);
	close (async->notify_pipe[1]);
	pthread_mutex_destroy (&async->pthread_mutex);
#else
	SetEvent (async->destroy_event);
	WaitForSingleObject (async->thread, INFINITE);
	CloseHandle (async->thread);
	CloseHandle (async->destroy_event);
	CloseHandle (async->notify_event);
	CloseHandle (async->win32_mutex);
#endif /* !_WIN32 */
	while (NULL != async->head) {
		struct async_event_t *next = async->head->next;
		async_event_unref (async->head);
		async->head = next;
		async->length--;
	}
	free (async);
	return 0;
}

/* synchronous reading from the queue.
 *
 * returns GIOStatus with success, error, again, or eof.
 */

ssize_t
async_recvfrom (
	async_t* const restrict async,
	void*	       restrict	buf,
	size_t			len,
	pgm_tsi_t*     restrict from
	)
{
	struct async_event_t* async_event;

	if (NULL == async || NULL == buf || async->is_destroyed) {
		errno = EINVAL;
		return -1;
	}

#ifndef _WIN32
	pthread_mutex_lock (&async->pthread_mutex);
	if (0 == async->length) {
/* flush event pipe */
		char tmp;
		while (sizeof(tmp) == read (async->notify_pipe[0], &tmp, sizeof(tmp)));
		pthread_mutex_unlock (&async->pthread_mutex);
		errno = EAGAIN;
		return -1;
	}
	async_event = async_pop_event (async);
	pthread_mutex_unlock (&async->pthread_mutex);
#else
	WaitForSingleObject (async->win32_mutex, INFINITE);
	if (0 == async->length) {
/* clear event */
		ResetEvent (async->notify_event);
		ReleaseMutex (async->win32_mutex);
		errno = EAGAIN;
		return -1;
	}
	async_event = async_pop_event (async);
	ReleaseMutex (async->win32_mutex);
#endif /* _WIN32 */
	assert (NULL != async_event);

/* pass data back to callee */
	const size_t event_len = MIN(async_event->len, len);
	if (NULL != from) {
		memcpy (from, &async_event->tsi, sizeof(pgm_tsi_t));
	}
	memcpy (buf, async_event->data, event_len);
	async_event_unref (async_event);
	return event_len;
}

ssize_t
async_recv (
	async_t* const restrict async,
	void*	       restrict buf,
	size_t			len
	)
{
	return async_recvfrom (async, buf, len, NULL);
}

/* eof */
