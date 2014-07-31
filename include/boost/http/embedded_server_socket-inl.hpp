namespace boost {
namespace http {

inline
bool embedded_server_socket::outgoing_response_native_stream() const
{
    return flags & HTTP_1_1;
}

template<class Message, class CompletionToken>
typename asio::async_result<
    typename asio::handler_type<CompletionToken,
                                void(system::error_code)>::type>::type
embedded_server_socket::async_receive_message(Message &message,
                                              CompletionToken &&token)
{
    typedef typename asio::handler_type<
        CompletionToken, void(system::error_code)>::type Handler;

    Handler handler(std::forward<CompletionToken>(token));

    asio::async_result<Handler> result(handler);

    if (istate != http::incoming_state::empty) {
        handler(system::error_code{http_errc::out_of_order});
        return result.get();
    }

    if (used_size) {
        // Have cached some bytes from a previous read
        on_async_receive_message<READY>(std::move(handler), message,
                                        system::error_code{}, 0);
    } else {
        // TODO (C++14): move in lambda capture list
        channel.async_receive(asio::buffer(buffer + used_size),
                              [this,handler,&message]
                              (const system::error_code &ec,
                               std::size_t bytes_transferred) mutable {
            on_async_receive_message<READY>(std::move(handler), message, ec,
                                            bytes_transferred);
        });
    }

    return result.get();
}

template<class Message, class CompletionToken>
typename asio::async_result<
    typename asio::handler_type<CompletionToken,
                                void(system::error_code)>::type>::type
embedded_server_socket::async_receive_some_body(Message &message,
                                                CompletionToken &&token)
{
    typedef typename asio::handler_type<
        CompletionToken, void(system::error_code)>::type Handler;

    Handler handler(std::forward<CompletionToken>(token));

    asio::async_result<Handler> result(handler);

    if (istate != http::incoming_state::message_ready) {
        handler(system::error_code{http_errc::out_of_order});
        return result.get();
    }

    // TODO: abstract this code away
    // BEGINNING OF REDUNDANT CODE

    if (used_size) {
        // Have cached some bytes from a previous read
        on_async_receive_message<DATA>(std::move(handler), message,
                                       system::error_code{}, 0);
    } else {
        // TODO (C++14): move in lambda capture list
        channel.async_receive(asio::buffer(buffer + used_size),
                              [this,handler,&message]
                              (const system::error_code &ec,
                               std::size_t bytes_transferred) mutable {
            on_async_receive_message<DATA>(std::move(handler), message, ec,
                                           bytes_transferred);
        });
    }

    // END OF REDUNDANT CODE

    return result.get();
}


template<class Message, class CompletionToken>
typename asio::async_result<
    typename asio::handler_type<CompletionToken,
                                void(system::error_code)>::type>::type
embedded_server_socket::async_receive_trailers(Message &message,
                                               CompletionToken &&token)
{
    typedef typename asio::handler_type<
        CompletionToken, void(system::error_code)>::type Handler;

    Handler handler(std::forward<CompletionToken>(token));

    asio::async_result<Handler> result(handler);

    if (istate != http::incoming_state::body_ready) {
        handler(system::error_code{http_errc::out_of_order});
        return result.get();
    }

    // TODO: abstract this code away
    // BEGINNING OF REDUNDANT CODE

    if (used_size) {
        // Have cached some bytes from a previous read
        on_async_receive_message<END>(std::move(handler), message,
                                      system::error_code{}, 0);
    } else {
        // TODO (C++14): move in lambda capture list
        channel.async_receive(asio::buffer(buffer + used_size),
                              [this,handler,&message]
                              (const system::error_code &ec,
                               std::size_t bytes_transferred) mutable {
            on_async_receive_message<END>(std::move(handler), message, ec,
                                          bytes_transferred);
        });
    }

    // END OF REDUNDANT CODE

    return result.get();
}

template<class Message, class CompletionToken>
typename asio::async_result<
    typename asio::handler_type<CompletionToken,
                                void(system::error_code)>::type>::type
embedded_server_socket::async_write_message(Message &message,
                                            CompletionToken &&token)
{
    typedef typename asio::handler_type<
        CompletionToken, void(system::error_code)>::type Handler;

    Handler handler(std::forward<CompletionToken>(token));

    asio::async_result<Handler> result(handler);

    if (!writer_helper.write_message()) {
        handler(system::error_code{http_errc::out_of_order});
        return result.get();
    }

    auto crlf = asio::buffer("\r\n");
    auto sep = asio::buffer(": ");

    const auto nbuffer_pieces =
        // Start line + CRLF
        2
        // Headers
        // Each header is 4 buffer pieces: key + sep + value + crlf
        + 4 * message.headers.size()
        // Extra content-length header uses 3 pieces
        + 3
        // Extra CRLF for end of headers
        + 1
        // And finally, the message body
        + 1;

    std::vector<asio::const_buffer> buffers(nbuffer_pieces);

    buffers.push_back(asio::buffer(message.start_line));
    buffers.push_back(crlf);

    for (const auto &header: message.headers) {
        buffers.push_back(asio::buffer(header.first));
        buffers.push_back(sep);
        buffers.push_back(asio::buffer(header.second));
        buffers.push_back(crlf);
    }

    // because we don't create multiple responses at once with HTTP/1.1
    // pipelining, it's safe to use this "shared state"
    content_length_buffer = lexical_cast<std::string>(message.body.size());
    buffers.push_back(asio::buffer("content-length: "));
    buffers.push_back(asio::buffer(content_length_buffer));
    buffers.push_back(crlf);

    buffers.push_back(crlf);
    buffers.push_back(asio::buffer(message.body));

    asio::async_write(channel, buffers,
                      [handler]
                      (const system::error_code &ec, std::size_t) mutable {
        handler(ec);
    });

    return result.get();
}

template<int target, class Message, class Handler>
void embedded_server_socket
::on_async_receive_message(Handler handler, Message &message,
                           const system::error_code &ec,
                           std::size_t bytes_transferred)
{
    if (ec) {
        clear_buffer();
        handler(ec);
        return;
    }

    used_size += bytes_transferred;
    current_message = reinterpret_cast<void*>(&message);
    auto nparsed = execute(parser, settings<Message>(),
                           asio::buffer_cast<const std::uint8_t*>(buffer),
                           used_size);

    if (parser.http_errno) {
        system::error_code ignored_ec;

        if (parser.http_errno == int(parser_error::cb_headers_complete)) {
            clear_buffer();
            clear_message(message);

            const char error_message[]
            = "HTTP/1.1 505 HTTP Version Not Supported\r\n"
              "Content-Length: 48\r\n"
              "Connection: close\r\n"
              "\r\n"
              "This server only supports HTTP/1.0 and HTTP/1.1\n";
            asio::async_write(channel, asio::buffer(error_message),
                              [this,handler](system::error_code ignored_ec,
                                             std::size_t bytes_transferred)
                              mutable {
                channel.close(ignored_ec);
                handler(system::error_code{http_errc::parsing_error});
            });
            return;
        } else if (parser.http_errno
                   == int(parser_error::cb_message_complete)) {
            /* After an error is set, http_parser enter in an invalid state
               and needs to be reset. */
            init(parser);
        } else {
            clear_buffer();
            channel.close(ignored_ec);
            handler(system::error_code(http_errc::parsing_error));
            return;
        }
    }

    {
        auto b = asio::buffer_cast<std::uint8_t*>(buffer);
        std::copy_n(b + nparsed, used_size - nparsed, b);
    }

    used_size -= nparsed;

    if (target == READY && flags & READY) {
        flags &= ~READY;
        handler(system::error_code{});
    } else if (target == DATA && flags & (DATA|END)) {
        flags &= ~(READY|DATA);
        handler(system::error_code{});
    } else if (target == END && flags & END) {
        flags &= ~(READY|DATA|END);
        handler(system::error_code{});
    } else {
        // TODO (C++14): move in lambda capture list
        channel.async_receive(asio::buffer(buffer + used_size),
                              [this,handler,&message]
                              (const system::error_code &ec,
                               std::size_t bytes_transferred) mutable {
            on_async_receive_message<target>(std::move(handler), message,
                                             ec, bytes_transferred);
        });
    }
}

template</*class Buffer, */class Message>
detail::http_parser_settings embedded_server_socket::settings()
{
    http_parser_settings settings;

    settings.on_message_begin = on_message_begin<Message>;
    settings.on_url = on_url<Message>;
    settings.on_header_field = on_header_field<Message>;
    settings.on_header_value = on_header_value<Message>;
    settings.on_headers_complete = on_headers_complete<Message>;
    settings.on_body = on_body<Message>;
    settings.on_message_complete = on_message_complete<Message>;

    return settings;
}

template<class Message>
int embedded_server_socket::on_message_begin(http_parser *parser)
{
    auto socket = reinterpret_cast<embedded_server_socket*>(parser->data);
    auto message = reinterpret_cast<Message*>(socket->current_message);
    clear_message(*message);
    return 0;
}

template<class Message>
int embedded_server_socket::on_url(http_parser *parser, const char *at,
                                   std::size_t size)
{
    auto socket = reinterpret_cast<embedded_server_socket*>(parser->data);
    auto message = reinterpret_cast<Message*>(socket->current_message);

    /* Preferably, it should be on the on_headers_complete callback, but I
       want to avoid move operations inside the string. The tradeoff is the
       introduced branch misprediction. It can be fixed separating url and
       verb string or replacing the parser. */
    if (message->start_line.empty()) {
        /* TODO: Could compute the size at compile type with template magic,
           just like how it's done with Tufão. Optimization will go away
           with the parser replacement anyway. */
        const char *methods[] = {
            "DELETE ",
            "GET ",
            "HEAD ",
            "POST ",
            "PUT ",
            "CONNECT ",
            "OPTIONS ",
            "TRACE ",
            "COPY ",
            "LOCK ",
            "MKCOL ",
            "MOVE ",
            "PROPFIND ",
            "PROPPATCH ",
            "SEARCH ",
            "UNLOCK ",
            "REPORT ",
            "MKACTIVITY ",
            "CHECKOUT ",
            "MERGE ",
            "M-SEARCH ",
            "NOTIFY ",
            "SUBSCRIBE ",
            "UNSUBSCRIBE ",
            "PATCH ",
            "PURGE "
        };
        message->start_line = methods[parser->method];
    }

    message->start_line.append(at, size);
    return 0;
}

template<class Message>
int embedded_server_socket::on_header_field(http_parser *parser, const char *at,
                                            std::size_t size)
{
    /* The SG4's uri library also face the problem to define a
       case-insensitive interface and the chosen solution was to convert
       everything to lower case upon normalization. */
    using std::transform;
    int (*const tolower)(int) = std::tolower;

    auto socket = reinterpret_cast<embedded_server_socket*>(parser->data);
    auto message = reinterpret_cast<Message*>(socket->current_message);
    auto &field = socket->last_header.first;
    auto &value = socket->last_header.second;

    if (field.empty() /* last header piece was value */) {
        field.replace(0, field.size(), at, size);
        transform(field.begin(), field.end(), field.begin(), tolower);
    } else {
        if (value.size()) {
            if (!socket->use_trailers)
                message->headers.insert(socket->last_header);
            else
                message->trailers.insert(socket->last_header);
            value.clear();
        }

        auto offset = field.size();
        field.append(at, size);
        auto begin = field.begin() + offset;
        transform(begin, field.end(), begin, tolower);
    }

    return 0;
}

template<class Message>
int embedded_server_socket::on_header_value(http_parser *parser, const char *at,
                                            std::size_t size)
{
    auto socket = reinterpret_cast<embedded_server_socket*>(parser->data);
    auto &value = socket->last_header.second;
    value.append(at, size);
    return 0;
}

template<class Message>
int embedded_server_socket::on_headers_complete(http_parser *parser)
{
    auto socket = reinterpret_cast<embedded_server_socket*>(parser->data);
    auto message = reinterpret_cast<Message*>(socket->current_message);

    {
        auto handle_error = [](){
            /* WARNING: if you update the code and another error condition
               become possible, you'll be in trouble, because, as I write
               this comment, there is **NOT** a non-hacky way to notify
               different error conditions through the callback return value
               (Ryan Dahl's parser limitation, probably designed in favor of
               lower memory consumption) and then you'll need to call the
               type erased user handler from this very function. One
               solution with minimal performance impact to this future
               problem is presented below.

               First, update this function signature to also remember the
               erased type:

               ```cpp
               template<class Message, class Handler>
               static int on_headers_complete(http_parser *parser)
               ```

               Now add the following member to this class:

               ```cpp
               void *handler;
               ```

               Before the execute function is called, update the previously
               declared member:

               ```cpp
               this->handler = &handler;
               ```
               Now use the following to call the type erased user function
               from this very function:

               ```cpp
               auto &handler = *reinterpret_cast<Handler*>(socket->handler);
               handler(system::error_code{http_errc::parsing_error});
               ``` */
            /* We don't use http_parser_pause, because it looks error-prone:
               <https://github.com/joyent/http-parser/issues/97>. */
            return -1;
        };
        switch (parser->http_major) {
        case 1:
            switch (parser->http_minor) {
            case 0:
                message->start_line += " HTTP/1.0";
                break;
            case 1:
                message->start_line += " HTTP/1.1";
                socket->flags |= HTTP_1_1;
                break;
            default:
                return handle_error();
            }
            break;
        default:
            return handle_error();
        }
    }

    message->headers.insert(socket->last_header);
    socket->last_header.first.clear();
    socket->last_header.second.clear();
    socket->use_trailers = true;
    socket->istate = http::incoming_state::message_ready;
    socket->flags |= READY;

    if (should_keep_alive(*parser))
        socket->flags |= KEEP_ALIVE;

    return 0;
}

template<class Message>
int embedded_server_socket::on_body(http_parser *parser, const char *data,
                                    std::size_t size)
{
    auto socket = reinterpret_cast<embedded_server_socket*>(parser->data);
    auto message = reinterpret_cast<Message*>(socket->current_message);
    auto begin = reinterpret_cast<const std::uint8_t*>(data);
    message->body.insert(message->body.end(), begin, begin + size);
    socket->flags |= DATA;

    if (body_is_final(*parser))
        socket->istate = http::incoming_state::body_ready;

    return 0;
}

template<class Message>
int embedded_server_socket::on_message_complete(http_parser *parser)
{
    auto socket = reinterpret_cast<embedded_server_socket*>(parser->data);
    socket->istate = http::incoming_state::empty;
    socket->use_trailers = false;
    socket->flags |= END;

    /* To avoid passively parsing pipelined message ahead-of-asked, we
       signalize error to stop parsing. */
    return parser->upgrade ? -1 : 0;
}

inline
void embedded_server_socket::clear_buffer()
{
    istate = http::incoming_state::empty;
    writer_helper.state = http::outgoing_state::empty;
    used_size = 0;
    flags = 0;
    init(parser);
}

template<class Message>
void embedded_server_socket::clear_message(Message &message)
{
    message.start_line.clear();
    message.headers.clear();
    message.body.clear();
    message.trailers.clear();
}

} // namespace boost
} // namespace http
