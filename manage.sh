#!/bin/sh

if [ "$#" -le 1 ]; then
    echo "Usage: $0 <db.sqlite3> <command> <options>"
    exit 1
fi

case $2 in

    createdb)
        # creates the initial database, postmaster user, etc
        #  a user account on the system
        echo "CREATE TABLE IF NOT EXISTS mailbox (id TEXT PRIMARY KEY, auth TEXT) WITHOUT ROWID, STRICT" | sqlite3 $1
        # a message in the db
        echo "CREATE TABLE IF NOT EXISTS message (id INTEGER PRIMARY KEY, data BLOB NOT NULL) STRICT" | sqlite3 $1
        # link a message to a recipient
        echo "CREATE TABLE IF NOT EXISTS mailbox_message (mailbox_id TEXT NOT NULL, message_id INTEGER NOT NULL, PRIMARY KEY(mailbox_id, message_id), FOREIGN KEY(mailbox_id) REFERENCES mailbox(id), FOREIGN KEY(message_id) REFERENCES message(id)) WITHOUT ROWID, STRICT" | sqlite3 $1
        # message garbage collection trigger
        echo "CREATE TRIGGER IF NOT EXISTS message_trigger AFTER DELETE ON mailbox_message BEGIN DELETE FROM message WHERE id=OLD.message_id AND NOT EXISTS (SELECT 1 FROM mailbox_message WHERE message_id=OLD.message_id); END;" | sqlite3 $1
        # postmaster
        echo "INSERT OR IGNORE INTO mailbox(id, auth) VALUES('postmaster', null)" | sqlite3 $1
        ;;

    # USER MGMT
    adduser)
        if [ "$#" -le 3 ]; then
            echo "Usage: $0 $1 adduser username password"
            exit 1
        fi

        echo "INSERT INTO mailbox(id, auth) VALUES('$3', '$4')" | sqlite3 $1
        ;;

    changepassword)
        if [ "$#" -le 3 ]; then
            echo "Usage: $0 $1 changepassword username password"
            exit 1
        fi

        echo "UPDATE mailbox SET auth='$4' WHERE id='$3'" | sqlite3 $1
        ;;

    deleteuser)
        if [ "$#" -le 2 ]; then
            echo "Usage: $0 $1 deleteuser username"
            exit 1
        fi

        echo "PRAGMA foreign_keys=ON; DELETE FROM mailbox WHERE id='$3'" | sqlite3 $1
        ;;

    # LIST ALL USERS
    listusers)
        echo "SELECT id FROM mailbox" | sqlite3 $1
        ;;

    *)
        echo "<command> must be one of 'createdb', 'adduser', 'changepassword', 'deleteuser', 'listusers'";
        exit 1
        ;;
esac
