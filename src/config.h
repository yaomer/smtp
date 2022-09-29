#ifndef __SMTP_CONFIG_H
#define __SMTP_CONFIG_H

#include <string>

namespace smtp {

struct config {
    static config *get_config(const char *pathname)
    {
        static config ins(pathname);
        return &ins;
    }
    int port;
    std::string mail_dir;
    std::string queue_dir;
    std::string sent_dir;
    std::string fail_dir;
    std::string tmp_dir;
private:
    config(const char *pathname);
};

}

#endif // __SMTP_CONFIG_H
