#include "httprequest.h"

#include <cstdio>
#include <string>

HttpRequest::LineState HttpRequest::ParseLine()
{
    char *rpos = m_buffer.CurReadPos();
    char *wpos = m_buffer.CurWritePos();

    if (m_iReadPositionOffsetWhenParseLine > 0)
        rpos += m_iReadPositionOffsetWhenParseLine;
    m_iReadPositionOffsetWhenParseLine = 0;

    char *end = 0;
    for (; rpos < wpos; ++rpos)
    {
        char __c = *rpos;
        if (__c == '\r')
        {
            if (rpos + 1 == wpos)
            {
                m_iReadPositionOffsetWhenParseLine = m_buffer.ReadableBytes();
                return eLineOpen;
            }
            else if (rpos[1] == '\n')
            {
                rpos[0] = '\0';
                rpos[1] = '\0';
                end = rpos + 2;
                // Note: if buffer modified, the address of m_sCurLine will be
                // invalid.
                m_sCurLine = m_buffer.CurReadPos();
                m_buffer.RetrieveUntil(end);
                return eLineOK;
            }
            else
                return eLineBad;
        }
        else if (__c == '\n')
        {
            if (rpos - m_buffer.CurReadPos() > 0 && *(rpos - 1) == '\r')
            {
                *(rpos - 1) = '\0';
                *(rpos) = '\0';
                end = rpos + 1;
                // Note: if buffer modified, the address of m_sCurLine will be
                // invalid.
                m_sCurLine = m_buffer.CurReadPos();
                m_buffer.RetrieveUntil(end);
                return eLineOK;
            }
            return eLineBad;
        }
    }
    m_iReadPositionOffsetWhenParseLine = m_buffer.ReadableBytes();
    return eLineOpen;
}

HttpRequest::HttpState HttpRequest::Parse()
{
    if (m_state == eParsingStart)
    {
        m_state = eParsingRequsetLine;
    }
    LineState linestat = eLineOK;
    while (m_state != eParsingBody && (linestat = ParseLine()) == eLineOK)
    {
        switch (m_state)
        {
        case eParsingRequsetLine:
            if (!ParseRequestLine())
                return eRequestBad;
            break;
        case eParsingHeader:
            if (strlen(m_sCurLine) != 0)
            {
                if (!ParseHeader())
                    return eRequestBad;
            }
            else
            { // __line is \r\n.
                m_state = eParsingBody;
            }
            break;
        default:
            break;
        }
    }

    if (m_state == eParsingBody)
    {
        if (!ParseBody())
            return eRequestIncomplete;
        else
            return eRequestOK;
    }

    if (linestat == eLineOpen)
        return eRequestIncomplete;
    else
        return eRequestBad;
}

bool HttpRequest::ParseHeader()
{
    const char* colonPos = std::strchr(m_sCurLine, ':');
    if (!colonPos)
        return false;

    size_t keyLength = colonPos - m_sCurLine;
    std::string key(m_sCurLine, keyLength);

    const char* valStart = colonPos + 1;
    while (*valStart == ' ') valStart++;
    
    std::string val(valStart);

    size_t keyEnd = key.find_last_not_of(' ');
    if (keyEnd != std::string::npos)
        key = key.substr(0, keyEnd + 1);

    size_t valEnd = val.find_last_not_of(' ');
    if (valEnd != std::string::npos)
        val = val.substr(0, valEnd + 1);

    if (key == "Connection")
    {
        m_bKeepAlive = (val == "keep-alive");
    }

    if (key == "Content-Length")
        m_iHttpRequsetContentLength = std::atoi(val.c_str());

    if (key == "Content-Type")
        m_sContentType = val;

    m_mapHeader[key] = val;
    return true;
}

bool HttpRequest::ParseBody()
{
    bool ret = m_buffer.ReadableBytes() >= m_iHttpRequsetContentLength;
    if (ret)
        m_state = eParsingFinish;
    return ret;
}

static void parse_http_version(std::string_view protocol, int& major, int& minor)
{
    auto dot = protocol.find('.');
    if (dot == std::string_view::npos)
    {
        std::from_chars(protocol.data(), protocol.data() + protocol.size(), major);
        return;
    }

    std::from_chars(protocol.data(), protocol.data() + dot, major);
    std::from_chars(protocol.data() + dot + 1,
                    protocol.data() + protocol.size(), minor);
}

bool HttpRequest::ParseRequestLine()
{
    const char* methodEnd = std::strchr(m_sCurLine, ' ');
    if (!methodEnd)
        return false;

    m_sMethod = std::string(m_sCurLine, methodEnd - m_sCurLine); // 提取 method

    const char* pathEnd = std::strchr(methodEnd + 1, ' ');
    if (!pathEnd)
        return false;

    m_sPath = std::string(methodEnd + 1, pathEnd - methodEnd - 1); // 提取 path

    const char* protocolStart = pathEnd + 1;
    if (std::strncmp(protocolStart, "HTTP/", 5) != 0)
        return false;

    m_sProtocol = protocolStart + 5;
    parse_http_version(m_sProtocol, m_iHttpVersionMajor, m_iHttpVersionMinor);
    if (m_iHttpVersionMajor == 1 && m_iHttpVersionMinor == 0)
    {
        m_bKeepAlive = false;
    }

    m_state = eParsingHeader;
    return true;
}

void HttpRequest::Reset()
{
    m_sMethod.clear();
    m_sPath.clear();
    m_sProtocol.clear();
    m_sContentType.clear();
    m_mapHeader.clear();
    m_sCurLine = nullptr;
    m_iHttpVersionMajor = 1;
    m_iHttpVersionMinor = 1;
    m_iHttpRequsetContentLength = 0;
    m_iReadPositionOffsetWhenParseLine = -1;
    m_bKeepAlive = true;
    m_readReqTime = {m_iReadTimeoutSec, 0};
    m_state = eParsingStart;
    m_buffer.Reset();
}

std::string_view HttpRequest::ReadBody()
{
    if (m_state == eParsingFinish)
        return std::string_view(m_buffer.CurReadPos(),
                                m_iHttpRequsetContentLength);
    else
        return std::string_view();
}
