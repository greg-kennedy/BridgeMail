#ifndef POP3_H_
#define POP3_H_

// for our storage db
#include <sqlite3.h>

struct pop3;

int pop3_setup(sqlite3 * db);
void pop3_teardown();

struct pop3 * pop3_init(int fd);
int pop3_process(struct pop3 * s, const char * buffer, int len, int fd);
void pop3_free(struct pop3 * s);

#endif
