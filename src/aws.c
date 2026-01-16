// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;
	size_t remaining = BUFSIZ - 1 - conn->request_path_len;

	if (len > remaining)
		len = remaining;
	memcpy(conn->request_path + conn->request_path_len, buf, len);
	conn->request_path_len += len;
	conn->request_path[conn->request_path_len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	int len = snprintf(conn->send_buffer, BUFSIZ,
					   "HTTP/1.1 200 OK\r\n"
					   "Content-Length: %zu\r\n"
					   "Connection: close\r\n"
					   "\r\n",
					   conn->file_size);

	if (len < 0)
		len = 0;
	conn->send_len = (size_t)len;
	conn->send_pos = 0;
}

static void connection_prepare_send_404(struct connection *conn)
{
	int len = snprintf(conn->send_buffer, BUFSIZ,
					   "HTTP/1.1 404 Not Found\r\n"
					   "Content-Length: 0\r\n"
					   "Connection: close\r\n"
					   "\r\n");

	if (len < 0)
		len = 0;
	conn->send_len = (size_t)len;
	conn->send_pos = 0;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	const char *path = conn->request_path;
	const char *static_prefix = "/" AWS_REL_STATIC_FOLDER;
	const char *dynamic_prefix = "/" AWS_REL_DYNAMIC_FOLDER;

	if (!conn->have_path)
		return RESOURCE_TYPE_NONE;

	if (strncmp(path, static_prefix, strlen(static_prefix)) == 0) {
		snprintf(conn->filename, sizeof(conn->filename), "%s%s",
				 AWS_ABS_STATIC_FOLDER, path + strlen(static_prefix));
		return RESOURCE_TYPE_STATIC;
	}

	if (strncmp(path, dynamic_prefix, strlen(dynamic_prefix)) == 0) {
		snprintf(conn->filename, sizeof(conn->filename), "%s%s",
				 AWS_ABS_DYNAMIC_FOLDER, path + strlen(dynamic_prefix));
		return RESOURCE_TYPE_DYNAMIC;
	}

	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	struct connection *conn = calloc(1, sizeof(*conn));

	DIE(conn == NULL, "calloc");

	conn->sockfd = sockfd;
	conn->fd = -1;
	conn->eventfd = eventfd(0, EFD_NONBLOCK);
	DIE(conn->eventfd < 0, "eventfd");
	conn->ctx = ctx;
	conn->state = STATE_INITIAL;
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;

	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	int rc;

	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, BUFSIZ,
				  conn->file_pos);
	io_set_eventfd(&conn->iocb, conn->eventfd);
	conn->iocb.data = conn;
	conn->piocb[0] = &conn->iocb;
	rc = io_submit(conn->ctx, 1, conn->piocb);
	if (rc < 0) {
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}
	conn->state = STATE_ASYNC_ONGOING;
}

void connection_remove(struct connection *conn)
{
	if (conn == NULL)
		return;
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	w_epoll_remove_ptr(epollfd, conn->eventfd, conn);
	if (conn->fd >= 0)
		close(conn->fd);
	if (conn->eventfd >= 0)
		close(conn->eventfd);
	if (conn->sockfd >= 0)
		close(conn->sockfd);
	free(conn);
}

void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;
	int flags;

	sockfd = accept(listenfd, (SSA *)&addr, &addrlen);
	DIE(sockfd < 0, "accept");

	flags = fcntl(sockfd, F_GETFL);
	DIE(flags < 0, "fcntl");
	rc = fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
	DIE(rc < 0, "fcntl");

	conn = connection_create(sockfd);

	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_in");
	rc = w_epoll_add_ptr_in(epollfd, conn->eventfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_in");

	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
}

