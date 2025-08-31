#include "pop3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctype.h>

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

static const char * eOK = "+OK\r\n";
static const char * eERR = "-ERR\r\n";

// calculations
// 4 bytes command + 2x(space + 40char args) + CR
#define LINE_MAX (4 + (1 + 40) * 2 + 1)

// Structure containing all state for a pop3 connection
struct pop3 {
	enum {
		INIT,
		AUTH,
		TRANSACTION
	} state;

	char line[LINE_MAX];
	unsigned char line_len;
	unsigned char line_overflow;

	char username[41];

	struct msg {
		unsigned int id;
		unsigned int size;
		unsigned char deleted;
	} * store;
	size_t store_len;
};

// prep the sqlite3 statements for use later
static sqlite3 * db;
//static sqlite3_stmt * stmt_begin;
static sqlite3_stmt * stmt_check_login;
static sqlite3_stmt * stmt_store;
static sqlite3_stmt * stmt_retr;
static sqlite3_stmt * stmt_dele;
//static sqlite3_stmt * stmt_commit;
//static sqlite3_stmt * stmt_rollback;

int pop3_setup(sqlite3 * parent_db)
{
	db = parent_db;

	//if (sqlite3_prepare_v2(db, "BEGIN", -1, &stmt_begin, NULL) != SQLITE_OK) return -1;

	if (sqlite3_prepare_v2(db, "SELECT EXISTS(SELECT 1 FROM mailbox WHERE id=? AND auth=?)", -1, &stmt_check_login, NULL) != SQLITE_OK) return -1;

	if (sqlite3_prepare_v2(db, "SELECT b.id, LENGTH(b.data) FROM mailbox_message a INNER JOIN message b ON a.message_id = b.id WHERE a.mailbox_id = ?", -1, &stmt_store, NULL) != SQLITE_OK) return -1;

	if (sqlite3_prepare_v2(db, "SELECT b.data FROM mailbox_message a INNER JOIN message b ON a.message_id = b.id WHERE a.mailbox_id = ? AND a.message_id = ?", -1, &stmt_retr, NULL) != SQLITE_OK) return -1;
	if (sqlite3_prepare_v2(db, "DELETE FROM mailbox_message WHERE mailbox_id = ? AND message_id = ?", -1, &stmt_dele, NULL) != SQLITE_OK) return -1;

	//if (sqlite3_prepare_v2(db, "INSERT INTO mailbox_message(mailbox_id, message_id) VALUES(?, ?)", -1, &stmt_insert_recipient, NULL) != SQLITE_OK) return -1;
	//if (sqlite3_prepare_v2(db, "COMMIT", -1, &stmt_commit, NULL) != SQLITE_OK) return -1;
	//if (sqlite3_prepare_v2(db, "ROLLBACK", -1, &stmt_rollback, NULL) != SQLITE_OK) return -1;
	return 0;
}

void pop3_teardown()
{
	// sqlite3_finalize(stmt_rollback);
	// sqlite3_finalize(stmt_commit);
	//sqlite3_finalize(stmt_stat);
	sqlite3_finalize(stmt_check_login);
	// sqlite3_finalize(stmt_begin);
}

struct pop3 * pop3_init(int fd)
{
	// Send initial "+OK <domain>" to announce connection start
	char response[23 + HOST_NAME_MAX + 3 + 1] = "+OK POP3 server ready <";

	if (gethostname(& response[23], HOST_NAME_MAX) == -1)
		perror("gethostname");

	strcat(response, ">\r\n");

	if (send(fd, response, strlen(response), 0) == -1) {
		perror("send");
		return NULL;
	}

	// Allocate state-struct for this connection and set it up
	struct pop3 * s = calloc(1, sizeof(struct pop3));

	if (s == NULL) {
		perror("malloc(struct pop3)");
		return NULL;
	}

	s->state = INIT;
	return s;
}

/*
         USER name               valid in the AUTHORIZATION state
         PASS string
         QUIT

         STAT                    valid in the TRANSACTION state
         LIST [msg]
         RETR msg
         DELE msg
         NOOP
         RSET
         QUIT

	TOP
	UIDL
*/

