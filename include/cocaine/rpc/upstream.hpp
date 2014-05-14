/*
    Copyright (c) 2011-2013 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2013 Other contributors as noted in the AUTHORS file.

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

#ifndef COCAINE_IO_UPSTREAM_HPP
#define COCAINE_IO_UPSTREAM_HPP

#include "cocaine/asio/socket.hpp"
#include "cocaine/asio/tcp.hpp"

#include "cocaine/rpc/session.hpp"
#include "cocaine/rpc/channel.hpp"

namespace cocaine {

template<class Tag> class upstream;

namespace io {

class basic_upstream_t {
    const std::shared_ptr<session_t> session;
    const uint64_t index;

    struct states {
        enum values: int { active, sealed };
    };

    // NOTE: Sealed upstreams ignore any messages. At some point it might change to an explicit way
    // to show that the operation won't be completed.
    states::values state;

public:
    basic_upstream_t(const std::shared_ptr<session_t>& session_, uint64_t index_):
        session(session_),
        index(index_),
        state(states::active)
    { }

    template<class Event, typename... Args>
    void
    send(Args&&... args);

    void
    revoke();
};

template<class Event, typename... Args>
void
basic_upstream_t::send(Args&&... args) {
    if(state != states::active) {
        return;
    }

    if(std::is_same<typename io::event_traits<Event>::transition_type, void>::value) {
        state = states::sealed;
    }

    std::lock_guard<std::mutex> guard(session->mutex);

    if(session->ptr) {
        session->ptr->wr->write<Event>(index, std::forward<Args>(args)...);
    }
}

inline
void
basic_upstream_t::revoke() {
    session->revoke(index);
}

template<class Tag> class message_queue;

} // namespace io

template<class T>
class upstream {
    const std::shared_ptr<io::basic_upstream_t> ptr;

public:
    friend class io::message_queue<T>;

    upstream(const std::shared_ptr<io::basic_upstream_t>& upstream_):
        ptr(upstream_)
    { }

    // Protocol constraint for this upstream.
    typedef typename io::protocol<T>::type protocol;

    template<class Event, typename... Args>
    typename std::enable_if<
        std::is_same<typename Event::tag, T>::value
    >::type
    send(Args&&... args) {
        ptr->send<Event>(std::forward<Args>(args)...);
    }
};

} // namespace cocaine

#endif