void receive_data(struct connection *conn)
{
	ssize_t bytes_recv;
	size_t space;

	for (;;) {
		if (conn->recv_len >= BUFSIZ - 1) {
			conn->state = STATE_CONNECTION_CLOSED;
			return;
		}
		space = BUFSIZ - 1 - conn->recv_len;
		bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
						  space, 0);
		if (bytes_recv < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			conn->state = STATE_CONNECTION_CLOSED;
			return;
		}
		if (bytes_recv == 0) {
			if (conn->recv_len == 0) {
				conn->state = STATE_CONNECTION_CLOSED;
				return;
			}
			break;
		}
		conn->recv_len += bytes_recv;
	}
	conn->recv_buffer[conn->recv_len] = '\0';
}

int connection_open_file(struct connection *conn)
{
	struct stat st;

	conn->res_type = connection_get_resource_type(conn);
	if (conn->res_type == RESOURCE_TYPE_NONE)
		return -1;

	conn->fd = open(conn->filename, O_RDONLY);
	if (conn->fd < 0)
		return -1;
	if (fstat(conn->fd, &st) < 0)
		return -1;
	conn->file_size = st.st_size;
	conn->file_pos = 0;

	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	struct io_event event;
	uint64_t completed;
	int rc;

	rc = read(conn->eventfd, &completed, sizeof(completed));
	if (rc < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) {
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}

	rc = io_getevents(conn->ctx, 1, 1, &event, NULL);
	if (rc <= 0) {
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}
	if ((ssize_t)event.res < 0) {
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}
	if (event.res == 0) {
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}

	conn->async_read_len = event.res;
	conn->send_len = conn->async_read_len;
	conn->send_pos = 0;
	conn->file_pos += conn->async_read_len;
	conn->state = STATE_SENDING_DATA;
}

int parse_header(struct connection *conn)
{
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0};
	size_t nparsed;

	conn->have_path = 0;
	conn->request_path_len = 0;
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
	nparsed = http_parser_execute(&conn->request_parser, &settings_on_path,
								  conn->recv_buffer, conn->recv_len);
	if (nparsed == 0 || !conn->have_path)
		return -1;
	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	ssize_t bytes_sent;
	off_t offset = (off_t)conn->file_pos;
	size_t remaining = conn->file_size - conn->file_pos;

	bytes_sent = sendfile(conn->sockfd, conn->fd, &offset, remaining);
	if (bytes_sent < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return STATE_SENDING_DATA;
		return STATE_CONNECTION_CLOSED;
	}
	if (bytes_sent == 0)
		return STATE_CONNECTION_CLOSED;

	conn->file_pos = offset;
	if (conn->file_pos >= conn->file_size)
		return STATE_DATA_SENT;

	return STATE_SENDING_DATA;
}

int connection_send_data(struct connection *conn)
{
	ssize_t bytes_sent;
	size_t remaining;

	if (conn->send_pos >= conn->send_len)
		return 0;

	remaining = conn->send_len - conn->send_pos;
	bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos,
					  remaining, 0);
	if (bytes_sent < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -1;
	}
	if (bytes_sent == 0)
		return -1;

	conn->send_pos += bytes_sent;
	return bytes_sent;
}

int connection_send_dynamic(struct connection *conn)
{
	connection_start_async_io(conn);
	if (conn->state != STATE_ASYNC_ONGOING)
		return -1;
	return 0;
}

void handle_input(struct connection *conn)
{
	switch (conn->state) {
	case STATE_INITIAL:
	case STATE_RECEIVING_DATA:
		receive_data(conn);
		if (conn->state == STATE_CONNECTION_CLOSED)
			break;
		if (strstr(conn->recv_buffer, "\r\n\r\n") == NULL) {
			conn->state = STATE_RECEIVING_DATA;
			break;
		}
		if (parse_header(conn) < 0) {
			connection_prepare_send_404(conn);
			conn->state = STATE_SENDING_404;
			w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			break;
		}
		if (connection_open_file(conn) < 0) {
			connection_prepare_send_404(conn);
			conn->state = STATE_SENDING_404;
			w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			break;
		}
		connection_prepare_send_reply_header(conn);
		conn->state = STATE_SENDING_HEADER;
		w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		break;
	case STATE_ASYNC_ONGOING:
		connection_complete_async_io(conn);
		if (conn->state == STATE_SENDING_DATA)
			w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		break;
	default:
		printf("shouldn't get here %d\n", conn->state);
	}
}

