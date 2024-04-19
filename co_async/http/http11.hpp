#pragma once /*{export module co_async:http.http11;}*/

#include <co_async/std.hpp>                    /*{import std;}*/
#include <co_async/awaiter/task.hpp>           /*{import :awaiter.task;}*/
#include <co_async/utils/simple_map.hpp>       /*{import :utils.simple_map;}*/
#include <co_async/iostream/string_stream.hpp> /*{import :iostream.string_stream;}*/
#include <co_async/iostream/zlib_stream.hpp> /*{import :iostream/zlib_stream;}*/
#include <co_async/http/http_status_code.hpp>/*{import :http.http_status_code;}*/
#include <co_async/utils/string_utils.hpp>   /*{import :utils.string_utils;}*/
#include <co_async/system/fs.hpp>            /*{import :system.fs;}*/
#include <co_async/http/uri.hpp>             /*{import :http.uri;}*/

namespace co_async {

/*[export]*/ struct HTTPHeaders : SimpleMap<std::string, std::string> {
    using SimpleMap<std::string, std::string>::SimpleMap;
};

/*[export]*/ enum class HTTPTransferEncoding {
    Auto = 0,
    Identity,
    Chunked,
    Gzip,
    Compress,
    Deflate,
    Br,
};

/*[export]*/ struct HTTPRequest {
    std::string method;
    URI uri;
    HTTPHeaders headers;
    HTTPTransferEncoding encoding = HTTPTransferEncoding::Auto;

    auto repr() const {
        return std::make_tuple(method, uri, headers);
    }
};

/*[export]*/ struct HTTPResponse {
    int status;
    HTTPHeaders headers;
    HTTPTransferEncoding encoding = HTTPTransferEncoding::Auto;

    auto repr() const {
        return std::make_tuple(status, headers);
    }
};

/*[export]*/ template <class Sock>
struct HTTP11 {
    Sock sock;

    Task<> write_body_stream(auto const &resq, auto &&body) {
        using namespace std::string_view_literals;
        switch (resq.encoding) {
        case HTTPTransferEncoding::Auto: [[fallthrough]];
        case HTTPTransferEncoding::Chunked: {
            co_await sock.puts("transfer-encoding: chunked\r\n\r\n"sv);
            while (co_await body.fillbuf()) {
                auto bufSpan = body.rdbuf();
                auto n = bufSpan.size();
                char buf[sizeof(n) * 2 + 4] = {}, *ep = buf;
                *ep++ = '\r';
                *ep++ = '\n';
                do {
                    *ep++ = "01234567890ABCDEF"[n & 15];
                } while (n >>= 4);
                *ep++ = '\r';
                *ep++ = '\n';
                co_await sock.puts(std::string_view{buf, static_cast<std::size_t>(ep - buf)});
                co_await sock.putspan(bufSpan);
            }
            co_await sock.puts("\r\n0\r\n"sv);
        } break;
        case HTTPTransferEncoding::Gzip:
            co_await sock.puts("transfer-encoding: gzip\r\n\r\n"sv);
            break;
        case HTTPTransferEncoding::Compress:
            co_await sock.puts("transfer-encoding: compress\r\n\r\n"sv);
            break;
        case HTTPTransferEncoding::Deflate:
            co_await sock.puts("transfer-encoding: deflate\r\n\r\n"sv);
            break;
        case HTTPTransferEncoding::Br:
            co_await sock.puts("transfer-encoding: br\r\n\r\n"sv);
            break;
        case HTTPTransferEncoding::Identity:
            {
                auto content = co_await body.getall();
                co_await sock.puts("content-length: "sv);
                co_await sock.puts(to_string(content.size()));
                co_await sock.puts("\r\n\r\n"sv);
                co_await sock.puts(content);
            }
            break;
        }
        co_await sock.flush();
    }

    Task<> write_body(auto const &resq, std::string_view body) {
        using namespace std::string_view_literals;
        if (resq.encoding == HTTPTransferEncoding::Identity ||
            resq.encoding == HTTPTransferEncoding::Auto) {
            co_await sock.puts("content-length: "sv);
            co_await sock.puts(to_string(body.size()));
            co_await sock.puts("\r\n\r\n"sv);
            co_await sock.puts(body);
        } else {
            co_await write_body_stream(resq, StringIStream(body));
        }
        co_await sock.flush();
    }

