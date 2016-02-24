#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <resolv.h>
#include <errno.h>
#include <getopt.h>

#include <list>
#include <iostream>
#include <iomanip>
#include <vector>
#include <array>
#include <forward_list>
#include <functional>
#include <limits>
#include <cmath>
#include <cassert>
#include <thread>
#include <mutex>
#include <system_error>
#include <functional>
#include <type_traits>

#include <ev++.h>

#include "http-parser/http_parser.h"

#include "worker.h"
#include "httpparser.h"

using namespace std;

//
// Fwd decl
//
int setnonblock(int fd);

//
//
//
std::unique_ptr<WorkerPool> g_workerPool;

//
//
//
struct AbstractData
{
    virtual ~AbstractData();
    virtual ssize_t write(int fd) = 0;
    virtual bool eof() const = 0;
};

struct MemoryBuffer : AbstractData
{
    ssize_t write(int fd) override
    {
        auto sts = ::write(fd, data.data() + consumed, data.size() - consumed);
        if (sts > 0)
        {
            consumed += sts;
        }
        return sts;
    }

    bool eof() const override
    {
        return data.size() == consumed;
    }

    std::vector<uint8_t> data;
    size_t               consumed = 0;
};

struct StringBuffer : AbstractData
{
    ssize_t write(int fd) override
    {
        auto sts = ::write(fd, data.c_str() + consumed, data.size() - consumed);
        if (sts > 0)
        {
            consumed += sts;
        }
        return sts;
    }

    bool eof() const override
    {
        return data.size() == consumed;
    }

    std::string data;
    size_t      consumed = 0;
};

template<typename T>
struct PtrBuffer : AbstractData
{
    using Deleter = std::function<void(PtrBuffer*)>;

    PtrBuffer() = default;

    explicit PtrBuffer(T *buffer, size_t size, Deleter deleter = Deleter())
        : buffer(buffer),
          size(size),
          deleter(deleter)
    {
    }

    ~PtrBuffer() override
    {
        if (deleter)
            deleter(this);
    }

    ssize_t write(int fd) override
    {
        clog << "write generic data to socket, size:" << size << ", consumed: " << consumed << '\n';

        using ByteType = typename std::conditional<std::is_signed<T>::value, const char, const unsigned char>::type;

        auto sts = ::write(fd,
                           static_cast<ByteType*>(buffer) + consumed,
                           size - consumed);
        if (sts > 0)
        {
            consumed += sts;
        }
        return sts;
    }

    bool eof() const override
    {
        return size == consumed;
    }

    T *buffer = nullptr;
    size_t size     = 0;
    size_t consumed = 0;

    Deleter deleter;
};

struct FileReader : AbstractData
{
    FileReader() = default;

    explicit FileReader(int fd, size_t count = 8192) : fd(fd), count(count) {}

    ~FileReader() override
    {
        if (fd != -1)
            close(fd);
    }

    ssize_t write(int fd) override
    {
        clog << "write file to socket\n";
#ifdef __linux
        auto sts = sendfile(fd, this->fd, &_offset, this->count);
        if (sts >= 0 && static_cast<size_t>(sts) < this->count) {
            // Possible input file EOF
            using Stat = struct stat;
            auto st = Stat();
            if (fstat(this->fd, &st) == 0) {
                _eof = _offset == st.st_size;
            }
        }
        return sts;
#else
#error you must check `sendfile` implementation on your platform and write appropriate code
#endif
    }

    bool eof() const override
    {
        return _eof;
    }

    int    fd      = -1;
    size_t count   = 8192;
    bool   _eof    = false;
    off_t  _offset = 0;
};

AbstractData::~AbstractData()
{
}

//
//
//
class HttpSession {
private:
    enum class Type
    {
        Generic,
        Future, /// Future detect
        Publish,
        Play,
        Pull,
        Push
    };

    ev::loop_ref &m_loop;

    int    m_socket;
    ev::io m_readio;
    ev::io m_writeio;

