//
// Copyright (C) 2011-2012 Andrey Sibiryov <me@kobology.ru>
//
// Licensed under the BSD 2-Clause License (the "License");
// you may not use this file except in compliance with the License.
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <boost/algorithm/string/join.hpp>

#include "cocaine/server/server.hpp"

#include "cocaine/context.hpp"
#include "cocaine/engine.hpp"
#include "cocaine/job.hpp"
#include "cocaine/logging.hpp"

#include "cocaine/interfaces/storage.hpp"

using namespace cocaine;
using namespace cocaine::engine;

server_t::server_t(context_t& context, server_config_t config):
    m_context(context),
    m_log(m_context.log("core")),
    m_server(m_context.io(), ZMQ_REP, m_context.config.runtime.hostname),
    m_auth(m_context),
    m_birthstamp(m_loop.now())
{
    int minor, major, patch;
    zmq_version(&major, &minor, &patch);

    m_log->info("using libev version %d.%d", ev_version_major(), ev_version_minor());
    m_log->info("using libmsgpack version %s", msgpack_version());
    m_log->info("using libzmq version %d.%d.%d", major, minor, patch);
    m_log->info("route to this node is '%s'", m_server.route().c_str());

    // Server socket
    // -------------

    int linger = 0;

    m_server.setsockopt(ZMQ_LINGER, &linger, sizeof(linger));

    for(std::vector<std::string>::const_iterator it = config.listen_endpoints.begin();
        it != config.listen_endpoints.end();
        ++it)
    {
        try {
            m_server.bind(*it);
        } catch(const zmq::error_t& e) {
            throw configuration_error_t(std::string("invalid listen endpoint - ") + e.what());
        }
            
        m_log->info("listening on %s", it->c_str());
    }
    
    m_watcher.set<server_t, &server_t::request>(this);
    m_watcher.start(m_server.fd(), ev::READ);
    m_processor.set<server_t, &server_t::process>(this);
    m_pumper.set<server_t, &server_t::pump>(this);
    m_pumper.start(0.2f, 0.2f);    

    // Autodiscovery
    // -------------

    if(!config.announce_endpoints.empty()) {
        m_announces.reset(new networking::socket_t(m_context.io(), ZMQ_PUB));
        m_announces->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));
        
        for(std::vector<std::string>::const_iterator it = config.announce_endpoints.begin();
            it != config.announce_endpoints.end();
            ++it)
        {
            try {
                m_announces->connect(*it);
            } catch(const zmq::error_t& e) {
                throw configuration_error_t(std::string("invalid announce endpoint - ") + e.what());
            }

            m_log->info("announcing on %s", it->c_str());
        }

        m_announce_timer.reset(new ev::timer());
        m_announce_timer->set<server_t, &server_t::announce>(this);
        m_announce_timer->start(0.0f, config.announce_interval);
    }

    // Signals
    // -------

    m_sigint.set<server_t, &server_t::terminate>(this);
    m_sigint.start(SIGINT);

    m_sigterm.set<server_t, &server_t::terminate>(this);
    m_sigterm.start(SIGTERM);

    m_sigquit.set<server_t, &server_t::terminate>(this);
    m_sigquit.start(SIGQUIT);

    m_sighup.set<server_t, &server_t::reload>(this);
    m_sighup.start(SIGHUP);
    
    recover();
}

server_t::~server_t() { }

void server_t::run() {
    m_loop.loop();
}

void server_t::terminate(ev::sig&, int) {
    if(!m_engines.empty()) {
        m_log->info("stopping the apps");
        m_engines.clear();
    }

    m_loop.unloop(ev::ALL);
}

void server_t::reload(ev::sig&, int) {
    m_log->info("reloading the apps");

    try {
        recover();
    } catch(const configuration_error_t& e) {
        m_log->error("unable to reload the apps - %s", e.what());
    } catch(const storage_error_t& e) {
        m_log->error("unable to reload the apps - %s", e.what());
    } catch(...) {
        m_log->error("unable to reload the apps - unexpected exception");
    }
}

void server_t::request(ev::io&, int) {
    if(m_server.pending() && !m_processor.is_active()) {
        m_processor.start();
    }
}