    Task<> write_nobody(auto const &resq) {
        using namespace std::string_view_literals;
        co_await sock.puts("\r\n"sv);
        co_await sock.flush();
    }

    Task<bool> read_body_stream(auto const &resq, auto &&body) {
        using namespace std::string_view_literals;
        switch (resq.encoding) {
        case HTTPTransferEncoding::Auto: [[fallthrough]];
        case HTTPTransferEncoding::Identity: {
            if (auto n = resq.headers.get("content-length"sv, from_string<int>)) {
                co_await body.puts(co_await sock.getn(*n));
            }
        }
        case HTTPTransferEncoding::Chunked: {
            std::string line;
            while (true) {
                line.clear();
                if (!co_await sock.getline(line, "\r\n"sv)) [[unlikely]] {
                    co_return false;
                }
                auto n = std::strtoul(line.c_str(), nullptr, 16);
                if (n == 0) {
                    break;
                }
                line.clear();
                co_await sock.getn(line, n);
                co_await body.puts(line);
                line.clear();
                if (!co_await sock.dropn(2)) [[unlikely]] {
                    co_return false;
                }
            }
        } break;
        case HTTPTransferEncoding::Gzip: [[fallthrough]];
        case HTTPTransferEncoding::Compress: [[fallthrough]];
        case HTTPTransferEncoding::Deflate: [[fallthrough]];
        case HTTPTransferEncoding::Br: {
            std::string content = co_await sock.getall();
            co_await body.puts(content);
        } break;
        }
        co_await body.flush();
        co_return true;
    }

    Task<std::string> read_body(auto const &resq) {
        using namespace std::string_view_literals;
        switch (resq.encoding) {
        case HTTPTransferEncoding::Auto: [[fallthrough]];
        case HTTPTransferEncoding::Identity: {
            if (auto n = resq.headers.get("content-length"sv, from_string<int>)) {
                co_return co_await sock.getn(*n);
            }
            co_return {};
        }
        case HTTPTransferEncoding::Chunked: {
            std::string line, body;
            while (true) {
                line.clear();
                if (!co_await sock.getline(line, "\r\n"sv)) [[unlikely]] {
                    co_return {};
                }
                auto n = std::strtoul(line.c_str(), nullptr, 16);
                if (n == 0) {
                    break;
                }
                co_await sock.getn(body, n);
                line.clear();
                if (!co_await sock.dropn(2)) [[unlikely]] {
                    co_return {};
                }
            }
            co_return body;
        }
        default: {
            StringOStream os;
            co_await read_body_stream(resq, os);
            co_return os.release();
        }
        };
    }

    Task<> write_header(HTTPRequest const &req) {
        using namespace std::string_view_literals;
        co_await sock.puts(req.method);
        co_await sock.putchar(' ');
        co_await sock.puts(req.uri.dump());
        co_await sock.puts(" HTTP/1.1\r\n"sv);
        for (auto const &[k, v]: req.headers) {
            co_await sock.puts(k);
            co_await sock.puts(": "sv);
            co_await sock.puts(v);
            co_await sock.puts("\r\n"sv);
        }
        co_await sock.puts("connection: keep-alive\r\n"sv);
    }

