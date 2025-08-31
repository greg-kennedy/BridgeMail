/*
** BridgeMail - a local-only SMTP / POP3 mail service
*  Greg Kennedy 2021
*/

// handlers for SMTP and POP3 protocols
#include "smtp.h"
#include "pop3.h"

// for our storage db
#include <sqlite3.h>

// system includes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <ctype.h>

enum sock_type {
	SOCK_NONE = 0,
	SOCK_LISTEN_SMTP = 1,
	SOCK_LISTEN_POP3 = 2,
	SOCK_XFER_SMTP = 3,
	SOCK_XFER_POP3 = 4
};

static struct socket_detail {
	enum sock_type type;
	void * data;
} * socket_details = NULL;

static struct pollfd * socket_fds = NULL;
static int socket_count = 0;
static int socket_max = 0;

// Error callback for SQLite errors - simply print to stderr
static void errorLogCallback(const void * pArg, int iErrCode, const char * zMsg)
{
	fprintf(stderr, "SQLite Error (%d): %s\n", iErrCode, zMsg);
}

// Get printable address info
static const char * get_addr_detail(const struct sockaddr * sa)
{
	static char ip[INET6_ADDRSTRLEN] = "";
	const char * ret;

	if (sa->sa_family == AF_INET)
		ret = inet_ntop(AF_INET, & ((struct sockaddr_in *)sa)->sin_addr, ip, INET_ADDRSTRLEN);
	else if (sa->sa_family == AF_INET6)
		ret = inet_ntop(AF_INET6, & ((struct sockaddr_in6 *)sa)->sin6_addr, ip, INET6_ADDRSTRLEN);
	else
		return "(unknown)";

	if (ret == NULL) {
		perror("inet_ntop()");
		return "(error)";
	}

	return ret;
}

// add new socket info to the socket lists
static int addSocket(int fd, enum sock_type type)
{
	//  if it is full already, we must grow it
	if (socket_count == socket_max) {
		// determine new list size
		const int new_socket_max = socket_max * 1.5 + 1;
		// resize the socket-type list
		struct socket_detail * new_socket_details = realloc(socket_details, new_socket_max * sizeof(struct socket_detail));

		if (new_socket_details == NULL) {
			perror("realloc(socket_details)");
			return -1;
		}

		socket_details = new_socket_details;
		// resize the socket-fd list
		struct pollfd * new_socket_fds = realloc(socket_fds, new_socket_max * sizeof(struct pollfd));

		if (new_socket_fds == NULL) {
			perror("realloc(socket_fds)");
			// be nice and try to resize new_socket_details back down
			new_socket_details = realloc(socket_details, socket_max * sizeof(enum sock_type));

			if (new_socket_details == NULL)
				perror("realloc(socket_details)");
			else
				socket_details = new_socket_details;

			return -1;
		}

		socket_fds = new_socket_fds;
		// all good, move forward
		socket_max = new_socket_max;
	}

	// add our new socket at the end of the list
	socket_details[socket_count].type = type;
	socket_fds[socket_count].fd = fd;
	socket_fds[socket_count].events = POLLIN; // | POLLPRI;
	//socket_count ++;
	return (socket_count ++);
}

static void delSocket(int index)
{
	socket_fds[index] = socket_fds[socket_count];
	socket_details[index] = socket_details[socket_count];
	socket_fds[socket_count].fd = -1;
	socket_fds[socket_count].events = 0;
	socket_details[socket_count].type = SOCK_NONE;
	socket_details[socket_count].data = NULL;
	socket_count --;
}

static int acceptSocket(const int listener)
{
	// handle new connections
	struct sockaddr_storage remoteaddr; // client address
	socklen_t addrlen = sizeof remoteaddr;
	int fd = accept(listener, (struct sockaddr *)&remoteaddr, &addrlen);

	if (fd == -1) {
		// an error occurred trying to accept the new connection - maybe they disconnected in the meantime or something
		perror("accept");
		return -1;
	}

	// success!  print some helpful info
	printf(" . Received connection from %s on socket %d -> new socket %d\n", get_addr_detail((struct sockaddr *)&remoteaddr), listener, fd);
	return fd;
}

