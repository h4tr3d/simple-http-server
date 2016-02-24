#include "httpparser.h"


HttpParser::HttpParser(enum http_parser_type type) noexcept
{
    m_parser.data = this;
    init(type);
}

void HttpParser::init(http_parser_type type) noexcept
{
    http_parser_init(&m_parser, type);
}

size_t HttpParser::execute(const char *data, size_t length) noexcept
{
    return http_parser_execute(&m_parser, &m_settings, data, length);
}

bool HttpParser::shouldKeepAlive() const noexcept
{
    return http_should_keep_alive(&m_parser);
}

bool HttpParser::bodyIsFinal() const noexcept
{
    return http_body_is_final(&m_parser);
}

http_errno HttpParser::error() const
{
    return HTTP_PARSER_ERRNO(&m_parser);
}

const char *HttpParser::errorName() const
{
    return http_errno_name(error());
}

const char *HttpParser::errorDescription() const
{
    return http_errno_description(error());
}

const http_parser &HttpParser::parser() const noexcept
{
    return m_parser;
}

const http_parser *HttpParser::operator->() const noexcept
{
    return &m_parser;
}

HttpParser::operator const http_parser *() const noexcept
{
    return &m_parser;
}