int pop3_process(struct pop3 * s, const char * buffer, int len, int fd)
{
#define RESPONSE(x) { puts( x ); if (send(fd, x, strlen(x), 0) == -1) { perror("send(" #x ")"); return -1; } }
#define POP3_RESPONSE(x) { puts( e ## x ); if (send(fd, e ## x, strlen(e ## x), 0) == -1) { perror("send(" #x ")"); return -1; } }

	// process incoming chars
	for (int i = 0; i < len; i ++) {
		if (s->line_len > 0 && s->line[s->line_len - 1] == '\r' && buffer[i] == '\n') {
			if (s->line_overflow) {
				// line overflowed and won't be parsed
				// can emit error now though
				POP3_RESPONSE(ERR);
			} else {
				// attempt to handle this line
				// replace CRLF with nul
				s->line[s->line_len - 1] = '\0';

				// debug
				fprintf(stderr, "Got command: [%s]\n", s->line);

				// tokenize the first bit
				char * cmd = strtok(s->line, " ");

				// switch action based on cmd
				if (cmd == NULL) {
					// empty line
					POP3_RESPONSE(ERR)
				} else if (strcasecmp(cmd, "QUIT") == 0) {
					// QUIT should not have params, and also it works at any point
					if (strtok(NULL, "") != NULL)
						POP3_RESPONSE(ERR)
					else {
						for (int j = 0; j < s->store_len; j ++) {
							if (s->store[j].deleted) {
								sqlite3_bind_text(stmt_dele, 1, s->username, -1, NULL);
								sqlite3_bind_int(stmt_dele, 2, s->store[j].id);
								if (sqlite3_step(stmt_dele) != SQLITE_DONE)
        								printf("Some error encountered\n");
								sqlite3_reset(stmt_dele);
							}
						}
						// 250 OK
						POP3_RESPONSE(OK)
						return -1;
					}
				} else if (strcasecmp(cmd, "USER") == 0) {
					// USER should have one param, the NAME
					if (s->state != INIT) {
						// must be issued before anything else
						POP3_RESPONSE(ERR);
					} else {
						char * arg = strtok(NULL, "");

						if (arg == NULL)
							POP3_RESPONSE(ERR)
						else if (strlen(arg) > 40)
							POP3_RESPONSE(ERR)
						else {
							// accepted USER, awaiting PASSWORD
							strcpy(s->username, arg);
							s->state = AUTH;
							POP3_RESPONSE(OK)
						}
					}
				} else if (strcasecmp(cmd, "PASS") == 0) {
					// PASS should have one param, the PASSWORD
					if (s->state != AUTH) {
						// must be issued only after sending USER
						POP3_RESPONSE(ERR)
					} else {
						char * arg = strtok(NULL, "");

						if (arg == NULL)
							POP3_RESPONSE(ERR)
						else if (strlen(arg) > 40)
							POP3_RESPONSE(ERR)
						else {
							// Check password against DB
							sqlite3_bind_text(stmt_check_login, 1, s->username, -1, NULL);
							sqlite3_bind_text(stmt_check_login, 2, arg, -1, NULL);
                      					if (sqlite3_step(stmt_check_login) != SQLITE_ROW) {
								POP3_RESPONSE(ERR)
							} else if (! sqlite3_column_int(stmt_check_login, 0)) {
								POP3_RESPONSE(ERR)
							} else {
								POP3_RESPONSE(OK)
								s->state = TRANSACTION;

								// and retrieve the message store
								sqlite3_bind_text(stmt_store, 1, s->username, -1, NULL);
                      						int retval = sqlite3_step(stmt_store);
								while (retval == SQLITE_ROW) {
									s->store = realloc(s->store, (s->store_len + 1) * sizeof(struct msg));
									if (s->store == NULL) {
										perror("realloc");
										exit(1);
									}
									s->store[s->store_len].id = sqlite3_column_int(stmt_store, 0);
									s->store[s->store_len].size = sqlite3_column_int(stmt_store, 1);
									s->store[s->store_len].deleted = 0;
									s->store_len ++;

                      							 retval = sqlite3_step(stmt_store);
								}
								if (retval != SQLITE_DONE) {
        								printf("Some error encountered\n");
								}
								sqlite3_reset(stmt_store);
							}

							sqlite3_reset(stmt_check_login);
						}
					}
				} else if (strcasecmp(cmd, "NOOP") == 0) {
					// NOOP should not have params, and only in state TRANSACTION
					if (s->state != TRANSACTION)
						POP3_RESPONSE(ERR)
					else {
						if (strtok(NULL, "") != NULL)
							POP3_RESPONSE(ERR)
						else
							POP3_RESPONSE(OK)
					}
				} else if (strcasecmp(cmd, "STAT") == 0) {
					// no args, transaction only
					if (s->state != TRANSACTION)
						POP3_RESPONSE(ERR)
					else {
						if (strtok(NULL, "") != NULL)
							POP3_RESPONSE(ERR)
						else {
							int store_size = 0;
							for (int j = 0; j < s->store_len; j ++)
								store_size += s->store[j].size;

							char response[1024];
							sprintf(response, "+OK %d %d\r\n", s->store_len, store_size);
							RESPONSE(response);
						}
					}
                               } else if (strcasecmp(cmd, "LIST") == 0) {
                                       // arg optional, transaction only
                                       if (s->state != TRANSACTION)
                                               POP3_RESPONSE(ERR)
                                       else {
                                               char * arg = strtok(NULL, " ");

                                               if (arg == NULL) {
							POP3_RESPONSE(OK)
							for (int j = 0; j < s->store_len; j ++) {
							        char response[1024];
							        sprintf(response, "%d %d\r\n", j + 1, s->store[i].size);
							    	RESPONSE(response);
							}
							RESPONSE(".\r\n");
						}
 
						else {
							int j = atoi(arg);
							if (j < 1 || j > s->store_len)
								POP3_RESPONSE(ERR)
							else {
							        char response[1024];
							        sprintf(response, "+OK %d %d\r\n", j, s->store[j - 1].size);
							    	RESPONSE(response);
							}
						}
					}
				} else if (strcasecmp(cmd, "RETR") == 0) {
					// message-number, transaction reqd
					if (s->state != TRANSACTION)
						POP3_RESPONSE(ERR)
					else {
						char * arg = strtok(NULL, " ");

						if (arg == NULL)
							POP3_RESPONSE(ERR)
						else {
							int j = atoi(arg) - 1;
							if (j < 0 || j >= s->store_len) {
								POP3_RESPONSE(ERR)
							} else {
								POP3_RESPONSE(OK)
								sqlite3_bind_text(stmt_retr, 1, s->username, -1, NULL);
								sqlite3_bind_int(stmt_retr, 2, s->store[j].id);
                      						int retval = sqlite3_step(stmt_retr);
								while (retval == SQLITE_ROW) {
								     	RESPONSE(sqlite3_column_text(stmt_retr, 0));
                      							retval = sqlite3_step(stmt_retr);
								}
								if (retval != SQLITE_DONE) {
        								printf("Some error encountered\n");
								}
								RESPONSE(".\r\n");
        							sqlite3_reset(stmt_retr);
							}
						}
					}
				} else if (strcasecmp(cmd, "DELE") == 0) {
					// message-number, transaction reqd
					if (s->state != TRANSACTION)
						POP3_RESPONSE(ERR)
					else {
						char * arg = strtok(NULL, " ");

						if (arg == NULL)
							POP3_RESPONSE(ERR)
						else {
							int j = atoi(arg) - 1;
							if (j < 0 || j >= s->store_len) {
								POP3_RESPONSE(ERR)
							} else if (s->store[j].deleted) {
								POP3_RESPONSE(ERR)
							} else {
								s->store[j].deleted = 1;
								POP3_RESPONSE(OK)
							}
						}
					}
				} else if (strcasecmp(cmd, "RSET") == 0) {
					// NO arguments, transaction reqd
					if (s->state != TRANSACTION)
						POP3_RESPONSE(ERR)
					else {
						if (strtok(NULL, "") != NULL)
							POP3_RESPONSE(ERR)
						else {
							for (int j = 0; j < s->store_len; j ++)
								s->store[j].deleted = 0;
						
							POP3_RESPONSE(OK)
						}
					}
				} else if (strcasecmp(cmd, "TOP") == 0) {
					// message-number, transaction reqd
					if (s->state != TRANSACTION)
						POP3_RESPONSE(ERR)
					else {
						char * arg = strtok(NULL, " ");

						if (arg == NULL)
							POP3_RESPONSE(ERR)
						else
							POP3_RESPONSE(OK)
					}
				} else if (strcasecmp(s->line, "UIDL") == 0) {
					if (s->state != TRANSACTION)
						POP3_RESPONSE(ERR)
					else {
						char * arg = strtok(NULL, " ");

						if (arg == NULL)
							POP3_RESPONSE(OK)
						else
							POP3_RESPONSE(OK)
					}
				} else {
					// command error of some sort - unrecognized
					POP3_RESPONSE(ERR)
				}
			}

			// reset line to empty
			s->line_overflow = s->line_len = 0;
		} else {

			// advance line ptr
			if (! s->line_overflow) {
				// check for line-too-long and set overflow if so
				if (s->line_len >= LINE_MAX) {
					printf("LINE OVERFLOW\n");
					s->line_overflow = 1;
				} else {
					s->line_len ++;
				}
			}

			// copy incoming char into linebuf
			s->line[s->line_len - 1] = buffer[i];
		}
	}

	return 0;
}

void pop3_free(struct pop3 * s)
{
	free(s->store);
	free(s);
}
