#ifndef SMTP_H_
#define SMTP_H_

// for our storage db
#include <sqlite3.h>

struct smtp;

int smtp_setup(sqlite3 * db);
void smtp_teardown();

struct smtp * smtp_init(int fd);
int smtp_process(struct smtp * s, const char * buffer, int len, int fd);
void smtp_free(struct smtp * s);

#endif