void server_t::process(ev::idle&, int) {
    if(!m_server.pending()) {
        m_processor.stop();
        return;
    }

    zmq::message_t message, signature;
    Json::Reader reader(Json::Features::strictMode());
    Json::Value root, response;

    m_server.recv(&message);

    if(reader.parse(
        static_cast<const char*>(message.data()),
        static_cast<const char*>(message.data()) + message.size(),
        root)) 
    {
        try {
            if(!root.isObject()) {
                throw configuration_error_t("json root must be an object");
            }

            unsigned int version = root["version"].asUInt();
            std::string username(root["username"].asString());
            
            if(version < 2 || version > 3) {
                throw configuration_error_t("unsupported protocol version");
            }
  
            if(version == 3) {
                if(m_server.more()) {
                    m_server.recv(&signature);
                }

                if(!username.empty()) {
                    m_auth.verify(
                        blob_t(message.data(), message.size()),
                        blob_t(signature.data(), signature.size()),
                        username
                    );
                } else {
                    throw authorization_error_t("username expected");
                }
            }

            response = dispatch(root);
        } catch(const authorization_error_t& e) {
            response = helpers::make_json("error", e.what());
        } catch(const configuration_error_t& e) {
            response = helpers::make_json("error", e.what());
        } catch(const storage_error_t& e) {
            response = helpers::make_json("error", e.what());
        } catch(...) {
            response = helpers::make_json("error", "unexpected exception");
        }
    } else {
        response = helpers::make_json("error", reader.getFormatedErrorMessages());
    }

    // Serialize and send the response.
    std::string json(Json::FastWriter().write(response));
    message.rebuild(json.size());
    memcpy(message.data(), json.data(), json.size());

    // Send in non-blocking mode in case the client has disconnected.
    m_server.send(message, ZMQ_NOBLOCK);
}

void server_t::pump(ev::timer&, int) {
    request(m_watcher, ev::READ);
}

Json::Value server_t::dispatch(const Json::Value& root) {
    std::string action(root["action"].asString());

    if(action == "create" || action == "delete") {
        Json::Value apps(root["apps"]),
                    result(Json::objectValue);

        if(!apps.isArray() || !apps.size()) {
            throw configuration_error_t("no apps have been specified");
        }

        for(Json::Value::iterator it = apps.begin(); it != apps.end(); ++it) {
            std::string app((*it).asString());

            try {
                if(action == "create") {
                    result[app] = create_engine(app);
                } else if(action == "delete") {
                    result[app] = delete_engine(app);                
                }
            } catch(const configuration_error_t& e) {
                result[app]["error"] = e.what();
            } catch(...) {
                result[app]["error"] = "unexpected exception";
            }
        }

        return result;
    } else if(action == "info") {
        return info();
    } else {
        throw configuration_error_t("unsupported action");
    }
}

// Commands
// --------

Json::Value server_t::create_engine(const std::string& name) {
    if(m_engines.find(name) != m_engines.end()) {
        throw configuration_error_t("the specified app already exists");
    }

    std::auto_ptr<engine_t> engine(
        new engine_t(
            m_context,
            name
        )
    );

    engine->start();

    Json::Value result(engine->info());

    m_engines.insert(name, engine);
    
    return result;
}

Json::Value server_t::delete_engine(const std::string& name) {
    engine_map_t::iterator engine(m_engines.find(name));

    if(engine == m_engines.end()) {
        throw configuration_error_t("the specified app does not exists");
    }

    engine->second->stop();

    Json::Value result(engine->second->info());

    m_engines.erase(engine);

    return result;
}

Json::Value server_t::info() {
    Json::Value result(Json::objectValue);

    result["route"] = m_server.route();

    for(engine_map_t::const_iterator it = m_engines.begin();
        it != m_engines.end(); 
        ++it) 
    {
        result["apps"][it->first] = it->second->info();
    }

    result["jobs"]["pending"] = static_cast<Json::UInt>(job_t::objects_alive);
    result["jobs"]["processed"] = static_cast<Json::UInt>(job_t::objects_created);

    result["loggers"] = static_cast<Json::UInt>(logging::logger_t::objects_alive);

    result["uptime"] = m_loop.now() - m_birthstamp;

    return result;
}

void server_t::announce(ev::timer&, int) {
    m_log->debug("announcing the node");

    zmq::message_t message(m_server.endpoint().size());
 
    memcpy(message.data(), m_server.endpoint().data(), m_server.endpoint().size());
    m_announces->send(message, ZMQ_SNDMORE);

    std::string announce(Json::FastWriter().write(info()));
    
    message.rebuild(announce.size());
    memcpy(message.data(), announce.data(), announce.size());
    m_announces->send(message);
}

void server_t::recover() {
    // NOTE: Allowing the exception to propagate here, as this is a fatal error.
    std::vector<std::string> apps(m_context.storage("core")->list("apps")),
                             diff;

    std::set<std::string> available(apps.begin(), apps.end()),
                          active;
  
    for(engine_map_t::const_iterator it = m_engines.begin();
        it != m_engines.end();
        ++it)
    {
        active.insert(it->first);
    }

    // Generate a list of apps which are either new or dead.
    std::set_symmetric_difference(active.begin(), active.end(),
                                  available.begin(), available.end(),
                                  std::back_inserter(diff));

    if(diff.size()) {
        for(std::vector<std::string>::const_iterator it = diff.begin();
            it != diff.end(); 
            ++it)
        {
            if(m_engines.find(*it) == m_engines.end()) {
                create_engine(*it);
            } else {
                m_engines.find(*it)->second->stop();
            }
        }
    }
}
