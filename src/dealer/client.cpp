//
// Copyright (C) 2011-2012 Rim Zaidullin <creator@bash.org.ru>
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

#include <stdexcept>

#include <boost/current_function.hpp>

#include "cocaine/dealer/client.hpp"
#include "cocaine/dealer/response.hpp"
#include "cocaine/dealer/core/client_impl.hpp"

namespace cocaine {
namespace dealer {

client::client(const std::string& config_path) {
	impl_.reset(new client_impl(config_path));
}

client::~client() {
	impl_.reset();
}

boost::shared_ptr<response>
client::send_message(const void* data,
					 size_t size,
					 const message_path& path,
					 const message_policy& policy)
{
	boost::mutex::scoped_lock lock(mutex_);

	//std::cout << "+ enter send_message\n";
	boost::shared_ptr<message_iface> msg = get_impl()->create_message(data, size, path, policy);
	//std::cout << "+ message created\n";
	response* resp_ptr = new response(get_impl(), msg->uuid(), path);
	//std::cout << "+ response created\n";
	boost::shared_ptr<response> resp(resp_ptr);
	//std::cout << "+ message ptr created\n";
	get_impl()->send_message(msg, resp);
	//std::cout << "+ message sent\n";
	return resp;
}

inline boost::shared_ptr<client_impl>
client::get_impl() {
	if (impl_.get()) {
		return impl_;
	}
	else {
		throw internal_error("client_impl object is empty at: " + std::string(BOOST_CURRENT_FUNCTION));
	}
}

} // namespace dealer
} // namespace cocaine
