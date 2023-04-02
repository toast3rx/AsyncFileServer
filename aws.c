#include <stdio.h>
#include <sys/epoll.h>
#include <libaio.h>
#include <stdlib.h>
#include "w_epoll.h"
#include "sock_util.h"
#include "aws.h"
#include "util.h"
#include <sys/eventfd.h>
#include "http_parser.h"
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <sys/sendfile.h>

#define NOT_FOUND_CODE 404
#define NOT_FOUND_MSG "Not Found"

#define OK_CODE 200
#define OK_MSG "OK"

///////////////////// CONNECTION UTILS /////////////////////
enum state {
	CONN_INIT,
	CONN_RECEIVED,
	CONN_CLOSED,
	CONN_WREADY,
	CONN_READING,
	CONN_SENDING,
	CONN_SENT,
	CONN_READING_ASYNC,
	CONN_ASYNC_READ_DONE,
};

struct connection {

	char read_buf[BUFSIZ];
	int bytes_read;

	char write_buf[BUFSIZ];
	ssize_t wbuff_len;
	ssize_t bytes_wrote;

	int sockfd;
	int efd;

	int fd;

	io_context_t ctx;
	struct iocb *iocb;
	struct iocb **piocb;
	int ops_no;

	int is_static; // use zero-copy if true
	char path[BUFSIZ]; // path to file

	size_t file_size;
	enum state conn_state; // current state of the connection

	int status_code;

	char **file_buffers;
	int buffers_sent;
	int subbed_ops;

	int header_sent;

	int tasks_done;

	ssize_t last_bytes;
};

struct connection *connection_create(int sockfd)
{
	struct connection *res = (struct connection *)malloc(sizeof(struct connection));
	DIE(res == NULL, "connection malloc");

	memset(res->read_buf, 0, BUFSIZ);
	res->bytes_read = 0;

	memset(res->write_buf, 0, BUFSIZ);
	res->bytes_wrote = 0;

	res->is_static = -1;

	memset(res->path, 0, BUFSIZ);

	res->conn_state = CONN_INIT;

	res->sockfd = sockfd;

	res->fd = -1;

	res->status_code = -1;

	res->buffers_sent = 0;

	res->subbed_ops = 0;

	res->efd = -1;

	res->header_sent = 0;

	res->tasks_done = 0;

	res->iocb = NULL;
	res->piocb = NULL;

	res->last_bytes = 0;

	res->wbuff_len = 0;

	return res;
}

static void connection_remove(struct connection *conn)
{
	if (conn->sockfd)
		close(conn->sockfd);
	if (conn->efd > 0)
		close(conn->efd);

	if (conn->file_buffers) {
		for (int i = 0; i < conn->ops_no; i++)
			free(conn->file_buffers[i]);

		free(conn->file_buffers);
	}

	if (conn->fd > 0)
		close(conn->fd);

	if (conn->iocb)
		free(conn->iocb);
	if (conn->piocb)
		free(conn->piocb);
	free(conn);

}

/////////////////////// epoll file descriptor /////////////////////
static int epollfd;

////////////////////// socket fd /////////////////////
static int listenfd;


///////////////////// parser utils /////////////////////
static http_parser request_parser;
static char request_path[BUFSIZ];	/* storage for request_path */

/**
 * @brief Callback method for http parser to get request path
 *
 * @param p http_parser
 * @param buf buffer where the path is located
 * @param len size of the buffer
 * @return int 0 on success
 */
static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);
	memcpy(request_path, AWS_DOCUMENT_ROOT, strlen(AWS_DOCUMENT_ROOT));
	memcpy(request_path + strlen(AWS_DOCUMENT_ROOT), buf + 1, len - 1);
	return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};


static void read_file_aio(struct connection *conn);

/**
 * @brief Accept new connection from client using listenfd
 *
 */
