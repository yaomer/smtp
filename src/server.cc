//
// SMTP Server
// See https://www.rfc-editor.org/rfc/rfc5321.html
//

#include "server.h"

#include <fcntl.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <regex>

#include "config.h"

namespace smtp {

static config *conf;

static const int max_mail_size = 1024 * 1024 * 70; // 70M
static const int max_cmdline_size = 512;
static const int ttl = 30 * 1000;

struct context {
    enum { Prepare, Ready, Mail, Rcpt, Data };
    bool cmd_verify(angel::buffer&);
    bool parse_mail(angel::buffer&);
    bool parse_rcpt(angel::buffer&);
    void bad_sequence(const angel::connection_ptr& conn);
    void do_mail_transaction(const angel::connection_ptr&, angel::buffer&);
    void ready_recv_data();
    bool recv_data(const angel::connection_ptr&, angel::buffer&);
    void write_to_file(const char *data, size_t len);
    void reset()
    {
        state = Ready;
        mail_from.clear();
        mail_to.clear();
        if (fd != -1) {
            close(fd);
            fd = -1;
        }
        if (filename != "") {
            unlink(filename.c_str());
            filename.clear();
        }
    }

    int state = Prepare;
    int cmd;
    int fd = -1;
    std::string filename;
    size_t recv_size;
    std::string mail_from;
    std::vector<std::string> mail_to;
};

bool mail_verify(std::string_view name)
{
    static std::regex r(
            "^[A-Za-z0-9]+([\\.\\-_]?[A-Za-z0-9]+)*@[A-Za-z0-9]+([\\.\\-_]?[A-Za-z0-9]+)*\\.[a-z]{2,6}$");
    return std::regex_match(name.begin(), name.end(), r);
}

static std::string generate_id()
{
    uuid_t uu;
    uuid_string_t out;
    uuid_generate(uu);
    uuid_unparse_lower(uu, out);
    return out;
}

static std::string get_mail_filename(std::string_view username)
{
    std::string filename(conf->queue_dir);
    return filename.append("/").append(username).append("-").append(generate_id()).append(".mail");
}

server::server(angel::evloop *loop, angel::inet_addr listen_addr)
    : smtp(loop, listen_addr)
{
    conf = config::get_config("");
    smtp.set_connection_handler([listen_addr](const angel::connection_ptr& conn){
            conn->format_send("220 %s Simple Mail Transfer Service Ready\r\n", listen_addr.to_host());
            conn->set_context(context());
            });
    smtp.set_message_handler([this](const angel::connection_ptr& conn, angel::buffer& buf){
            this->receive_mail(conn, buf);
            });
    smtp.set_connection_ttl(ttl);
    smtp.start();
}

enum CommandType {
    EHLO, HELO, MAIL, RCPT, DATA, RSET, VRFY, EXPN, HELP, NOOP, QUIT
};

static const char *ok = "250 OK\r\n";

// mail transaction: MAIL, RCPT(0 or more), DATA
void server::receive_mail(const angel::connection_ptr& conn, angel::buffer& buf)
{
    context& ctx = std::any_cast<context&>(conn->get_context());
    while (buf.readable() > 0) {

        if (ctx.state == context::Data) {
            if (!ctx.recv_data(conn, buf)) return;
            continue;
        }

        int crlf = buf.find_crlf();
        if (crlf < 0) break;

        if (crlf > max_cmdline_size) {
            conn->send("500 Command line too long.\r\n");
            goto retrieve;
        }

        if (!ctx.cmd_verify(buf)) {
            conn->send("500 Command unrecognized.\r\n");
            goto retrieve;
        }

        if (ctx.cmd == DATA || ctx.cmd == RSET || ctx.cmd == QUIT) {
            const char *start = buf.peek() + 4;
            const char *end = buf.peek() + crlf;
            if (std::find_if_not(start, end, isspace) != end) {
                conn->send("501 Command not accept parameters.\r\n");
                goto retrieve;
            }
        }

        switch (ctx.cmd) {
        case HELO:
        case VRFY:
        case EXPN:
        case HELP:
            conn->send("502 Command not implemented.\r\n");
            break;
        case NOOP:
            conn->send(ok);
            break;
        case EHLO:
            ctx.reset();
            conn->send(ok);
            break;
        case RSET:
            ctx.reset();
            conn->send(ok);
            break;
        case QUIT:
            conn->send("221 Service closing transmission channel\r\n");
            conn->close();
            return;
        default:
            ctx.do_mail_transaction(conn, buf);
            break;
        }
retrieve:
        buf.retrieve(crlf + 2);
    }
}

void context::bad_sequence(const angel::connection_ptr& conn)
{
    switch (cmd) {
    case MAIL: conn->send("503 Send command HELO/EHLO first.\r\n"); break;
    case RCPT: conn->send("503 Send command MAIL first.\r\n"); break;
    case DATA: conn->send("503 Send command RCPT first.\r\n"); break;
    }
}

void context::do_mail_transaction(const angel::connection_ptr& conn, angel::buffer& buf)
{
    switch (state) {
    case Ready:
        if (cmd != MAIL) {
            bad_sequence(conn);
        } else if (!parse_mail(buf)) {
            conn->send("501 Syntax error in arguments.\r\n");
        } else {
            conn->send(ok);
            state = Mail;
        }
        break;
    case Mail:
        if (cmd != RCPT) {
            bad_sequence(conn);
        } else if (!parse_rcpt(buf)) {
            conn->send("501 Syntax error in arguments.\r\n");
        } else {
            conn->send(ok);
            state = Rcpt;
        }
        break;
    case Rcpt:
        if (cmd == RCPT) {
            if (!parse_rcpt(buf)) {
                conn->send("501 Syntax error in arguments.\r\n");
            }
        } else if (cmd == DATA) {
            conn->send("354 Start mail input; end with <CRLF>.<CRLF>\r\n");
            ready_recv_data();
            state = Data;
        } else {
            bad_sequence(conn);
        }
        break;
    case Prepare:
        bad_sequence(conn);
        break;
    }
}

bool context::parse_mail(angel::buffer& buf)
{
    const char *p = buf.peek();
    p += 10; // skip 'MAIL FROM:'
    if (*p++ != '<') return false;
    const char *q = strchr(p, '>');
    if (!q) return false;
    std::string_view name(p, q - p);
    if (!mail_verify(name)) return false;
    mail_from = name;
    return true;
}

bool context::parse_rcpt(angel::buffer& buf)
{
    const char *p = buf.peek();
    p += 8; // skip 'RCPT TO:'
    if (*p++ != '<') return false;
    const char *q = strchr(p, '>');
    if (!q) return false;
    std::string_view name(p, q - p);
    if (!mail_verify(name)) return false;
    mail_to.emplace_back(name);
    return true;
}

void context::ready_recv_data()
{
    char tmpfile[] = "tmp.XXXXXX";
    mktemp(tmpfile);

    filename = conf->tmp_dir;
    filename.append("/").append(tmpfile);

    fd = open(filename.c_str(), O_RDWR | O_APPEND | O_CREAT, 0644);
    assert(fd >= 0);
    // Save sender and receivers
    std::string buf;
    uint16_t len = (uint16_t)mail_from.size();
    buf.append((const char*)&len, 2);
    buf.append(mail_from);
    uint32_t receiver = mail_to.size();
    buf.append((const char*)&receiver, 4);
    for (auto& name : mail_to) {
        len = name.size();
        buf.append((const char*)&len, 2);
        buf.append(name);
    }
    // Separate email text for readability
    buf.push_back('\n');
    write_to_file(buf.data(), buf.size());
    recv_size = 0;
}

bool context::recv_data(const angel::connection_ptr& conn, angel::buffer& buf)
{
    // 在没有读到结束标记<CRLF>.<CRLF>时，我们需要在buf中保留4个字符
    if (buf.readable() < 5) return false;
    int end = buf.find("\r\n.\r\n");
    if (end >= 0) {
        recv_size += end;
        if (recv_size > max_mail_size) {
            reset();
            conn->send("552 Too much mail data\r\n");
        } else {
            write_to_file(buf.peek(), end);
            fsync(fd);
            auto mail_filename = get_mail_filename(mail_from);
            rename(filename.c_str(), mail_filename.c_str());
            filename.clear();
            reset();
            conn->send(ok);
        }
        buf.retrieve(end + 5);
    } else {
        // 至少读到4KB再进行一次写操作
        if (buf.readable() < 4096) return false;
        size_t len = buf.readable() - 4;
        recv_size += len;
        if (recv_size <= max_mail_size) {
            write_to_file(buf.peek(), len);
        }
        buf.retrieve(len);
    }
    return true;
}

void context::write_to_file(const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            assert(0);
        }
        buf += n;
        len -= n;
    }
}