    ev::idle m_idle;

    std::string m_directory;

#if 1
    std::array<char, 8192> m_readbuf;
#else
    // Check correct header parsing
    std::array<char, 5> m_readbuf;
#endif

    //
    // Output chain
    //
    std::list<std::unique_ptr<AbstractData>> m_out;

    //
    // Control
    //
    bool                 m_doDestroy = false;
    bool                 m_doDestroyAfterWrite = false;

    HttpParser           m_http{HTTP_REQUEST};
    bool                 m_doReset = false;
    size_t               m_messages = 0;

    std::string          m_url;
    std::string          m_headerField;
    std::string          m_headerValue;
    bool                 m_chunked = false;

    Type                 m_type{Type::Generic};
    bool                 m_doPublishing = false;

private:

    // effictivly a close and a destroy
    virtual ~HttpSession()
    {
        cout << "Destroy session" << endl;
    }

    void callback(ev::io &watcher, int revents)
    {
        if (revents & ev::ERROR) {
            perror("HttpSession: event error");
            return;
        }

        if (revents & ev::READ) {
            onRead(watcher);
        }

        if (revents & ev::WRITE) {
            onWrite(watcher);
        }

        if (m_doDestroy) {
            destroy();
            return;
        }
    }

    void onRead(ev::io &watcher)
    {
        static_cast<void>(watcher);

        //clog << "on read\n";

        ssize_t readed = read(m_socket, m_readbuf.data(), m_readbuf.size());

        if (readed <= 0) {
            destroyLater();
            return;
        }

        auto parsed = parserExecute(m_readbuf.data(), static_cast<size_t>(readed));
        if (parsed == 0) {
            if (m_http.error() != HPE_OK) {
                cerr << "Parser error: " << m_http.error()
                     << ", name=" << m_http.errorName()
                     << ", desc=" << m_http.errorDescription()
                     << endl;
                destroyLater();
                return;
            }
        }
    }


    void onWrite(ev::io &watcher)
    {
        static_cast<void>(watcher);

        // We work here under loop mutex protection

        while (!m_out.empty()) {

            auto &buffer = m_out.front();

            auto sts = buffer->write(m_socket);
            if (sts == 0) {
                if (!buffer->eof()) {
                    destroyLater();
                    return;
                }
            }

            if (sts < 0) {
                if (sts == EAGAIN || sts == EWOULDBLOCK)
                    break;

                destroyLater();
                return;
            }

            if (buffer->eof()) {
                m_out.pop_front();
            }
        }

        if (m_out.empty())
        {
            m_writeio.stop();
            m_idle.start();

            if (m_doDestroyAfterWrite)
                destroyLater();
        }

        //cout << "on write finish, chain output=" << m_out.empty() << endl;
    }

    void send(std::unique_ptr<AbstractData> &&data)
    {
        m_out.push_back(std::move(data));

        if (!m_writeio.is_active()) {
            if (m_idle.is_active())
                m_idle.stop();
            m_writeio.start();
        }
    }

    void send(AbstractData *data)
    {
        send(std::unique_ptr<AbstractData>(data));
    }

    void destroy()
    {
        m_readio.stop();
        m_writeio.stop();
        m_idle.stop();
        //shutdown(m_socket, SHUT_RDWR); // ???
        close(m_socket);               // close() should shutdown connection in both directions

        // destroy
        delete this;
    }

    void destroyLater()
    {
        m_doDestroy = true;
    }

    void onIdle(ev::idle &watcher, int /*revents*/)
    {
        int opt = 1;
        // setting NODELAY flushs buffers, but we are still in tcp cork mode
        setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        watcher.stop();
    }

