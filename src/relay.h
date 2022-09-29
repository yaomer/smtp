#ifndef __SMTP_RELAY_H
#define __SMTP_RELAY_H

#include <angel/evloop_thread.h>
#include <angel/resolver.h>
#include <angel/smtplib.h>

#include <unordered_map>

namespace smtp {

struct relay_mail;

class relay {
public:
    relay();
    relay(const relay&) = delete;
    relay& operator=(const relay&) = delete;
private:
    void check_pendding_mails();

    angel::smtplib::sender sender;
    angel::dns::resolver *resolver;
    angel::evloop_thread relay_thread;

    std::unordered_map<std::string, relay_mail*> relay_map;

    friend struct relay_mail;
    friend struct relay_task;
};

}

#endif // __SMTP_RELAY_H