// Bind to listener addresses
//  This takes a service (port) and binds to ALL addresses
//  also ipv4 AND ipv6
// returns the number of sockets added
int get_listener_socket(const char * const port, const enum sock_type type)
{
	int sockets_added = 0;
	// Get us a socket and bind it
	const struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP,
		.ai_flags = AI_PASSIVE | AI_NUMERICSERV | AI_ADDRCONFIG
	};
	struct addrinfo * ai;
	int rv = getaddrinfo(NULL, port, &hints, &ai);

	if (rv != 0) {
		fprintf(stderr, "getaddrinfo(port=%s) (%d): %s\n", port, rv, gai_strerror(rv));
		return 0;
	}

	for (const struct addrinfo * p = ai; p != NULL; p = p->ai_next) {
		const int listener = socket(p->ai_family, p->ai_socktype, p->ai_protocol);

		if (listener == -1) {
			perror("socket( AF_UNSPEC, SOCK_STREAM, IPPROTO_TCP )");
			continue;
		}

		static const int yes = 1;

		if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
			// not fatal just annoying
			perror("setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, 1)");
		}

		if (bind(listener, p->ai_addr, p->ai_addrlen) == -1) {
			perror("bind()");
			close(listener);
			continue;
		}

		// Listen
		if (listen(listener, SOMAXCONN) == -1) {
			perror("listen()");
			close(listener);
			continue;
		}

		if (addSocket(listener, type) == -1) {
			fprintf(stderr, "Failed to addSocket(%d, %d).\n", listener, type);
			close(listener);
			continue;
		}

		// success!  print some helpful info
		printf(" . Bound to %s:%s on socket %d (type %d)\n", get_addr_detail(p->ai_addr), port, listener, type);
		sockets_added ++;
	}

	freeaddrinfo(ai); // All done with this
	return sockets_added;
}

// Flag to indicate whether we should keep working
//  Set to 0 to close the program
static int running;
// replacement signal handler that sets running to 0 for clean shutdown
static void sig_handler(int signum)
{
	fprintf(stderr, "Received signal %d (%s), exiting.\n", signum, strsignal(signum));
	running = 0;
}