    // Special case return code: 0 - ok, 1 - skip other processing, other - error
    int onHeadersComplete()
    {
        //clog << "Headers completed [1], keep-alive: " << http_should_keep_alive(&m_parser) << endl;

#if 0
        static uint8_t streamNotFound[] = "HTTP/1.1 404 Not found\r\n"
                                          "Content-Type: text/html\r\n"
                                          "Connection: close\r\n"
                                          "Content-Length: 44\r\n"
                                          "\r\n"
                                          "<html><body>Stream not found</body></html>\r\n";
#else
        static uint8_t streamNotFound[] = "HTTP/1.0 404 Not found\r\n"
                                          "Content-Type: text/html\r\n"
                                          "Content-Length: 0\r\n"
                                          "\r\n";
#endif

        auto &parser = m_http.parser();

        // Process last header
        processHeader();

        //
        cout << "Handle URL: " << m_url << endl;

        // We don't support HTTP 0.x and 2.x
        if (parser.http_major != 1) {
            destroyLater();
            return 1;
        }

        switch (parser.method)
        {
            case HTTP_GET:
            {
                struct http_parser_url url;
                http_parser_url_init(&url);
                auto sts = http_parser_parse_url(m_url.c_str(), m_url.size(), false, &url);

                bool notFound = false;
                string fname;

                using Stat = struct stat;
                Stat st;

                if (sts == 0 && url.field_set & (1<<UF_PATH)) {
                    fname = m_directory + '/' + m_url.substr(url.field_data[UF_PATH].off,
                                                             url.field_data[UF_PATH].len);

                    cout << "Request file: " << fname << endl;

                    auto sts = stat(fname.c_str(), &st);

                    // Not safe file open: we must check path to omit "out of work directory" reading

                    notFound = (sts == -1 || !S_ISREG(st.st_mode));

                } else {
                    notFound = true;
                }

                if (notFound) {
                    send(new PtrBuffer<const uint8_t>(&streamNotFound[0], sizeof(streamNotFound) - 1));
                    m_doDestroyAfterWrite = true;
                    return 1;
                }

                auto fd = open(fname.c_str(), O_RDONLY | O_EXCL);

#if 0
                //"Transfer-Encoding: chunked\r\n"
                static string headers = "HTTP/1.1 200 OK\r\n"
                                        "Content-Type: text/html\r\n"
                                        "Connection: close\r\n"
                                        "\r\n";
#else
                string headers = "HTTP/1.0 200 OK\r\n"
                                 "Content-Type: text/html\r\n"
                                 "Content-Length: " +
                                 std::to_string(st.st_size) +
                                 "\r\n"
                                 "\r\n";
#endif

                send(new PtrBuffer<const char>(headers.c_str(), headers.size()));
                send(new FileReader(fd));
                m_doDestroyAfterWrite = true;

                return 1;
            }

            default:
            {
                destroyLater();
                return -1;
            }
        }

        return 0;
    }

    void processHeader()
    {
        // Complete pair of Header:Value
        cout << m_headerField << ": " << m_headerValue << endl;
        // see: boost::iequals()
        if (strcasecmp(m_headerField.c_str(), "Transfer-Encoding") == 0)
        {
            if (strncasecmp(m_headerValue.c_str(), "chunked", 7) == 0)
            {
                m_chunked = true;
            }
        }

        m_headerField.clear();
        m_headerValue.clear();
    }