void handle_output(struct connection *conn)
{
	switch (conn->state) {
	case STATE_SENDING_HEADER:
		if (connection_send_data(conn) < 0) {
			conn->state = STATE_CONNECTION_CLOSED;
			break;
		}
		if (conn->send_pos < conn->send_len)
			break;
		conn->send_len = 0;
		conn->send_pos = 0;
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			conn->state = STATE_SENDING_DATA;
		} else if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
			if (connection_send_dynamic(conn) < 0) {
				conn->state = STATE_CONNECTION_CLOSED;
				break;
			}
			w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
			break;
		}
		if (conn->res_type != RESOURCE_TYPE_STATIC) {
			conn->state = STATE_CONNECTION_CLOSED;
			break;
		}
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			enum connection_state next_state = connection_send_static(conn);

			if (next_state == STATE_DATA_SENT)
				conn->state = STATE_CONNECTION_CLOSED;
			else
				conn->state = next_state;
		}
		break;
	case STATE_SENDING_404:
		if (connection_send_data(conn) < 0) {
			conn->state = STATE_CONNECTION_CLOSED;
			break;
		}
		if (conn->send_pos >= conn->send_len)
			conn->state = STATE_CONNECTION_CLOSED;
		break;
	case STATE_SENDING_DATA:
		if (conn->res_type == RESOURCE_TYPE_STATIC) {
			enum connection_state next_state = connection_send_static(conn);

			if (next_state == STATE_DATA_SENT)
				conn->state = STATE_CONNECTION_CLOSED;
			else
				conn->state = next_state;
			break;
		}
		if (conn->res_type == RESOURCE_TYPE_DYNAMIC) {
			if (connection_send_data(conn) < 0) {
				conn->state = STATE_CONNECTION_CLOSED;
				break;
			}
			if (conn->send_pos < conn->send_len)
				break;
			if (conn->file_pos >= conn->file_size) {
				conn->state = STATE_CONNECTION_CLOSED;
				break;
			}
			if (connection_send_dynamic(conn) < 0) {
				conn->state = STATE_CONNECTION_CLOSED;
				break;
			}
			w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
		}
		break;
	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	if (event & EPOLLERR) {
		conn->state = STATE_CONNECTION_CLOSED;
		connection_remove(conn);
		return;
	}

	if ((event & EPOLLHUP) &&
		(conn->state == STATE_INITIAL || conn->state == STATE_RECEIVING_DATA) &&
		strstr(conn->recv_buffer, "\r\n\r\n") == NULL) {
		conn->state = STATE_CONNECTION_CLOSED;
		connection_remove(conn);
		return;
	}

	if (event & EPOLLIN)
		handle_input(conn);
	if (conn->state == STATE_CONNECTION_CLOSED) {
		connection_remove(conn);
		return;
	}
	if (event & EPOLLOUT)
		handle_output(conn);
	if (conn->state == STATE_CONNECTION_CLOSED)
		connection_remove(conn);
}

int main(void)
{
	int rc;
	int flags;

	memset(&ctx, 0, sizeof(ctx));
	rc = io_setup(128, &ctx);
	DIE(rc < 0, "io_setup");

	epollfd = w_epoll_create();
	DIE(epollfd < 0, "epoll_create");

	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");
	flags = fcntl(listenfd, F_GETFL);
	DIE(flags < 0, "fcntl");
	rc = fcntl(listenfd, F_SETFL, flags | O_NONBLOCK);
	DIE(rc < 0, "fcntl");

	rc = w_epoll_add_ptr_in(epollfd, listenfd, NULL);
	DIE(rc < 0, "w_epoll_add_ptr_in");

	/* Uncomment the following line for debugging. */
	// dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		rc = w_epoll_wait_infinite(epollfd, &rev);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			DIE(1, "epoll_wait");
		}

		if (rev.data.ptr == NULL)
			handle_new_connection();
		else
			handle_client(rev.events, (struct connection *)rev.data.ptr);
	}

	return 0;
}
