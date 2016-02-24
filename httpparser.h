#ifndef HTTPPARSER_H
#define HTTPPARSER_H

#include <functional>

#include "http-parser/http_parser.h"

class HttpParser
{
public:
    using HttpDataCb  = std::function<int(const char *at, size_t length)>;
    using HttpStateCb = std::function<int()>;

    explicit HttpParser(enum http_parser_type type = HTTP_BOTH) noexcept;

    HttpParser(const HttpParser&) = delete;
    void operator=(const HttpParser&) = delete;

    HttpParser(HttpParser&&) = default;
    HttpParser& operator=(HttpParser&&) = default;

    void init(enum http_parser_type type = HTTP_BOTH) noexcept;

    size_t execute(const char* data, size_t length) noexcept;

    bool shouldKeepAlive() const noexcept;

    bool bodyIsFinal() const noexcept;

    http_errno error() const;

    const char *errorName() const;

    const char *errorDescription() const;

    const http_parser& parser() const noexcept;

    operator const http_parser*() const noexcept;

    const http_parser* operator->() const noexcept;

    //
    // Setup Handlers
    //

    // Helper macrosses
#define HTTP_STATE_CALLBACK(name, cfg_name) \
    private: \
    static int on ## name(http_parser *parser) \
    { \
        auto self = static_cast<HttpParser*>(parser->data); \
        return self->m_on ## name(); \
    } \
    public: \
    template<typename T> \
    void setOn ## name ## Handler(T cb) \
    { \
        setupHandler(cb, m_on ## name, m_settings.cfg_name, on ## name); \
    } \
    private: \
    HttpStateCb m_on ## name

#define HTTP_DATA_CALLBACK(name, cfg_name) \
    private: \
    static int on ## name(http_parser *parser, const char *at, size_t length) \
    { \
        auto self = static_cast<HttpParser*>(parser->data); \
        return self->m_on ## name(at, length); \
    } \
    public: \
    template<typename T> \
    void setOn ## name ## Handler(T cb) \
    { \
        setupHandler(cb, m_on ## name, m_settings.cfg_name, on ## name); \
    } \
    private: \
    HttpDataCb m_on ## name

    //
    // Declare callbacks wrappers
    //
    HTTP_STATE_CALLBACK(MessageBegin,    on_message_begin);
    HTTP_DATA_CALLBACK(Url,              on_url);
    HTTP_DATA_CALLBACK(Status,           on_status);
    HTTP_DATA_CALLBACK(HeaderField,      on_header_field);
    HTTP_DATA_CALLBACK(HeaderValue,      on_header_value);
    HTTP_STATE_CALLBACK(HeadersComplete, on_headers_complete);
    HTTP_DATA_CALLBACK(Body,             on_body);
    HTTP_STATE_CALLBACK(MessageComplete, on_message_complete);
    HTTP_STATE_CALLBACK(ChunkHeader,     on_chunk_header);
    HTTP_STATE_CALLBACK(ChunkComplete,   on_chunk_complete);

#undef HTTP_STATE_CALLBACK
#undef HTTP_DATA_CALLBACK

private:
    template<typename T, typename Y, typename N>
    void setupHandler(T cb, Y &memberField, N &settingsField, N wrapperProc)
    {
        memberField = cb;
        if (memberField) {
            settingsField = wrapperProc;
        } else {
            settingsField = nullptr;
        }
    }

private:
    http_parser_settings m_settings = http_parser_settings();
    http_parser          m_parser   = http_parser();
};

#endif // HTTPPARSER_H