    void parserSetup()
    {
        m_http.setOnHeadersCompleteHandler(std::bind(&HttpSession::onHeadersComplete, this));

        m_http.setOnUrlHandler([this](const char *at, size_t length) {
            m_url.append(at, at + length);
            return 0;
        });


        m_http.setOnHeaderFieldHandler([this](const char *at, size_t length)
        {
            if (!m_headerValue.empty())
            {
                processHeader();
            }

            m_headerField.append(at, at + length);
            return 0;
        });

        m_http.setOnHeaderValueHandler([this](const char *at, size_t length)
        {
            m_headerValue.append(at, at + length);
            return 0;
        });

        m_http.setOnMessageBeginHandler([this]() {
            m_messages++;
            cout << "Begin message, cnt=" << m_messages << endl;

            // Second or later request in keep-alive session
            if (!m_url.empty())
            {
                m_url.clear();
            }

            return 0;
        });



//        m_http.setOnChunkHeaderHandler([this]() {
//            //cout << "Begin chunk, size=" << m_http.parser().content_length << endl;
//            return 0;
//        });



        m_http.setOnChunkCompleteHandler([this]() {
            //cout << "End chunk" << endl;

            // Produce chain here, if chunked transfer
            if (m_chunked)
            {
#if 0
                if (m_in)
                {
                    if (m_in.get() != m_inCur)
                        m_in->append(m_inCur);
                    m_inCur = nullptr;

                    // PRODUCE
                    produce();
                }
#endif
            }

            return 0;
        });



        m_http.setOnBodyHandler([this](const char* at, size_t len) {
            //cout << "Data portion, ptr=" << (void*)at << ", size=" << len << endl;

            // Produce chain here, if non-chunked transfer, otherwise aggregate chain
            //  or
            // Produce chain here, if session-type + container allow partial data processing,
            // in this case, we ignore chunked transfering option

#if 0
            constexpr size_t count = 8192;

            if (!m_in)
            {
                m_in = make_buffer_chain_shared(count);
                m_inCur = m_in.get();
            }

            size_t consume = 0;
            while (consume < len)
            {
                if (m_inCur == nullptr)
                {
                    m_inCur = make_buffer_chain(count);
                }

                auto buffer = m_inCur->buffer;

                // Copy data for output
                size_t toCopy = std::min(len - consume, buffer->nbytes());
                memcpy(buffer->dpos(), at + consume, toCopy);
                buffer->count += toCopy;

                consume += toCopy;

                // Buffer filled, attach to chain
                if (buffer->full())
                {
                    if (m_in.get() != m_inCur)
                        m_in->append(m_inCur); // now buffer owned by shared parent
                    m_inCur = nullptr;
                }
            }

            if (!m_chunked)
            {
                // PRODUCE
                produce();
            }
#endif
            return 0;
        });
    }

    void parserReset()
    {
        m_http.init(HTTP_REQUEST);
    }

    void parserRequesstReset()
    {
        m_doReset = true;
    }

    size_t parserExecute(const char *data, size_t size)
    {
        if (m_doReset)
            parserReset();
        return m_http.execute(data, size);
    }

public:
    HttpSession(int fd, ev::loop_ref &loop, const std::string &directory)
        : m_loop(loop),
          m_socket(fd),
          m_readio(loop),
          m_writeio(loop),
          m_idle(loop),
          m_directory(directory)
    {
        parserSetup();

        m_readio.set<HttpSession, &HttpSession::callback>(this);
        m_readio.set(m_socket, ev::READ);

        m_writeio.set<HttpSession, &HttpSession::callback>(this);
        m_writeio.set(m_socket, ev::WRITE);

        m_readio.start();

        m_idle.set<HttpSession, &HttpSession::onIdle>(this);
    }
};

class TcpServer
{
private:
    ev::io          m_io;
    int             m_socket;
    std::string     m_directory;

    std::vector<std::unique_ptr<ev::io>> m_acceptors;

public:

    void onAccept(ev::io &watcher, int revents)
    {
        if (EV_ERROR & revents) {
            perror("got invalid event");
            return;
        }

        while (true)
        {
            //cout << "New connection" << endl;

            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int fd = accept(watcher.fd, (struct sockaddr *)&client_addr, &client_len);
            if (fd < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    perror("accept error");
                return;
            }

            // make nonblock
            setnonblock(fd);

            int opt = 1;
            // let's set NODELAY to work faster, but don't care if doesn't work
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
            // let's also cork our connection to send in smaller number of
            // packets
            setsockopt(fd, IPPROTO_TCP, TCP_CORK, &opt, sizeof(opt));

            new HttpSession(fd, watcher.loop, m_directory);
        }
    }

