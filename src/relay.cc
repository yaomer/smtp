#include "relay.h"

#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <string>
#include <chrono>
#include <future>

#include <angel/util.h>
#include <angel/logger.h>

#include "config.h"

namespace smtp {

struct relay_mail;

struct relay_task {
    std::string host;
    std::vector<std::string> mail_to;
    std::queue<std::string> mx_name_list;
    std::string cur_mx_name;
    angel::smtplib::result_future f;
    relay_mail *relay_mail;

    void set_mx_name_list();
    bool is_ready();
    void start();
};

struct relay_mail {
    ~relay_mail();
    std::string raw_filename;
    std::string filename;
    angel::smtplib::email mail;
    std::unordered_map<std::string, std::unique_ptr<relay_task>> task_map;
    size_t check_timer_id;
    relay *relay;
    void check_relay_task();
    void read_mail();
    void build();
    void start();
    void relay_ok();
    void relay_fail();
};

static config *conf;

relay::relay()
{
    conf = config::get_config("");
    auto *loop = relay_thread.wait_loop();
    loop->run_every(1000, [this]{ this->check_pendding_mails(); });
    resolver = angel::dns::resolver::get_resolver();
    printf("relay start\n");
}

relay_mail::~relay_mail()
{
    relay_ok();
}

void relay::check_pendding_mails()
{
    DIR *dirp = opendir(conf->queue_dir.c_str());
    struct dirent *dir;

    while ((dir = readdir(dirp))) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0)
            continue;

        std::string filename(conf->queue_dir);
        filename.append("/").append(dir->d_name);

        if (relay_map.count(filename)) {
            // Relaying...
            continue;
        }

        log_info("ready to relay mail (%s)", dir->d_name);
        auto *relay_mail = new struct relay_mail();
        relay_mail->relay = this;
        relay_mail->raw_filename = dir->d_name;
        relay_mail->filename = std::move(filename);
        relay_mail->read_mail();
        relay_mail->build();
        relay_mail->start();

        relay_map.emplace(relay_mail->filename, relay_mail);
    }
    closedir(dirp);
}

void relay_mail::relay_ok()
{
    log_info("relay mail (%s) successfully", raw_filename.c_str());
    auto newname = conf->sent_dir + "/" + raw_filename;
    int rc = rename(filename.c_str(), newname.c_str());
    assert(rc != -1);
}

void relay_mail::relay_fail()
{
    auto newname = conf->fail_dir + "/" + raw_filename;
    int rc = rename(filename.c_str(), newname.c_str());
    assert(rc != -1);
}

void relay_mail::read_mail()
{
    static char buf[65536];
    int fd = open(filename.c_str(), O_RDONLY);
    assert(fd >= 0);

    uint16_t len;
    read(fd, &len, sizeof(len));
    read(fd, buf, len);
    mail.from.assign(buf, len);
    uint32_t receiver;
    read(fd, &receiver, sizeof(receiver));
    while (receiver-- > 0) {
        read(fd, &len, sizeof(len));
        read(fd, buf, len);
        mail.to.emplace_back(buf, len);
    }
    char newline;
    read(fd, &newline, 1);

    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        mail.data.append(buf, n);
    }
    close(fd);
}

// All receiver are grouped by host,
// and those with the same host are grouped into one group.
void relay_mail::build()
{
    for (auto& name : mail.to) {
        // <user@example.com> -> <user, example.com>
        auto a = angel::util::split(name.data(), name.data() + name.size(), '@');
        auto& host = a[1];

        auto it = task_map.find(host);
        if (it == task_map.end()) {
            auto task = std::make_unique<relay_task>();
            task->relay_mail = this;
            task->host = std::move(host); // example.com
            task->mail_to.emplace_back(std::move(name)); // <user@example.com>
            task->set_mx_name_list();
            task_map.emplace(task->host, std::move(task));
        } else {
            it->second->mail_to.emplace_back(std::move(name));
        }
    }
}

void relay_mail::start()
{
    for (auto& [host, task] : task_map) {
        task->start();
    }
    auto *loop = relay->relay_thread.get_loop();
    check_timer_id = loop->run_every(500, [this]{ this->check_relay_task(); });
}

void relay_mail::check_relay_task()
{
    for (auto it = task_map.begin(); it != task_map.end(); ) {
        if (it->second->is_ready()) {
            auto& res = it->second->f.get();
            if (!res.is_ok) {
                log_error("(relay): mail <%s> failed to <%s>: %s",
                          filename.c_str(), it->second->cur_mx_name.c_str(), res.err.c_str());
                relay_fail();
            }
            auto del_it = it++;
            task_map.erase(del_it);
        } else {
            ++it;
        }
    }
    if (task_map.empty()) {
        relay->relay_map.erase(filename);
        auto *loop = relay->relay_thread.get_loop();
        loop->cancel_timer(check_timer_id);
    }
}

// get <user> from <user@example.com>
static std::string get_username(std::string_view mail_name)
{
    const char *p = std::find(mail_name.begin(), mail_name.end(), '@');
    assert(p != mail_name.end());
    return std::string(mail_name.begin(), p);
}

void relay_task::start()
{
    if (mx_name_list.empty()) {
        printf("No MX name available\n");
        return;
    }
    auto& mail = relay_mail->mail;
    mail.to = mail_to;
    mail.headers["From"] = get_username(mail.from) + "<" + mail.from + ">\r\n";
    // To: address-list <crlf>
    for (auto& name : mail.to) {
        mail.headers["To"] = get_username(name) + "<" + name + ">,";
    }
    mail.headers["To"].pop_back();
    mail.headers["To"] += "\r\n";
    mail.headers["Subject"] = "hello";
    cur_mx_name = std::move(mx_name_list.front());
    f = relay_mail->relay->sender.send(cur_mx_name, 25, "", "", mail);
    mx_name_list.pop();
}

void relay_task::set_mx_name_list()
{
    auto res = relay_mail->relay->resolver->get_mx_name_list(host);
    for (auto name : res) {
        mx_name_list.emplace(std::move(name));
    }
    if (res.empty()) {
        mx_name_list.emplace(host);
    }
}

bool relay_task::is_ready()
{
    return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

}