static const char *ehlo = "EHLO";
static const char *helo = "HELO";
static const char *mail = "MAIL FROM:";
static const char *rcpt = "RCPT TO:";
static const char *data = "DATA";
static const char *rset = "RSET";
static const char *vrfy = "VRFY";
static const char *expn = "EXPN";
static const char *help = "HELP";
static const char *noop = "NOOP";
static const char *quit = "QUIT";

bool context::cmd_verify(angel::buffer& buf)
{
    const char *s = nullptr;
    const char *p = buf.peek();
    char c = p[4];
    switch (*p) {
    case 'E': case 'e':
        if (++p >= buf.end()) return false;
        switch (*p) {
        case 'H': case 'h': s = ehlo; cmd = EHLO; break;
        case 'X': case 'x': s = expn; cmd = EXPN; break;
        }
        break;
    case 'H': case 'h':
        p += 3;
        if (p >= buf.end()) return false;
        switch (*p) {
        case 'O': case 'o': s = helo; cmd = HELO; break;
        case 'P': case 'p': s = help; cmd = HELP; break;
        }
        break;
    case 'R': case 'r':
        if (++p >= buf.end()) return false;
        switch (*p) {
        case 'C': case 'c': s = rcpt; c = ' '; cmd = RCPT; break;
        case 'S': case 's': s = rset; cmd = RSET; break;
        }
        break;
    case 'M': case 'm': s = mail; c = ' '; cmd = MAIL; break;
    case 'D': case 'd': s = data; cmd = DATA; break;
    case 'V': case 'v': s = vrfy; cmd = VRFY; break;
    case 'N': case 'n': s = noop; cmd = NOOP; break;
    case 'Q': case 'q': s = quit; cmd = QUIT; break;
    }
    return s && buf.starts_with_case(s) && isspace(c);
}

}