    TcpServer(const std::string &ip, int port, const std::string &directory)
        : m_directory(directory)
    {
        std::cout << "Listening on port " << port << std::endl;

        struct sockaddr_in addr;

        m_socket = socket(PF_INET, SOCK_STREAM, 0);

        int flag = 1;
        if(setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0)
        {
            perror("setsockopt(SO_REUSEADDR)");
        }

        // Turn off Nagle Algorithm (TCP No Delay)
        setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (!ip.empty()) {
            if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) < 0) {
                perror("inet_pton");
                exit(1);
            }
        }

        // Bind socket to iface
        if (bind(m_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind");
            exit(1);
        }

        // make non blocked
        if (setnonblock(m_socket) < 0)  {
            perror("setnonblock");
            exit(1);
        }

        if (listen(m_socket, 128) < 0) {
            perror("listen");
            exit(1);
        }

        size_t threads = g_workerPool->m_workers.size();
        m_acceptors.reserve(threads);

        for (size_t i = 0; i < threads; ++i)
        {
            cout << "Add acceptor: " << (i+1) << endl;
            Worker &worker = *g_workerPool->m_workers[i];

            worker.modify([this,&worker](){
                m_acceptors.emplace_back( new ev::io(worker.m_loop) );
                ev::io &io = *m_acceptors.back();
                io.set<TcpServer, &TcpServer::onAccept>(this);
                io.start(m_socket, ev::READ);
            });

        }

        cout << "Ready to accept" << endl;
    }

    virtual ~TcpServer()
    {
        shutdown(m_socket, SHUT_RDWR);
        close(m_socket);
    }
};

// Simply adds O_NONBLOCK to the file descriptor of choice
int setnonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

// Set max open files for process
int setnofile(size_t count)
{
    struct rlimit maxfd = {count, count};
    return setrlimit(RLIMIT_NOFILE, &maxfd);
}

void signal_cb(ev::sig &signal, int /*revents*/)
{
    signal.loop.break_loop();
}

// -h <bind-ip-address>
// -p <listen-port>
// -d <directory>
// -w <work-threads>
static const char* s_opts = "h:p:d:w:nl:";

int main(int argc, char **argv) 
{
    int    port      = 8080;
    string bindip    = "127.0.0.1";
    string directory = "/tmp";
    size_t workers   = 0;
    bool   daemon    = true;
    string logfile   = "/home/box/webserver.log";

    int opt;
    while ((opt = getopt(argc, argv, s_opts)) != -1)
    {
        switch (opt)
        {
            case 'h':
                bindip = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'd':
                directory = optarg;
                break;
            case 'w':
                workers = static_cast<size_t>(atoi(optarg));
                break;
            case 'n':
                daemon = false;
                break;
            case 'l':
                logfile = optarg;
                break;
            default:
                exit(1);
        }
    }

    if (daemon)
    {
        auto pid = fork();

        if (pid > 0)
            exit(0);

        auto sid = setsid();
        if (sid < 0)
            exit(1);

        auto logfd = open(logfile.c_str(), O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (logfd < 0) {
            perror("can't open log file");
            exit(1);
        }

        dup2(logfd, STDOUT_FILENO);
        dup2(logfd, STDERR_FILENO);
        close(STDIN_FILENO);
        if (logfd > 2)
            close(logfd);

        clog << "===========================[ start server ]===========================\n";
    }

    if (setnofile(999999) < 0)
        perror("setnofile()");

    g_workerPool.reset(new WorkerPool(workers));

    ev::sig sio;
    sio.set<signal_cb>();
    sio.start(SIGTERM);

    ev::default_loop  loop;
    TcpServer         server(bindip, port, directory);

    cout << "Run main loop\n" << flush;
    loop.run(0);

    cout << "Stop workers...\n" << flush;

    // Stop workers
    for (auto &w : g_workerPool->m_workers)
    {
        w->stop();
        w->m_thread.join();
    }

    //
    cout << "Bye! Bye!\n" << flush;

    return 0;
}

