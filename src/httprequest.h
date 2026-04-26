#pragma once

#include "ubuffer.h"
#include "uio.h"

#include <string>
#include <string_view>
#include <unordered_map>

class HttpRequest
{

  public:
    enum ParseState
    {
        eParsingStart,       // parsing start.
        eParsingRequsetLine, // parsing request line.
        eParsingHeader,      // parsing requset header.
        eParsingBody,        // parsing requset body.
        eParsingFinish,      // parsing finished.
    };

    enum LineState
    {
        eLineOK,  // read a full line.
        eLineBad, // read a error line.
        eLineOpen // line not yet complete.
    };

    enum HttpState
    {
        eRequestIncomplete, // Incomplete request, need continue read client
                            // data.
        eRequestOK,         // Got a full customer request.
        eRequestBad,        // Client request with syntax error.
    };

    HttpRequest(int recvsec) : m_iReadTimeoutSec(recvsec) { Reset(); }

    std::string_view ReadBody();

  protected:
    friend class HttpContext;
    friend class HttpConnection;
    void Reset();

    // eRequestIncomplete: need read again.
    // eRequestOK: parse succeed.
    // eRequestBad: parse error.
    HttpState Parse();

    // attempt to get a full line.
    LineState ParseLine();
    bool ParseRequestLine();
    bool ParseHeader();
    bool ParseBody();

  protected:
    std::string m_sMethod;   // GET/POST etc.
    std::string m_sPath;     // request path.
    std::string m_sProtocol; // Http version.
    std::string m_sContentType;
    std::unordered_map<std::string, std::string> m_mapHeader;
    const char *m_sCurLine = 0;
    int m_iHttpVersionMajor = 0;
    int m_iHttpVersionMinor = 0;
    int m_iHttpRequsetContentLength = 0;
    int m_iReadPositionOffsetWhenParseLine = -1;
    int m_iReadTimeoutSec = 0;
    bool m_bKeepAlive = true;
    uco_time_t m_readReqTime = {0, 0};
    ParseState m_state = eParsingStart;
    uco::buffer m_buffer;
};
