/***************************************************************************\
    server.cpp - iqdb server (database maintenance and queries)

    Copyright (C) 2008 piespy@gmail.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\**************************************************************************/

#include <csignal>
#include <cstddef>
#include <cstring>
#include <string>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <iqdb/debug.h>
#include <iqdb/imgdb.h>
#include <iqdb/imglib.h>
#include <iqdb/haar_signature.h>
#include <iqdb/types.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

using nlohmann::json;
using httplib::Server;
using iqdb::IQDB;

namespace iqdb {

static Server server;

static void signal_handler(int signal, siginfo_t* info, void* ucontext) {
  INFO("Received signal {} ({})\n", signal, strsignal(signal));

  if (signal == SIGSEGV) {
    INFO("Address: {}\n", info->si_addr);
    DEBUG("{}", get_backtrace(2));
    exit(1);
  }

  if (server.is_running()) {
    server.stop();
  }
}

void install_signal_handlers() {
  struct sigaction action = {};
  sigfillset(&action.sa_mask);
  action.sa_flags = SA_RESTART | SA_SIGINFO;

  action.sa_sigaction = signal_handler;

  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGSEGV, &action, NULL);
}

void http_server(const std::string host, const int port, const std::string database_filename) {
  INFO("Starting server...\n");

  std::shared_mutex mutex_;
  auto memory_db = std::make_unique<IQDB>(database_filename);

  install_signal_handlers();

  // GET /images/:id
  //    get the info about an image based on an image ID
  //
  server.Get("/images/(\\d+)", [&](const auto &request, auto &response) {
    std::unique_lock lock(mutex_);

    const postId post_id = std::stoi(request.matches[1]);
    INFO("getting post_id {}\n", post_id);
    auto image = memory_db->getImage(post_id);
    auto haar = image->haar();

    json data;
    if (image == std::nullopt) {
      response.status = 404;
      data = {
        { "error", {
          { "code", response.status },
          { "message", "Image not found" }
        }}
      };
    } else {
      response.status = 200;
      data = {
        { "post_id", post_id },
        { "hash", haar.to_string() },
        { "signature", {
          { "avglf", haar.avglf },
          { "sig", haar.sig }
        }}
      };
    };

    response.set_content(data.dump(4), "application/json");
  });

  server.Post("/images/(\\d+)", [&](const auto &request, auto &response) {
    std::unique_lock lock(mutex_);

    if (!request.has_file("file")) {
      response.status = 400;
      json data = {
          { "error", {
            { "code", response.status },
            { "message", "`POST /images/:id` requires a `file` param" }
          }}
      };
      response.set_content(data.dump(4), "application/json");
      throw iqdb::param_error("`POST /images/:id` requires a `file` param");
    };

    const postId post_id = std::stoi(request.matches[1]);
    const auto &file = request.get_file_value("file");
    const auto signature = HaarSignature::from_file_content(file.content);
    memory_db->addImage(post_id, signature);

    json data = {
      { "post_id", post_id },
      { "hash", signature.to_string() },
      { "signature", {
        { "avglf", signature.avglf },
        { "sig", signature.sig }
      }}
    };

    response.status = 200;
    response.set_content(data.dump(4), "application/json");
  });

  server.Delete("/images/(\\d+)", [&](const auto &request, auto &response) {
    std::unique_lock lock(mutex_);

    const postId post_id = std::stoi(request.matches[1]);
    int status = memory_db->removeImage(post_id);

    json data;
    if (status == 200) {
        response.status = 200;
        data = {
          { "post_id", post_id }
        };
    } else if (status == 404) {
        response.status = 404;
        data = {
          { "error", {
            { "code", response.status },
            { "message", "Post doesn't exists!" }
          }},
          { "post_id", post_id }
        };
    };

    response.set_content(data.dump(4), "application/json");
  });

  server.Post("/query", [&](const auto &request, auto &response) {
    std::shared_lock lock(mutex_);

    int limit = 10;
    sim_vector matches;
    json data = json::array();

    if (request.has_param("limit"))
      limit = stoi(request.get_param_value("limit"));

    if (request.has_param("hash")) {
      const auto hash = request.get_param_value("hash");
      HaarSignature haar = HaarSignature::from_hash(hash);
      matches = memory_db->queryFromSignature(haar, limit);
    } else if (request.has_file("file")) {
      const auto &file = request.get_file_value("file");
      matches = memory_db->queryFromBlob(file.content, limit);
    } else {
      response.status = 400;
      json data = {
        { "error", {
          { "code", response.status },
          { "message", "`POST /query` requires a `file` or `hash` param" }
        }}
      };
      response.set_content(data.dump(4), "application/json");
      throw param_error("`POST /query` requires a `file` or `hash` param");
    }

    for (const auto &match : matches) {
      auto image = memory_db->getImage(match.id);
      auto haar = image->haar();

      data += {
        { "post_id", match.id },
        { "score", match.score },
        { "hash", haar.to_string() },
        { "signature", {
          { "avglf", haar.avglf },
          { "sig", haar.sig },
        }}
      };
    }

    response.status = 200;
    response.set_content(data.dump(4), "application/json");
  });

  server.Get("/status", [&](const auto &request, auto &response) {
    std::shared_lock lock(mutex_);

    const size_t count = memory_db->getImgCount();
    json data = {{"images", count}};

    response.status = 200;
    response.set_content(data.dump(4), "application/json");
  });

  server.set_logger([](const auto &req, const auto &res) {
    INFO("{} \"{} {} {}\" {} {}\n", req.remote_addr, req.method, req.path, req.version, res.status, res.body.size());
  });

  server.set_exception_handler([](const auto& req, auto& res, std::exception &e) {
    const auto name = demangle_name(typeid(e).name());
    const auto message = e.what();

    json data = {
      { "exception", name },
      { "message", message },
      { "backtrace", last_exception_backtrace }
    };

    DEBUG("Exception: {} ({})\n{}\n", name, message, last_exception_backtrace);

    res.set_content(data.dump(4), "application/json");
    res.status = 500;
  });

  INFO("Listening on {}:{}.\n", host, port);
  server.listen(host.c_str(), port);
  INFO("Stopping server...\n");
}

void show_usage() {
  printf(
    "Usage: iqdb [-d=N] COMMAND [ARGS...]\n"
    "\n"
    "  iqdb http [host] [port] [dbfile]   Run HTTP server on given host/port.\n"
    "  iqdb help                          Show this help.\n"
    "\n"
    "Options:\n"
    "  -d=N       The logging verbosity level, where N is any of the integers below:\n"
    "             0 = DEBUG, 1 = ERROR, 2 = WARN, 3 = INFO\n"
  );
}

}
