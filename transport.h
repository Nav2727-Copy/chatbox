#pragma once

#include "common.h"

#include <boost/asio/ssl.hpp>

#include <memory>
#include <utility>

class TransportStream
{
public:
    using TlsStream = boost::asio::ssl::stream<tcp::socket>;

    TransportStream(tcp::socket socket, boost::asio::ssl::context* tls_context)
    {
        if (tls_context)
            tls_ = std::make_unique<TlsStream>(std::move(socket), *tls_context);
        else
            plain_ = std::make_unique<tcp::socket>(std::move(socket));
    }

    TransportStream(boost::asio::io_context& io,
        boost::asio::ssl::context* tls_context)
    {
        if (tls_context)
            tls_ = std::make_unique<TlsStream>(io, *tls_context);
        else
            plain_ = std::make_unique<tcp::socket>(io);
    }

    bool is_tls() const { return static_cast<bool>(tls_); }

    tcp::socket& lowest_layer()
    {
        return tls_ ? tls_->next_layer() : *plain_;
    }

    boost::asio::any_io_executor get_executor()
    {
        return lowest_layer().get_executor();
    }

    SSL* native_tls_handle()
    {
        return tls_ ? tls_->native_handle() : nullptr;
    }

    template <typename Handler>
    void async_server_handshake(Handler&& handler)
    {
        if (tls_)
            tls_->async_handshake(boost::asio::ssl::stream_base::server,
                std::forward<Handler>(handler));
        else
            boost::asio::post(get_executor(),
                [handler = std::forward<Handler>(handler)]() mutable
                {
                    handler(boost::system::error_code{});
                });
    }

    void client_handshake(boost::system::error_code& error)
    {
        if (tls_)
            tls_->handshake(boost::asio::ssl::stream_base::client, error);
        else
            error.clear();
    }

    template <typename DynamicBuffer, typename Handler>
    void async_read_until(DynamicBuffer& buffer, char delimiter, Handler&& handler)
    {
        if (tls_)
            boost::asio::async_read_until(*tls_, buffer, delimiter,
                std::forward<Handler>(handler));
        else
            boost::asio::async_read_until(*plain_, buffer, delimiter,
                std::forward<Handler>(handler));
    }

    template <typename ConstBufferSequence, typename Handler>
    void async_write(const ConstBufferSequence& buffer, Handler&& handler)
    {
        if (tls_)
            boost::asio::async_write(*tls_, buffer,
                std::forward<Handler>(handler));
        else
            boost::asio::async_write(*plain_, buffer,
                std::forward<Handler>(handler));
    }

    template <typename ConstBufferSequence>
    std::size_t write(const ConstBufferSequence& buffer,
        boost::system::error_code& error)
    {
        if (tls_)
            return boost::asio::write(*tls_, buffer, error);
        return boost::asio::write(*plain_, buffer, error);
    }

    template <typename DynamicBuffer>
    std::size_t read_until(DynamicBuffer& buffer, char delimiter,
        boost::system::error_code& error)
    {
        if (tls_)
            return boost::asio::read_until(*tls_, buffer, delimiter, error);
        return boost::asio::read_until(*plain_, buffer, delimiter, error);
    }

    void close()
    {
        boost::system::error_code ignored;
        lowest_layer().shutdown(tcp::socket::shutdown_both, ignored);
        lowest_layer().close(ignored);
    }

private:
    std::unique_ptr<tcp::socket> plain_;
    std::unique_ptr<TlsStream> tls_;
};