void handle_connection(void)
{
	int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	sockfd = accept(listenfd, (SSA *)&addr, &addrlen);
	DIE(sockfd < 0, "accept");

	rc = fcntl(sockfd, F_SETFD, fcntl(sockfd, F_GETFD, 0) | O_NONBLOCK);
	DIE(rc < 0, "fcntl sockfd");

	conn = connection_create(sockfd);

	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

static enum state receive_message(struct connection *conn)
{
	ssize_t bytes_recv;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0)
		goto remove_connection;

	bytes_recv = recv(conn->sockfd, conn->read_buf + conn->bytes_read, BUFSIZ - conn->bytes_read, 0);
	if (bytes_recv < 0)		/* error in communication */
		goto remove_connection;

	if (bytes_recv == 0)		/* connection closed */
		goto remove_connection;

	conn->bytes_read += bytes_recv;

	if (strcmp(conn->read_buf + strlen(conn->read_buf) - 4, "\r\n\r\n") == 0) {

		conn->conn_state = CONN_RECEIVED;

		http_parser_init(&request_parser, HTTP_REQUEST);
		memset(request_path, 0, BUFSIZ);
		http_parser_execute(&request_parser, &settings_on_path, conn->read_buf, conn->bytes_read);

		memcpy(conn->path, request_path, strlen(request_path));

		if (strstr(conn->read_buf, AWS_REL_STATIC_FOLDER))
			conn->is_static = 1;
		else
			conn->is_static = 0;

		return CONN_RECEIVED;
	}

	conn->conn_state = CONN_READING;
	return CONN_READING;


remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return CONN_CLOSED;
}

static void create_header(struct connection *conn, int code)
{
	char header[BUFSIZ];

	size_t size = code == NOT_FOUND_CODE ? -1 : conn->file_size;

	char *msg = code == NOT_FOUND_CODE ? NOT_FOUND_MSG : OK_MSG;


	sprintf(header, "HTTP/1.1 %d %s\r\n"
		"Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
		"Server: Apache/2.2.9\r\n"
		"Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
		"Accept-Ranges: bytes\r\n"
		"Content-Length: %ld\r\n"
		"Vary: Accept-Encoding\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html\r\n"
		"\r\n"
		, code, msg, size);

	conn->wbuff_len = strlen(header);
	memcpy(conn->write_buf, header, strlen(header));
}

static void handle_client_request(struct connection *conn)
{
	int rc;
	enum state ret_state;

	ret_state = receive_message(conn);

	if (ret_state == CONN_CLOSED || ret_state == CONN_READING)
		return;


	conn->fd = open(conn->path, O_RDONLY);

	if (conn->fd < 0) {
		create_header(conn, NOT_FOUND_CODE);
		conn->status_code = NOT_FOUND_CODE;
	} else {
		conn->file_size = lseek(conn->fd, 0, SEEK_END);
		lseek(conn->fd, 0, SEEK_SET);

		conn->status_code = OK_CODE;
		create_header(conn, OK_CODE);
	}

	conn->conn_state = CONN_WREADY;

	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_add_tr_inout");

}

static enum state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		goto remove_connection;
	}

	bytes_sent = send(conn->sockfd, conn->write_buf + conn->bytes_wrote, conn->wbuff_len - conn->bytes_wrote, 0);

	if (bytes_sent < 0) {		/* error in communication */
		goto remove_connection;
	}

	conn->bytes_wrote += bytes_sent;

	if (bytes_sent == 0) {		/* connection closed */
		goto remove_connection;
	}

	if (conn->bytes_wrote < conn->wbuff_len) {
		return CONN_SENDING;
	}

	return CONN_SENT;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return CONN_CLOSED;
}

static enum state send_file_zc(struct connection *conn)
{

	ssize_t bytes;
	bytes = sendfile(conn->sockfd, conn->fd, NULL, conn->file_size);

	if (bytes == 0)
		return CONN_SENT;

	return CONN_SENDING;
}

static void read_file_aio(struct connection *conn)
{

	int rc;
	conn->efd = eventfd(0, EFD_NONBLOCK);
	DIE(conn->efd < 0, "eventfd failed");

	int n = conn->file_size / BUFSIZ;

	int remaining_bytes = conn->file_size % BUFSIZ;
	conn->last_bytes = BUFSIZ;

	if (remaining_bytes > 0) {
		conn->last_bytes = remaining_bytes;
		n++;
	}

	memset(&conn->ctx, 0, sizeof(io_context_t));
	rc = io_setup(n, &conn->ctx);
	DIE(rc < 0, "io_setup");

	conn->ops_no = n;

	conn->iocb = (struct iocb *)malloc(n * sizeof(struct iocb));
	DIE(conn->iocb == NULL, "iocb malloc");

	conn->piocb = (struct iocb **)malloc(n * sizeof(struct iocb *));
	DIE(conn->piocb == NULL, "piocb malloc");

	conn->file_buffers = (char **)malloc(n * sizeof(char *));
	DIE(conn->file_buffers == NULL, "file_buffers malloc");

	int offset = 0;

	for (int i = 0; i < n; i++) {
		conn->file_buffers[i] = (char *)calloc(BUFSIZ, sizeof(char));
		DIE(conn->file_buffers[i] == NULL, "file_buffer malloc");

		conn->piocb[i] = &conn->iocb[i];

		int bytes = BUFSIZ;

		if (i == n - 1 && remaining_bytes > 0) {
			bytes = remaining_bytes;
		}

		io_prep_pread(&conn->iocb[i], conn->fd, conn->file_buffers[i], bytes, offset);

		offset += bytes;
		io_set_eventfd(&conn->iocb[i], conn->efd);

	}
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);

	w_epoll_add_ptr_in(epollfd, conn->efd, conn);

	rc = io_submit(conn->ctx, n, conn->piocb);
	DIE(rc < 0, "io_submit");
	conn->subbed_ops = rc;

	conn->conn_state = CONN_READING_ASYNC;
}

