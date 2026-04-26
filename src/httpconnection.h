#pragma once

#include "uco.h"

#include <arpa/inet.h>

#include "httprequest.h"
#include "httpresponce.h"
#include "httpserver.h"

class HttpConnection
{

  public:
    HttpConnection(int sock, sockaddr_in &addr,
                   class HttpServer::HttpServerInstance *server);

    /**
     * @brief read data from socket to readbuf.
     *
     * @return
     *  true: read some data success (maybe incomplete).
     * false: read error, please close conn.
     */
    uco::task<bool> Read();

    /**
     * @brief process data.
     *
     * @return
     *  true: parse success or parse error,
     *     responce data was set at writebuf.
     * false: parse need continue, please read more data from socket.
     */
    uco::task<bool> Process();

    /**
     * write writebuf to socket.
     *
     * @return
     *  true: write all data successfully.
     * false: write error or write finished with
     *        keep-alive=false, please close conn.
     */
    uco::task<bool> Write();

    uco::task<void> CloseConnection();

    bool IsKeepAlive() const { return m_httpRequest.m_bKeepAlive; }

  protected:
    uco::task<void> GenErrorPage(int code);

    int m_iSocket;
    sockaddr_in m_sockAddr;
    HttpServer::HttpServerInstance *m_pHttpServer;
    HttpRequest m_httpRequest;
    HttpResponse m_httpResponse;
};
