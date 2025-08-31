#include "smtp.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctype.h>

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

static char e220[4 + HOST_NAME_MAX + 2 + 1] = "220 ";
static const char * e221 = "221 Service closing transmission channel\r\n";
static const char * e250 = "250 OK\r\n";
static const char * e252 = "252 Cannot VRFY user, but will accept message and attempt delivery\r\n";
static const char * e354 = "354 Start mail input; end with <CRLF>.<CRLF>\r\n";
static const char * e500 = "500 Syntax error, command unrecognized\r\n";
static const char * e501 = "501 Syntax error in parameters or arguments\r\n";
static const char * e503 = "503 Bad sequence of commands\r\n";
static const char * e550 = "550 Mailbox not found\r\n";

struct smtp {
	enum {
		INIT,
		HELO,
		MAIL,
		RCPT,
		DATA
	} state;

	time_t timestamp;

	char line[1001];
	unsigned short line_len;

	char ** rcpt;
	unsigned long rcpt_len;

	char * msg;
	unsigned long msg_len;
};

// prep the sqlite3 statements for use later
static sqlite3 * db;
// tx handlers
static sqlite3_stmt * stmt_begin = NULL;
static sqlite3_stmt * stmt_commit = NULL;
static sqlite3_stmt * stmt_rollback = NULL;
// specific db manip statements
static sqlite3_stmt * stmt_check_mailbox;
static sqlite3_stmt * stmt_insert_body;
static sqlite3_stmt * stmt_insert_recipient;

int smtp_setup(sqlite3 * parent_db)
{
	db = parent_db;

	if (sqlite3_prepare_v2(db, "BEGIN", -1, &stmt_begin, NULL) != SQLITE_OK) return -1;
	if (sqlite3_prepare_v2(db, "COMMIT", -1, &stmt_commit, NULL) != SQLITE_OK) return -1;
	if (sqlite3_prepare_v2(db, "ROLLBACK", -1, &stmt_rollback, NULL) != SQLITE_OK) return -1;

	if (sqlite3_prepare_v2(db, "SELECT EXISTS (SELECT 1 FROM mailbox WHERE id = ?)", -1, &stmt_check_mailbox, NULL) != SQLITE_OK) return -1;
	if (sqlite3_prepare_v2(db, "INSERT INTO message(data) VALUES(?)", -1, &stmt_insert_body, NULL) != SQLITE_OK) return -1;
	if (sqlite3_prepare_v2(db, "INSERT INTO mailbox_message(mailbox_id, message_id) VALUES(?, ?)", -1, &stmt_insert_recipient, NULL) != SQLITE_OK) return -1;

	// create initial "220 <domain>" sent at connection start
	if (gethostname(& e220[4], HOST_NAME_MAX + 1) == -1)
		perror("gethostname");
	strcat(e220, "\r\n");

	return 0;
}

void smtp_teardown()
{
	sqlite3_finalize(stmt_begin);
	sqlite3_finalize(stmt_commit);
	sqlite3_finalize(stmt_rollback);

	sqlite3_finalize(stmt_check_mailbox);
	sqlite3_finalize(stmt_insert_body);
	sqlite3_finalize(stmt_insert_recipient);
}

struct smtp * smtp_init(int fd)
{
	if (send(fd, e220, strlen(e220), 0) == -1) {
		perror("send");
		return NULL;
	}

	// Allocate state-struct for this connection and set it up
	struct smtp * s = malloc(sizeof(struct smtp));

	if (s == NULL) {
		perror("malloc(struct smtp)");
		return NULL;
	}

	s->state = INIT;
	s->timestamp = time(NULL);
	s->line_len = 0;
	s->rcpt = NULL;
	s->rcpt_len = 0;
	s->msg = NULL;
	s->msg_len = 0;
	return s;
}

// helper function: extract an address from a FROM: <*> or TO: <*> line
//  TODO: this could be RFC-whatever compliant and also parse the domain - for triggers!
static const char * get_address(const char * type, const char * line)
{
	// the first bytes must match
	const char * t = type, * p = line;
	while (*t != '\0') {
		if (toupper(*t) != toupper(*p)) return NULL;
		t ++; p ++;
	}
	// colon
	if (*p != ':') return NULL; p++;
	// whitespace
	//while (*p == ' ') p ++;
	// open bracket
	if (*p != '<') return NULL; p++;
	// validate closing bracket
	if (line[strlen(line) - 1] != '>') return NULL;

	// search for @ splitter
	t = p + 1;
	while (*t != '@' && *t != '>' && *t != '\0') t ++;

	// duplicate and return
	char * address = malloc(t - p + 1);
	memcpy(address, p, t - p);
	address[t - p] = '\0';

	return address;
}

