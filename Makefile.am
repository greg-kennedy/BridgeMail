ACLOCAL_AMFLAGS = -I m4

AM_CFLAGS = -Wall -Wextra

bin_PROGRAMS = bridgemail

bridgemail_SOURCES = main.c \
		smtp.c \
		pop3.c
bridgemail_CFLAGS = -fsanitize=address,undefined,leak,integer
