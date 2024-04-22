#pragma once /*{export module co_async:http.http_server_utils;}*/

#include <co_async/std.hpp>                   /*{import std;}*/
#include <co_async/awaiter/task.hpp>          /*{import :awaiter.task;}*/
#include <co_async/http/http11.hpp>           /*{import :http.http11;}*/
#include <co_async/http/http_server.hpp>      /*{import :http.http_server;}*/
#include <co_async/iostream/directory_stream.hpp>/*{import :iostream.directory_stream;}*/
#include <co_async/system/socket.hpp>            /*{import :system.socket;}*/
#include <co_async/system/fs.hpp>                /*{import :system.fs;}*/
#include <co_async/system/pipe.hpp>                /*{import :system.pipe;}*/
#include <co_async/http/uri.hpp>                 /*{import :http.uri;}*/
#include <co_async/system/process.hpp>           /*{import :system.process;}*/

namespace co_async {

/*[export]*/ struct HTTPServerUtils {
    static std::string html_encode(std::string_view str) {
        std::string res;
        res.reserve(str.size());
        for (auto c: str) {
            switch (c) {
            case '&': res.append("&amp;"); break;
            case '"': res.append("&quot;"); break;
            case '\'': res.append("&apos;"); break;
            case '<': res.append("&lt;"); break;
            case '>': res.append("&gt;"); break;
            default: res.push_back(c);
            }
        }
        return res;
    }

    template <class Request>
    static Task<>
    make_ok_response(Request const &req, std::string_view body,
                     std::string contentType = "text/html;charset=utf-8") {
        HTTPResponse res{
            .status = 200,
            .headers =
                {
                    {"content-type", std::move(contentType)},
                },
        };
        co_await (co_await req.write_response(res)).write_body(body);
    }

    template <class Request>
    static Task<> make_response_from_directory(Request const &req, DirFilePath path) {
        auto dirPath = path.path().generic_string();
        std::string content = "<h1>Files in " + dirPath + ":</h1>";
        auto parentPath = path.path().parent_path().generic_string();
        content +=
            "<a href=\"/" + URI::url_encode_path(parentPath) + "\">..</a><br>";
        DirectoryStream dir(co_await fs_open(path, OpenMode::Directory));
        while (auto entry = co_await dir.getdirent()) {
            if (entry == ".." || entry == ".")
                continue;
            content += "<a href=\"/" + URI::url_encode_path(make_path(dirPath, *entry).generic_string()) +
                       "\">" + html_encode(*entry) + "</a><br>";
        }
        co_await make_ok_response(req, content);
    }

    template <class Request>
    static Task<> make_error_response(Request const &req, int status) {
        auto error =
            to_string(status) + " " + std::string(getHTTPStatusName(status));
        HTTPResponse res{
            .status = status,
            .headers =
                {
                    {"content-type", "text/html;charset=utf-8"},
                },
        };
        co_await (co_await req.write_response(res))
            .write_body(
                "<html><head><title>" + error +
                "</title></head><body><center><h1>" + error +
                "</h1></center><hr><center>co_async</center></body></html>");
    }

    template <class Request>
    static Task<>
    make_response_from_file_or_directory(Request const &req, DirFilePath path) {
        auto stat = co_await fs_stat(path, STATX_MODE);
        if (!stat) [[unlikely]] {
            co_return make_error_response(req, 404);
        }
        if (!stat->is_readable()) [[unlikely]] {
            co_return make_error_response(req, 403);
        }
        if (stat->is_directory()) {
            co_return co_await make_response_from_directory(req, std::move(path));
        }
        HTTPResponse res{
            .status = 200,
            .headers =
                {
                    {"content-type", guessContentTypeByExtension(
                                         path.path().extension().string())},
                },
        };
        co_await (co_await req.write_response(res)).write_body_stream(co_await FileIStream::open(path));
    }

    template <class Request>
    static Task<>
    make_response_from_path(Request const &req, std::filesystem::path path) {
        auto stat = co_await fs_stat(path, STATX_MODE);
        if (!stat) [[unlikely]] {
            co_return co_await make_error_response(req, 404);
        }
        if (!stat->is_readable()) [[unlikely]] {
            co_return co_await make_error_response(req, 403);
        }
        if (stat->is_directory()) {
            co_return co_await make_response_from_directory(req, path);
        }
        if (stat->is_executable()) {
            co_return co_await make_response_from_cgi_script(req, req, path);
        }
        HTTPResponse res{
            .status = 200,
            .headers =
                {
                    {"content-type", guessContentTypeByExtension(
                                         path.extension().string())},
                },
        };
        co_await (co_await req.write_response(res)).write_body_stream(co_await FileIStream::open(path));
    }

    template <class Request>
    static Task<> make_response_from_file(Request const &req, DirFilePath path) {
        auto stat = co_await fs_stat(path, STATX_MODE);
        if (!stat || stat->is_directory()) [[unlikely]] {
            co_return make_error_response(req, 404);
        }
        if (!stat->is_readable()) [[unlikely]] {
            co_return make_error_response(req, 403);
        }
        HTTPResponse res{
            .status = 200,
            .headers =
                {
                    {"content-type", guessContentTypeByExtension(
                                         path.path().extension().string())},
                },
        };
        co_await (co_await req.write_response(res)).write_body_stream(co_await FileIStream::open(path));
    }

    template <class Request>
    static Task<>
    make_response_from_cgi_script(Request const &req,
                                  std::filesystem::path path) {
        auto stat = co_await fs_stat(path, STATX_MODE);
        if (!stat || stat->is_directory()) [[unlikely]] {
            co_return co_await make_error_response(req, 404);
        }
        if (!stat->is_executable()) [[unlikely]] {
            co_return co_await make_error_response(req, 403);
        }
        HTTPHeaders headers;
        std::string content;
        auto proc = ProcessBuilder();
        proc.path(path, true);
        proc.inherit_env();
        proc.env("HTTP_PATH", req.uri.path);
        proc.env("HTTP_METHOD", req.method);
        for (auto const &[k, v]: req.uri.params) {
            for (char c: k) {
                if (!(('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_'))
                    [[unlikely]] {
                    goto skip1;
                }
            }
            proc.env("HTTP_GET_" + k, v);
        skip1:;
        }
        for (auto const &[k, v]: req.headers) {
            auto key = k;
            for (char &c: key) {
                if (c == '-') c = '_';
                if (!(('a' <= c && c <= 'z') || c == '_'))
                    [[unlikely]] {
                    goto skip2;
                }
            }
            proc.env("HTTP_HEADER_" + key, v);
        skip2:;
        }
        auto pipeOut = co_await make_pipe();
        auto pipeIn = co_await make_pipe();
        proc.open(0, pipeIn.reader());
        proc.open(1, pipeOut.writer());
        proc.open(2, 2);
        Pid pid = co_await proc.spawn();
        FileIStream reader(pipeOut.reader());
        FileOStream writer(pipeIn.writer());
        req.read_body_stream(writer);
        std::string line;
        while (true) {
            line.clear();
            if (!co_await reader.getline(line, '\n')) [[unlikely]] {
#if CO_ASYNC_DEBUG
                std::cerr << "unexpected eof in cgi header\n";
#endif
                co_return co_await make_error_response(req, 500);
            }
            if (line.empty()) {
                break;
            }
            auto pos = line.find(':');
            if (pos == std::string::npos) [[unlikely]] {
#if CO_ASYNC_DEBUG
                std::cerr << "invalid k-v pair in cgi header\n";
#endif
                co_return co_await make_error_response(req, 500);
            }
            headers.insert_or_assign(
                trim_string(lower_string(line.substr(0, pos))),
                trim_string(line.substr(pos + 1)));
        }
        int status = 200;
        if (auto statusOpt = headers.get("status", from_string<int>)) {
            status = *statusOpt;
            headers.erase("status");
        }
        co_await reader.getall(content);
        auto exited = co_await wait_process(pid);
        if (exited.status != 0) [[unlikely]] {
#if CO_ASYNC_DEBUG
            std::cerr << "cgi script exit failure\n";
#endif
            co_return co_await make_error_response(req, 500);
        }
        HTTPResponse res{
            .status = status,
            .headers = std::move(headers),
        };
        co_await (co_await req.write_response(res)).write_body(content);
    }
};

} // namespace co_async
