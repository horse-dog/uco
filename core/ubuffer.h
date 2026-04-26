#pragma once

#include "ulog.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace uco
{

/**
 * @brief io buffer (not thread-safe).
 */
class buffer
{
  public:
    buffer(size_t sz = 1024)
        : m_VecBuffer(sz), m_iReadPosition(0), m_iWritePosition(0)
    {
    }

    ~buffer() {}

    size_t ReadableBytes() const { return m_iWritePosition - m_iReadPosition; }

    size_t WritableBytes() const
    {
        return m_VecBuffer.size() - m_iWritePosition;
    }

    const char *CurReadPos() const
    {
        return m_VecBuffer.data() + m_iReadPosition;
    }

    char *CurReadPos() { return m_VecBuffer.data() + m_iReadPosition; }

    const char *CurWritePos() const
    {
        return m_VecBuffer.data() + m_iWritePosition;
    }

    char *CurWritePos() { return m_VecBuffer.data() + m_iWritePosition; }

    // Return: nbytes retireved finally.
    size_t Retrieve(size_t nbytes)
    {
        size_t readable = ReadableBytes();
        if (nbytes >= readable)
        {
            RetriveAll();
            return readable;
        }
        else
        {
            m_iReadPosition += nbytes;
            return nbytes;
        }
    }

    void RetrieveUntil(const char *pos)
    {
        if (pos > CurReadPos() && pos < CurWritePos())
        {
            Retrieve(pos - CurReadPos());
        }
    }

    void RetriveAll() { Reset(); }

    void Advance(size_t nbytes)
    {
        size_t writable = WritableBytes();
        if (nbytes >= writable)
        {
            m_iWritePosition = m_VecBuffer.size();
        }
        else
        {
            m_iWritePosition += nbytes;
        }
    }

    void Append(const std::string &str) { Append(str.data(), str.size()); }

    void Append(const void *data, size_t len)
    {
        Append((const char *)(data), len);
    }

    void Append(const char *data, size_t len)
    {
        if (data == nullptr || len == 0)
            return;

        _M_EnsureWritableBytes(len);
        std::copy(data, data + len, CurWritePos());
        m_iWritePosition += len;
    }

    void Reset()
    {
        bzero(m_VecBuffer.data(), m_VecBuffer.size());
        m_iReadPosition = m_iWritePosition = 0;
    }

    void Compact()
    {
        size_t readable = ReadableBytes();
        if (readable == 0)
        {
            Reset();
        }
        else
        {
            std::memmove(m_VecBuffer.data(), CurReadPos(), readable);
            m_iReadPosition = 0;
            m_iWritePosition = readable;
        }
    }

    void ExtendSize(size_t size)
    {
        Compact();
        _M_ExtendBuffer(size);
    }

    void DoubleSize() { ExtendSize(m_VecBuffer.size()); }

  protected:
    void _M_EnsureWritableBytes(size_t len)
    {
        size_t writable = WritableBytes();
        if (writable < len)
        {
            _M_ExtendBuffer(len - writable);
        }
    }

    void _M_ExtendBuffer(size_t len)
    {
        size_t old_capacity = m_VecBuffer.size();
        size_t new_capacity = std::max(old_capacity + (old_capacity >> 1),
                                       old_capacity + len); // 1.5x
        if (new_capacity > INT32_MAX)
        {
            new_capacity = INT32_MAX;
            SYSERR("buffer extend too large, current size: %lu", new_capacity);
        }
        if (m_iReadPosition >= len)
        {
            Compact();
        }
        else
        {
            m_VecBuffer.resize(new_capacity);
        }
    }

  protected:
    std::vector<char> m_VecBuffer;
    size_t m_iReadPosition;
    size_t m_iWritePosition;
};

} // namespace uco
