# BridgeMail
Small footprint, local-only mail server

## Overview
BridgeMail is a standalone email server application.  It runs as a single process on a computer, accepts mail via SMTP, and delivers it over POP3.  All mail messages and accounts are stored in one sqlite database, which is managed with a script to make changes from the command line.

However, BridgeMail does NOT forward messages to other mail servers or provide any other relay methods.  It cannot be used to email an arbitrary recipient, only those registered on the server itself.

One could probably achieve a similar setup with postfix, exim, dovecot etc. but it requires doing terrible things to your system MTA or changing arcane config files.  BridgeMail instead is easy to understand and manage.

## Why?
[Email](https://en.wikipedia.org/wiki/Email) is one of the oldest network protocols still in widespread usage - practically everything comes with an email client, even those that predate a web browser, and many of these systems are also of interest to retrocomputing fans.  Fortunately, email standards have moved on since the 1980s (e.g. STARTTLS), but these have left behind systems that no longer receive such updates.  BridgeMail fulfills a role of allowing antique machines to communicate with modern ones (or each other), for message and file exchange.

## Security?
Lol, and furthermore, lmao.

## Usage
You must first create a mail database for the server, and populate it with users.  Assuming the database is to be called "mail.db", these commands create the initial database and then add a user to it named "user" with password "password".
```sh
./manage.py mail.db createdb
./manage.py mail.db adduser user password
```

Then, start BridgeMail.  By default it listens on port 25 (SMTP) and 110 (POP3), which are privileged ports under Unix.  This requires root access - probably a bad move - so you have a few options:
* Use different ports
```
./BridgeMail -p 2525 -s 11110 mail.db
```
* Launch as root but run with different user / group
```
sudo ./BridgeMail -u mailuser -g mailgroup mail.db
```
* Or make BridgeMail a setuid binary and then run it as mailuser.  It will drop permissions except when opening ports.
```
chmod o+s,g+s BridgeMail
./BridgeMail mail.db
```

## Connecting
Clients can now connect to a running BridgeMail to send and receive mail to one another.  Use the username and password for all login credentials.  **Domain names are ignored** in all actions - an email to `user@example.com` and one to `user@hotmail.com` both are sent to `user`'s mailbox.  Use any domain name for SMTP login, if one is required.

Many web hosts block port 25 and require special permission to unblock it.  If you are using BridgeMail and find that a blocked port 25 is preventing you from putting BridgeMail onto the Internet, please reconsider your life choices.