int main(void)
{
	int rc;

	// create epoll
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	// create listen port
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	// add socket in epoll
	w_epoll_add_fd_in(epollfd, listenfd);

	while (1) {
		struct epoll_event rev;
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		enum state status;

		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN) {
				handle_connection();
			}
		} else {
			struct connection *conn = ((struct connection *)rev.data.ptr);

			if (rev.events & EPOLLIN) {

				if (conn->conn_state == CONN_READING_ASYNC) {
					u_int64_t efd_val;
					int rc = read(conn->efd, &efd_val, sizeof(efd_val));
					DIE(rc < 0, "read");

					struct io_event *events = malloc(conn->subbed_ops * sizeof(struct io_event));

					rc = io_getevents(conn->ctx, efd_val, efd_val, events, NULL);
					DIE(rc != efd_val, "io_getevents");

					conn->tasks_done += rc;

					free(events);

					if (conn->subbed_ops < conn->ops_no) {
						rc = io_submit(conn->ctx, conn->ops_no - conn->subbed_ops, conn->piocb + conn->subbed_ops);
						DIE(rc < 0, "io_submit");
						conn->subbed_ops += rc;
					}

					if (conn->tasks_done == conn->ops_no) {
						conn->conn_state = CONN_ASYNC_READ_DONE;
						w_epoll_remove_ptr(epollfd, conn->efd, conn);
						close(conn->efd);

						// prepare write buff
						memset(conn->write_buf, 0, BUFSIZ);
						conn->bytes_wrote = 0;
						conn->wbuff_len = BUFSIZ;

						if (conn->ops_no == 1)
							conn->wbuff_len = conn->last_bytes;

						memcpy(conn->write_buf, conn->file_buffers[0], conn->wbuff_len);

						w_epoll_add_ptr_out(epollfd, conn->sockfd, conn);
					}

				} else {
					handle_client_request(rev.data.ptr);
				}
			} else if (rev.events & EPOLLOUT) {

				if (conn->header_sent) {
					// trb trimis fisierul efectiv
					if (conn->status_code == NOT_FOUND_CODE) {
						// close connection;
						w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
						connection_remove(conn);
					} else {
						if (conn->is_static) {
							status = send_file_zc(conn);

							if (status == CONN_SENT) {
								// close connection
								w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
								connection_remove(conn);
							}
						} else { // dynamic file
							if (conn->conn_state == CONN_WREADY) {
								read_file_aio(conn);
							} else if (conn->conn_state == CONN_ASYNC_READ_DONE) {
								// send buffers
								status = send_message(conn);

								if (status == CONN_SENT) {
									conn->buffers_sent++;

									if (conn->buffers_sent == conn->ops_no) {
										w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
										io_destroy(conn->ctx);
										connection_remove(conn);
									} else { // se realizeaza o noua scriere
										conn->bytes_wrote = 0;
										memset(conn->write_buf, 0, BUFSIZ);
										if (conn->buffers_sent == conn->ops_no - 1) { // ultimul buffer de scris
											conn->wbuff_len = conn->last_bytes;
											memcpy(conn->write_buf, conn->file_buffers[conn->buffers_sent], conn->last_bytes);
										} else {
											conn->wbuff_len = BUFSIZ;
											memcpy(conn->write_buf, conn->file_buffers[conn->buffers_sent], BUFSIZ);
										}
									}
								}
							}
						}
					}
				} else { // first the header is sent
					create_header(conn, conn->status_code);
					status = send_message(conn);
					if (status == CONN_SENT) {
						conn->header_sent = 1;
						conn->bytes_wrote = 0;
						memset(conn->write_buf, 0, BUFSIZ);
					}

				}
			}
		}
	}

	return 0;
}