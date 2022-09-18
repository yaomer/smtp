#ifndef __SMTP_SERVER_H
#define __SMTP_SERVER_H

#include <angel/server.h>

class smtp_server {
public:
    smtp_server(angel::evloop *, angel::inet_addr);
    ~smtp_server() = default;
    smtp_server(const smtp_server&) = delete;
    smtp_server& operator=(const smtp_server&) = delete;
private:
    void receive_mail(const angel::connection_ptr&, angel::buffer&);
    angel::server smtp;
};

#endif // __SMTP_SERVER_H
