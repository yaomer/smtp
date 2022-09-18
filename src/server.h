#ifndef __SMTP_SERVER_H
#define __SMTP_SERVER_H

#include <angel/server.h>

namespace smtp {

class server {
public:
    server(angel::evloop *, angel::inet_addr);
    ~server() = default;
    server(const server&) = delete;
    server& operator=(const server&) = delete;
private:
    void receive_mail(const angel::connection_ptr&, angel::buffer&);
    angel::server smtp;
};

}

#endif // __SMTP_SERVER_H