// Main
int main(int argc, char * argv[])
{
	printf("BridgeMail - Greg Kennedy 2023\nStarting up...\n");
	// parse options
	const char * port_smtp = "25", * port_pop3 = "110";
	int c;

	while ((c = getopt(argc, argv, "s:p:")) != -1)
		switch (c) {
		case 's':
			port_smtp = optarg;
			break;

		case 'p':
			port_pop3 = optarg;
			break;

		case '?':
			if (optopt == 's' || optopt == 'p')
				fprintf(stderr, "Option -%c requires an argument.\n", optopt);
			else if (isprint(optopt))
				fprintf(stderr, "Unknown option `-%c'.\n", optopt);
			else
				fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);

			return EXIT_FAILURE;

		default:
			return EXIT_FAILURE;
		}

	if (optind != argc - 1) {
		printf("Error: incorrect number of arguments.\nUsage: BridgeMail /path/to/mail.db\n");
		return EXIT_FAILURE;
	}

	// connect to the initial DB
	// turn on error printing for the sqlite3 interface
	sqlite3_config(SQLITE_CONFIG_LOG, errorLogCallback, NULL);
	sqlite3 * db;
	int rv = sqlite3_open_v2(argv[optind], &db, SQLITE_OPEN_READWRITE, NULL);

	if (rv != SQLITE_OK) {
		fputs("Failed to open database.\n", stderr);
		sqlite3_close(db);
		return EXIT_FAILURE;
	}

	if (sqlite3_exec(db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL) != SQLITE_OK) {
		fputs("Failed to enable foreign keys.\n", stderr);
		sqlite3_close(db);
		return EXIT_FAILURE;
	}

	// modules do any global setup
	if (smtp_setup(db) == -1) {
		fputs("Failed to setup SMTP module.\n", stderr);
		sqlite3_close(db);
		return EXIT_FAILURE;
	}

	if (pop3_setup(db) == -1) {
		fputs("Failed to setup POP3 module.\n", stderr);
		smtp_teardown();
		sqlite3_close(db);
		return EXIT_FAILURE;
	}

	// Great, now we are ready to open the ports and accept messages
	if (! get_listener_socket(port_smtp, SOCK_LISTEN_SMTP)) {
		fputs("Failed to open SMTP socket.\n", stderr);
		pop3_teardown();
		smtp_teardown();
		sqlite3_close(db);
		return EXIT_FAILURE;
	}

	if (! get_listener_socket(port_pop3, SOCK_LISTEN_POP3)) {
		fputs("Failed to open POP3 socket.\n", stderr);
		pop3_teardown();
		smtp_teardown();
		sqlite3_close(db);
		return EXIT_FAILURE;
	}

	// Main loop
	// Let's install some signal handlers for a graceful exit
	running = 1;
	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGQUIT, sig_handler);
	signal(SIGHUP, sig_handler);

	while (running) {
		// TODO: timeout as min(all sockets), or -1 if none connected, etc
		// SMTP RFC specifies 5 minutes for server timeout
		// POP3 RFC specifies 10 minutes for server timeout
		int rv = poll(socket_fds, socket_max, -1);

		if (rv == -1) {
			perror("poll"); // error occurred in poll()
		} else if (rv == 0)
			perror("timeout");
		else {
			// search for anything needing attention
			int i = 0;

			while (rv > 0 && i < socket_count) {
				if (socket_fds[i].revents & POLLIN) {
					char buffer[1460];
					int nbytes;
					int fd;

					switch (socket_details[i].type) {
					case SOCK_LISTEN_SMTP:
						fd = acceptSocket(socket_fds[i].fd);

						if (fd == -1)
							fputs("Failed to accept incoming SMTP connection.\n", stderr);
						else {
							struct smtp * s = smtp_init(fd);

							if (s == NULL) {
								fputs("Failed to initialize SMTP connection.\n", stderr);
								close(fd);
							} else {
								// need to create another socket_fds
								int j = addSocket(fd, SOCK_XFER_SMTP);

								if (j == -1) {
									fputs("Failed to store SMTP connection.\n", stderr);
									close(fd);
								} else {
									puts("Created SMTP connection.\n");
									socket_details[j].data = s;
								}
							}
						}

						i ++;
						break;

					case SOCK_LISTEN_POP3:
						fd = acceptSocket(socket_fds[i].fd);

						if (fd == -1)
							fputs("Failed to accept incoming POP3 connection.\n", stderr);
						else {
							struct pop3 * p = pop3_init(fd);

							if (p == NULL) {
								fputs("Failed to initialize POP3 connection.\n", stderr);
								close(fd);
							} else {
								// need to create another socket_fds
								int j = addSocket(fd, SOCK_XFER_POP3);

								if (j == -1) {
									fputs("Failed to store POP3 connection.\n", stderr);
									close(fd);
								} else {
									puts("Created POP3 connection.\n");
									socket_details[j].data = p;
								}
							}
						}

						i ++;
						break;

					case SOCK_XFER_SMTP:
						nbytes = recv(socket_fds[i].fd, buffer, sizeof buffer, 0);

						if (nbytes <= 0) {
							// got error or connection closed by client
							if (nbytes == 0)
								printf("- SMTP socket %d (%d) hung up\n", i, socket_fds[i].fd);
							else
								perror("recv");

							smtp_free(socket_details[i].data);
							close(socket_fds[i].fd);
							delSocket(i);
						} else {
							if (smtp_process(socket_details[i].data, buffer, nbytes, socket_fds[i].fd) == -1) {
								printf("- SMTP socket %d (%d) disconnected\n", i, socket_fds[i].fd);
								smtp_free(socket_details[i].data);
								close(socket_fds[i].fd);
								delSocket(i);
							} else
								i ++;
						}

						break;

					case SOCK_XFER_POP3:
						nbytes = recv(socket_fds[i].fd, buffer, sizeof buffer, 0);

						if (nbytes <= 0) {
							// got error or connection closed by client
							if (nbytes == 0)
								printf("- POP3 socket %d (%d) hung up\n", i, socket_fds[i].fd);
							else
								perror("recv");

							pop3_free(socket_details[i].data);
							close(socket_fds[i].fd);
							delSocket(i);
						} else {
							if (pop3_process(socket_details[i].data, buffer, nbytes, socket_fds[i].fd) == -1) {
								printf("- POP3 socket %d (%d) disconnected\n", i, socket_fds[i].fd);
								pop3_free(socket_details[i].data);
								close(socket_fds[i].fd);
								delSocket(i);
							} else
								i ++;
						}

						break;

					default:
						fprintf(stderr, "socket %d has unknown socket type %d\n", i, socket_details[i].type);
						i ++;
						break;
					}

					rv --;
				} else
					i ++;
			}
		}
	}

	/* *************************************************** */
	// CLEANUP CODE
	// restore signal handlers
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGHUP, SIG_DFL);

	// Shut down
	for (int i = 0; i < socket_count; i ++)
		close(socket_fds[i].fd);

	free(socket_fds);
	free(socket_details);
	pop3_teardown();
	smtp_teardown();
	sqlite3_close(db);
	return 0;
}
