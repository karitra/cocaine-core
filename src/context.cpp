/*
    Copyright (c) 2011-2014 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2014 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cocaine/context.hpp"

#include "cocaine/api/service.hpp"
#include "cocaine/context/config.hpp"
#include "cocaine/context/filter.hpp"
#include "cocaine/context/mapper.hpp"
#include "cocaine/detail/essentials.hpp"
#include "cocaine/detail/trace/logger.hpp"
#include "cocaine/format.hpp"
#include "cocaine/logging.hpp"

#include <asio/io_service.hpp>

#include <blackhole/logger.hpp>
#include <blackhole/scope/holder.hpp>
#include <blackhole/wrapper.hpp>

#include <boost/algorithm/string/join.hpp>
#include <boost/optional/optional.hpp>

#include <metrics/registry.hpp>

#include <deque>



namespace cocaine {

namespace {

struct match {
    template<class T>
    bool
    operator()(const T& service) const {
        return name == service.first;
    }

    const std::string& name;
};

}

using namespace cocaine::io;

using blackhole::scope::holder_t;

class context_impl_t : public context_t {
    typedef std::deque<std::pair<std::string, std::unique_ptr<api::service_t>>> service_list_t;

    // TODO: There was an idea to use the Repository to enable pluggable sinks and whatever else for
    // for the Blackhole, when all the common stuff is extracted to a separate library.
    std::unique_ptr<logging::trace_wrapper_t> m_log;

    // NOTE: This is the first object in the component tree, all the other dynamic components, be it
    // storages or isolates, have to be declared after this one.
    std::unique_ptr<api::repository_t> m_repository;

    // Services are stored as a vector of pairs to preserve the initialization order. Synchronized,
    // because services are allowed to start and stop other services during their lifetime.
    synchronized<service_list_t> m_services;

    //TODO: signalling hub

    // Metrics.
    metrics::registry_t m_metrics_registry;

    std::unique_ptr<config_t> m_config;

    // Service port mapping and pinning.
    port_mapping_t m_mapper;

public:
    context_impl_t(std::unique_ptr<config_t> _config,
                   std::unique_ptr<logging::logger_t> _log,
                   std::unique_ptr<api::repository_t> _repository) :
        m_log(new logging::trace_wrapper_t(std::move(_log))),
        m_repository(std::move(_repository)),
        m_config(std::move(_config)),
        m_mapper(*m_config)
    {
        const holder_t scoped(*m_log, {{"source", "core"}});

        reset_logger_filter();

        COCAINE_LOG_INFO(m_log, "initializing the core");

        // Load the builtin plugins.
        essentials::initialize(*m_repository);

        // Load the rest of plugins.
        m_repository->load(m_config->path().plugins());

        // Spin up all the configured services, launch execution units.
        COCAINE_LOG_INFO(m_log, "starting {:d} execution unit(s)", m_config->network().pool());

        COCAINE_LOG_INFO(m_log, "starting {:d} service(s)", m_config->services().size());

        std::vector<std::string> errored;

        m_config->services().each([&](const std::string& name, const config_t::component_t& service) mutable {
            const holder_t scoped(*m_log, {{"service", name}});

            const auto asio = std::make_shared<asio::io_service>();

            COCAINE_LOG_DEBUG(m_log, "starting service");

            try {
                //TODO: init service
            } catch (const std::system_error& e) {
                COCAINE_LOG_ERROR(m_log, "unable to initialize service: {}", error::to_string(e));
                errored.push_back(name);
            } catch (const std::exception& e) {
                COCAINE_LOG_ERROR(m_log, "unable to initialize service: {}", e.what());
                errored.push_back(name);
            }
        });

        if (!errored.empty()) {
            COCAINE_LOG_ERROR(m_log, "emergency core shutdown");

            // Signal and stop all the services, shut down execution units.
            terminate();

            const auto errored_str = boost::algorithm::join(errored, ", ");

            throw cocaine::error_t("couldn't start core because of {} service(s): {}",
                                   errored.size(), errored_str
            );
        } else {
            //m_signals.invoke<io::context::prepared>();
            //TODO: signaling
        }
    }

    ~context_impl_t() {
        const holder_t scoped(*m_log, {{"source", "core"}});

        // Signal and stop all the services, shut down execution units.
        terminate();
    }

    std::unique_ptr<logging::logger_t>
    log(const std::string& source) {
        return log(source, {});
    }

    std::unique_ptr<logging::logger_t>
    log(const std::string& source, blackhole::attributes_t attributes) {
        attributes.push_back({"source", {source}});

        // TODO: Make it possible to use in-place operator+= to fill in more attributes?
        return std::make_unique<blackhole::wrapper_t>(*m_log, std::move(attributes));
    }

    void
    logger_filter(filter_t new_filter) {
        m_log->filter(std::move(new_filter));
    }

    void
    reset_logger_filter() {
        auto config_severity = m_config->logging().severity();
        auto filter = [=](filter_t::severity_t severity, filter_t::attribute_pack&) -> bool {
            return severity >= config_severity || !trace_t::current().empty();
        };
        logger_filter(filter_t(std::move(filter)));
    }


    api::repository_t&
    repository() const {
        return *m_repository;
    }

    metrics::registry_t&
    metrics_hub() {
        return m_metrics_registry;
    }

    const config_t&
    config() const {
        return *m_config;
    }

    port_mapping_t&
    mapper(){
        return m_mapper;
    }


    void
    terminate() {
        COCAINE_LOG_INFO(m_log, "stopping {:d} service(s)", m_services->size());

        // There should be no outstanding services left. All the extra services spawned by others, like
        // app invocation services from the node service, should be dead by now.
        BOOST_ASSERT(m_services->empty());

        reset_logger_filter();

        COCAINE_LOG_INFO(m_log, "core has been terminated");
    }
};

std::unique_ptr<context_t>
make_context(std::unique_ptr<config_t> config, std::unique_ptr<logging::logger_t> log) {
    std::unique_ptr<logging::logger_t> repository_logger(new blackhole::wrapper_t(*log, {}));
    std::unique_ptr<api::repository_t> repository(new api::repository_t(std::move(repository_logger)));
    return make_context(std::move(config), std::move(log), std::move(repository));
}

std::unique_ptr<context_t>
make_context(std::unique_ptr<config_t> config, std::unique_ptr<logging::logger_t> log, std::unique_ptr<api::repository_t> repository) {
    return std::unique_ptr<context_t>(new context_impl_t(std::move(config), std::move(log), std::move(repository)));
}

} //  namespace cocaine
