#pragma once

#include <QByteArray>

#include <utility>

#include <openssl/crypto.h>

class SecureBytes
{
public:
    SecureBytes() = default;
    explicit SecureBytes(QByteArray bytes) : m_bytes(std::move(bytes)) {}
    ~SecureBytes() { clear(); }
    SecureBytes(const SecureBytes&) = delete;
    SecureBytes& operator=(const SecureBytes&) = delete;
    SecureBytes(SecureBytes&& other) noexcept : m_bytes(std::move(other.m_bytes)) {}
    SecureBytes& operator=(SecureBytes&& other) noexcept
    {
        if (this != &other) {
            clear();
            m_bytes = std::move(other.m_bytes);
        }
        return *this;
    }
    QByteArray& bytes() { return m_bytes; }
    const QByteArray& bytes() const { return m_bytes; }
    bool isEmpty() const { return m_bytes.isEmpty(); }
    void clear()
    {
        if (!m_bytes.isEmpty())
            OPENSSL_cleanse(m_bytes.data(), static_cast<size_t>(m_bytes.size()));
        m_bytes.clear();
        m_bytes.squeeze();
    }
private:
    QByteArray m_bytes;
};