int smtp_process(struct smtp * s, const char * buffer, int len, int fd)
{
#define SMTP_RESPONSE(x) { puts( e ## x ); if (send(fd, e ## x, strlen(e ## x), 0) == -1) { perror("send(" #x ")"); return -1; } }

	// process incoming chars
	for (int i = 0; i < len; i ++) {
		// copy next char into linebuf
		s->line[s->line_len] = buffer[i];
		s->line_len ++;

		// Check for CRLF
		if (s->line_len > 1 && s->line[s->line_len - 2] == '\r' && s->line[s->line_len - 1] == '\n') {
			// a received full command resets the idle timer
			s->timestamp = time(NULL);

			// regular commands outside DATA (email upload)
			if (s->state != DATA) {
				// rtrim CRLF and any trailing spaces
				s->line_len -= 2;

				while (s->line_len > 0 && s->line[s->line_len - 1] == ' ')
					s->line_len --;

				s->line[s->line_len] = '\0';
				fprintf(stderr, "Got command: [%s]\n", s->line);

				// tokenize the first bit
				char * cmd = strtok(s->line, " ");

				// switch action based on cmd
				if (cmd == NULL) {
					// empty line
					SMTP_RESPONSE(500)
				} else if (strcasecmp(cmd, "RSET") == 0) {
					// RSET should not have params
					if (strtok(NULL, "") != NULL)
						SMTP_RESPONSE(501)
					else {
						// 250 OK
						if (s->state != INIT) s->state = HELO;

						SMTP_RESPONSE(250)
					}
				} else if (strcasecmp(s->line, "NOOP") == 0) {
					// NOOP
					SMTP_RESPONSE(250)
				} else if (strcasecmp(s->line, "VRFY") == 0) {
					// VRFY should have a parameter (address to check)
					char * arg = strtok(NULL, "");
					if (arg == NULL)
						SMTP_RESPONSE(501)
					else
						// TODO: it might be nice to support VRFY
						SMTP_RESPONSE(252)
				} else if (strcasecmp(s->line, "HELO") == 0 || strcasecmp(s->line, "EHLO") == 0) {
					// HELO / EHLO should have a parameter (client domain)
					char * arg = strtok(NULL, "");
					if (arg == NULL)
						SMTP_RESPONSE(501)
					else if (s->state != INIT)
						// HELO only accepted at start-of-connection
						SMTP_RESPONSE(503)
					else {
						// Send initial "220 <domain>" to announce connection start
						SMTP_RESPONSE(250)
						// change to post-HELO state
						s->state = HELO;
					}
				} else if (strcasecmp(s->line, "QUIT") == 0) {
					// QUIT should not have a parameter
					if (strtok(NULL, "") != NULL)
						SMTP_RESPONSE(501)
					else if (s->state == INIT)
						// QUIT only accepted after HELO or later
						SMTP_RESPONSE(503)
					else {
						// Respond 221 and close connection in all cases
						SMTP_RESPONSE(221)
						return -1;
					}
				} else if (strcasecmp(s->line, "MAIL") == 0) {
					// MAIL command.  Line must be at least MAIL FROM:<*>
					char * arg = strtok(NULL, "");
					if (arg == NULL)
						SMTP_RESPONSE(500)
					else if (s->state != HELO)
						// MAIL only accepted after HELO
						SMTP_RESPONSE(503)
					else {
						// try to get FROM address
						const char * address = get_address("FROM", arg);
						if (address == NULL)
							// failed to parse address
							SMTP_RESPONSE(501)
						else {
							// verify sender
							sqlite3_bind_text(stmt_check_mailbox, 1, address, -1, NULL);
							if (sqlite3_step(stmt_check_mailbox) == SQLITE_ROW) {
								if (sqlite3_column_int(stmt_check_mailbox, 0)) {
									SMTP_RESPONSE(250)
									s->state = MAIL;
								} else
									SMTP_RESPONSE(550)
							} else
								SMTP_RESPONSE(550)

							sqlite3_reset(stmt_check_mailbox);

							free(address);
						}
					}
				} else if (strcasecmp(s->line, "RCPT") == 0) {
					// RCPT command.  Line must be at least RCPT TO:<*>
					char * arg = strtok(NULL, "");
					if (arg == NULL)
						SMTP_RESPONSE(501)
					else if (s->state != MAIL && s->state != RCPT)
						// RCPT only accepted after MAIL
						SMTP_RESPONSE(503)
					else {
						const char * address = get_address("TO", arg);
						if (address == NULL)
							// failed to parse address
							SMTP_RESPONSE(501)
						else {
							// verify recipient
							sqlite3_bind_text(stmt_check_mailbox, 1, address, -1, NULL);
							if (sqlite3_step(stmt_check_mailbox) == SQLITE_ROW) {
								if (sqlite3_column_int(stmt_check_mailbox, 0)) {
									// looks good, add to the recipient list
									s->rcpt = realloc(s->rcpt, (s->rcpt_len + 1) * sizeof(const char *));
									if (s->rcpt == NULL) {
										perror("realloc");
										exit(1);
									}
									s->rcpt[s->rcpt_len] = address;
									s->rcpt_len ++;

									SMTP_RESPONSE(250)
									s->state = RCPT;
								} else {
									SMTP_RESPONSE(550)
									free(address);
								}
							} else {
								SMTP_RESPONSE(550)
								free(address);
							}

							sqlite3_reset(stmt_check_mailbox);
						}
					}
				} else if (strcasecmp(s->line, "DATA") == 0) {
					// DATA command - only follows RCPT!
					if (strtok(NULL, "") != NULL)
						// DATA should not have a parameter
						SMTP_RESPONSE(501)
					else if (s->state != RCPT)
						// RCPT only accepted after MAIL
						SMTP_RESPONSE(503)
					else {
						// Get the FROM address
						SMTP_RESPONSE(354)
						s->state = DATA;
					}
				} else
					// bad command
					SMTP_RESPONSE(500)
			} else {
				// terminate line at line_len
				s->line[s->line_len] = '\0';
				fprintf(stderr, "Got data: [%s]\n", s->line);

				if (s->msg != NULL && strcmp(s->line, ".\r\n") == 0) {
					// put message into message store db
					sqlite3_step(stmt_begin);
					sqlite3_reset(stmt_begin);
					//
					sqlite3_bind_blob(stmt_insert_body, 1, s->msg, strlen(s->msg), NULL);

					if (sqlite3_step(stmt_insert_body) != SQLITE_DONE) {
						sqlite3_step(stmt_rollback);
						sqlite3_reset(stmt_rollback);
					} else {
						// recipients
						sqlite3_int64 rowid = sqlite3_last_insert_rowid(db);

						for (unsigned long i = 0; i < s->rcpt_len; i ++) {
							sqlite3_bind_text(stmt_insert_recipient, 1, s->rcpt[i], -1, NULL);
							sqlite3_bind_int64(stmt_insert_recipient, 2, rowid);

							if (sqlite3_step(stmt_insert_recipient) != SQLITE_DONE) {
								sqlite3_step(stmt_rollback);
								sqlite3_reset(stmt_rollback);
							} else {
								sqlite3_step(stmt_commit);
								sqlite3_reset(stmt_commit);
							}

							free(s->rcpt[i]);

							sqlite3_reset(stmt_insert_recipient);
						}
					}

					sqlite3_reset(stmt_insert_body);
					SMTP_RESPONSE(250)
					s->state = HELO;
					free(s->rcpt);
					s->rcpt = NULL;
					s->rcpt_len = 0;
					free(s->msg);
					s->msg = NULL;
					s->msg_len = 0;
				} else {
					// append
					if (s->msg == NULL) {
						s->msg = strdup(s->line);
						s->msg_len = s->line_len;
					} else {
						// TODO
						s->msg = realloc(s->msg, s->msg_len + s->line_len + 1);

						if (s->msg == NULL) {
							perror("realloc");
							exit(1);
						}

						strcat(s->msg, s->line);
						s->msg_len += s->line_len;
					}
				}
			}

			s->line_len = 0;
		}
	}

	return 0;
}

void smtp_free(struct smtp * s)
{
	for (unsigned long i = 0; i < s->rcpt_len; i ++)
		free(s->rcpt[i]);
	free(s->rcpt);
	free(s->msg);
	free(s);
}
