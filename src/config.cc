#include "config.h"

#include <angel/util.h>

namespace smtp {

config::config(const char *pathname)
{
    auto paramlist = angel::util::parse_conf(pathname);
    for (auto& arg : paramlist) {
        if (strcasecmp(arg[0].c_str(), "listen-port") == 0) {
            port = atoi(arg[1].c_str());
        } else if (strcasecmp(arg[0].c_str(), "mail-dir") == 0) {
            mail_dir = arg[1];
        }
    }
}

}
