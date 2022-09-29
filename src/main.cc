#include "server.h"
#include "relay.h"
#include "config.h"

#include <sys/stat.h>

static smtp::config *conf;

static void init_server(const char *pathname)
{
    conf = smtp::config::get_config(pathname);
    if (conf->mail_dir.back() == '/') {
        conf->mail_dir.pop_back();
    }
    conf->queue_dir = conf->mail_dir + "/queue";
    conf->sent_dir = conf->mail_dir + "/sent";
    conf->fail_dir = conf->mail_dir + "/fail";
    conf->tmp_dir = conf->mail_dir + "/tmp";
    mkdir(conf->mail_dir.c_str(), 0744);
    mkdir(conf->queue_dir.c_str(), 0744);
    mkdir(conf->sent_dir.c_str(), 0744);
    mkdir(conf->fail_dir.c_str(), 0744);
    mkdir(conf->tmp_dir.c_str(), 0744);
}

int main()
{
    init_server("../smtp.conf");
    angel::evloop loop;
    smtp::server server(&loop, angel::inet_addr(conf->port));
    smtp::relay relay;
    loop.run();
}
