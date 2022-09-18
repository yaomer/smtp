#ifndef __SMTP_TYPES_H
#define __SMTP_TYPES_H

namespace smtp {

extern const char *mail_queue;
extern const char *mail_sent;

enum CommandType {
    EHLO, HELO, MAIL, RCPT, DATA, RSET, VRFY, EXPN, HELP, NOOP, QUIT
};

}

#endif // __SMTP_TYPES_H