    Task<bool> read_header(HTTPRequest &req) {
        using namespace std::string_view_literals;
        std::string line;
        if (!co_await sock.getline(line, "\r\n"sv))
            co_return false;
        auto pos = line.find(' ');
        if (pos == line.npos || pos == line.size() - 1) [[unlikely]] {
#if CO_ASYNC_DEBUG
            std::cerr << "WARNING: invalid HTTP request:\n\t[" + line + "]\n";
#endif
            throw std::invalid_argument("invalid http request: version");
        }
        req.method = line.substr(0, pos);
        auto pos2 = line.find(' ', pos + 1);
        if (pos2 == line.npos || pos2 == line.size() - 1) [[unlikely]] {
#if CO_ASYNC_DEBUG
            std::cerr << "WARNING: invalid HTTP request:\n\t[" + line + "]\n";
#endif
            throw std::invalid_argument("invalid http request: method");
        }
        req.uri = URI::parse(line.substr(pos + 1, pos2 - pos - 1));
        while (true) {
            line.clear();
            if (!co_await sock.getline(line, "\r\n"sv)) [[unlikely]] {
                co_return false;
            }
            if (line.empty()) {
                break;
            }
            auto pos = line.find(':');
            if (pos == line.npos || pos == line.size() - 1 ||
                line[pos + 1] != ' ') [[unlikely]] {
#if CO_ASYNC_DEBUG
                std::cerr << "WARNING: invalid HTTP request:\n\t[" + line +
                                 "]\n";
#endif
                throw std::invalid_argument("invalid http request: header");
            }
            auto key = line.substr(0, pos);
            for (auto &c: key) {
                if (c >= 'A' && c <= 'Z') {
                    c += 'a' - 'A';
                }
            }
            req.headers.insert_or_assign(std::move(key), line.substr(pos + 2));
        }

        co_return true;
    }

    Task<> write_header(HTTPResponse const &res) {
        using namespace std::string_view_literals;
        co_await sock.puts("HTTP/1.1 "sv);
        co_await sock.puts(to_string(res.status));
        co_await sock.putchar(' ');
        co_await sock.puts(getHTTPStatusName(res.status));
        co_await sock.puts("\r\n"sv);
        for (auto const &[k, v]: res.headers) {
            co_await sock.puts(k);
            co_await sock.puts(": "sv);
            co_await sock.puts(v);
            co_await sock.puts("\r\n"sv);
        }
        co_await sock.puts("connection: keep-alive\r\n"sv);
    }

    Task<bool> read_header(HTTPResponse &res) {
        using namespace std::string_view_literals;
        auto line = co_await sock.getline("\r\n"sv);
        if (line.empty())
            co_return false;
        if (line.size() <= 9 || line.substr(0, 7) != "HTTP/1."sv ||
            line[8] != ' ') [[unlikely]] {
#if CO_ASYNC_DEBUG
            std::cerr << "WARNING: invalid HTTP response:\n\t[" + line + "]\n";
#endif
            throw std::invalid_argument("invalid http response: version");
        }
        if (auto statusOpt = from_string<int>(line.substr(9, 3))) [[likely]] {
            res.status = *statusOpt;
        } else [[unlikely]] {
#if CO_ASYNC_DEBUG
            std::cerr << "WARNING: invalid HTTP response:\n\t[" + line + "]\n";
#endif
            throw std::invalid_argument("invalid http response: status");
        }
        while (true) {
            line.clear();
            if (!co_await sock.getline(line, "\r\n"sv)) [[unlikely]] {
                co_return false;
            }
            if (line.empty()) {
                break;
            }
            auto pos = line.find(':');
            if (pos == line.npos || pos == line.size() - 1 ||
                line[pos + 1] != ' ') [[unlikely]] {
#if CO_ASYNC_DEBUG
                std::cerr << "WARNING: invalid HTTP response:\n\t[" + line +
                                 "]\n";
#endif
                throw std::invalid_argument("invalid http response: header");
            }
            auto key = line.substr(0, pos);
            for (auto &c: key) {
                if (c >= 'A' && c <= 'Z') {
                    c += 'a' - 'A';
                }
            }
            res.headers.insert_or_assign(std::move(key), line.substr(pos + 2));
        }

        if (auto transEnc = res.headers.get("transfer-encoding"sv)) {
            static constexpr std::pair<std::string_view, HTTPTransferEncoding> encodings[] = {
                {"identity"sv, HTTPTransferEncoding::Identity},
                {"chunked"sv, HTTPTransferEncoding::Chunked},
                {"gzip"sv, HTTPTransferEncoding::Gzip},
                {"compress"sv, HTTPTransferEncoding::Compress},
                {"deflate"sv, HTTPTransferEncoding::Deflate},
                {"br"sv, HTTPTransferEncoding::Br},
            };
            for (auto const &[name, enc]: encodings) {
                if (*transEnc == name) {
                    res.encoding = enc;
                    break;
                }
            }
        }
        co_return true;
    }
};

} // namespace co_async
