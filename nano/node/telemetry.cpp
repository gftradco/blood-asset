#include <nano/lib/alarm.hpp>
#include <nano/lib/worker.hpp>
#include <nano/node/network.hpp>
#include <nano/node/telemetry.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/buffer.hpp>

#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <cstdint>
#include <future>
#include <numeric>
#include <set>

nano::telemetry::telemetry (nano::network & network_a, nano::alarm & alarm_a, nano::worker & worker_a, bool disable_ongoing_requests_a) :
network (network_a),
alarm (alarm_a),
worker (worker_a),
disable_ongoing_requests (disable_ongoing_requests_a)
{
}

void nano::telemetry::start ()
{
	// Cannot be done in the constructor as a shared_from_this () call is made in ongoing_req_all_peers
	if (!disable_ongoing_requests)
	{
		ongoing_req_all_peers (std::chrono::milliseconds (0));
	}
}

void nano::telemetry::stop ()
{
	stopped = true;
}

void nano::telemetry::set (nano::telemetry_data const & telemetry_data_a, nano::endpoint const & endpoint_a, bool is_empty_a)
{
	if (!stopped)
	{
		nano::lock_guard<std::mutex> guard (mutex);
		auto it = recent_or_initial_request_telemetry_data.find (endpoint_a);
		if (it == recent_or_initial_request_telemetry_data.cend ())
		{
			// Not requesting telemetry data from this peer so ignore it
			return;
		}

		recent_or_initial_request_telemetry_data.modify (it, [&telemetry_data_a](nano::telemetry_info & telemetry_info_a) {
			telemetry_info_a.data = telemetry_data_a;
			telemetry_info_a.undergoing_request = false;
		});

		channel_processed (endpoint_a, is_empty_a);
	}
}

bool nano::telemetry::within_cache_cutoff (telemetry_info const & telemetry_info) const
{
	auto is_within = (telemetry_info.last_request + nano::telemetry_cache_cutoffs::network_to_time (network_params.network)) >= std::chrono::steady_clock::now ();
	return !telemetry_info.awaiting_first_response () && is_within;
}

void nano::telemetry::ongoing_req_all_peers (std::chrono::milliseconds next_request_interval)
{
	// Check if any peers actually need requesting
	alarm.add (std::chrono::steady_clock::now () + next_request_interval, [this_w = std::weak_ptr<telemetry> (shared_from_this ())]() {
		if (auto this_l = this_w.lock ())
		{
			// Check if there are any peers which are in the peers list which haven't been request, or any which are below or equal to the cache cutoff time
			if (!this_l->stopped)
			{
				auto peers = this_l->network.list (std::numeric_limits<size_t>::max (), this_l->network_params.protocol.telemetry_protocol_version_min, false);

				{
					std::unordered_set<nano::endpoint> temp_peers;
					std::transform (peers.begin (), peers.end (), std::inserter (temp_peers, temp_peers.end ()), [](auto const & channel_a) {
						return channel_a->get_endpoint ();
					});

					// Cleanup any stale saved telemetry data for non-existent peers
					nano::lock_guard<std::mutex> guard (this_l->mutex);
					for (auto it = this_l->recent_or_initial_request_telemetry_data.begin (); it != this_l->recent_or_initial_request_telemetry_data.end ();)
					{
						if (!it->undergoing_request && !this_l->within_cache_cutoff (*it) && temp_peers.count (it->endpoint) == 0)
						{
							it = this_l->recent_or_initial_request_telemetry_data.erase (it);
						}
						else
						{
							++it;
						}
					}

					peers.erase (std::remove_if (peers.begin (), peers.end (), [&this_l](auto const & channel_a) {
						// Remove from peers list if it exists and is within the cache cutoff
						auto it = this_l->recent_or_initial_request_telemetry_data.find (channel_a->get_endpoint ());
						return it != this_l->recent_or_initial_request_telemetry_data.end () && this_l->within_cache_cutoff (*it);
					}),
					peers.end ());
				}

				// Request data from new peers, or ones which are out of date
				for (auto const & peer : peers)
				{
					this_l->get_metrics_single_peer_async (peer, [](auto const &) {
						// Intentionally empty, just using to refresh the cache
					});
				}

				nano::lock_guard<std::mutex> guard (this_l->mutex);
				long long next_round = std::chrono::duration_cast<std::chrono::milliseconds> (nano::telemetry_cache_cutoffs::network_to_time (this_l->network_params.network)).count ();
				if (!this_l->recent_or_initial_request_telemetry_data.empty ())
				{
					// Use the default request time unless a telemetry request cache expires sooner
					auto const cache_cutoff = nano::telemetry_cache_cutoffs::network_to_time (this_l->network_params.network);
					auto const last_request = this_l->recent_or_initial_request_telemetry_data.get<tag_last_updated> ().begin ()->last_request;
					if (std::chrono::steady_clock::now () > last_request + cache_cutoff)
					{
						next_round = std::min<long long> (next_round, std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - (last_request + cache_cutoff)).count ());
					}
				}

				this_l->ongoing_req_all_peers (std::chrono::milliseconds (next_round));
			}
		}
	});
}

std::unordered_map<nano::endpoint, nano::telemetry_data> nano::telemetry::get_metrics ()
{
	std::unordered_map<nano::endpoint, nano::telemetry_data> telemetry_data;

	nano::lock_guard<std::mutex> guard (mutex);
	auto range = boost::make_iterator_range (recent_or_initial_request_telemetry_data);

	// clang-format off
	nano::transform_if (range.begin (), range.end (), std::inserter (telemetry_data, telemetry_data.end ()),
		[this](auto const & telemetry_info) { return this->within_cache_cutoff (telemetry_info); },
		[](auto const & telemetry_info) { return std::pair<const nano::endpoint, nano::telemetry_data>{ telemetry_info.endpoint, telemetry_info.data }; });
	// clang-format on

	return telemetry_data;
}

void nano::telemetry::get_metrics_single_peer_async (std::shared_ptr<nano::transport::channel> const & channel_a, std::function<void(telemetry_data_response const &)> const & callback_a)
{
	auto invoke_callback_with_error = [&callback_a, &worker = this->worker, channel_a]() {
		nano::endpoint endpoint;
		if (channel_a)
		{
			endpoint = channel_a->get_endpoint ();
		}
		worker.push_task ([callback_a, endpoint]() {
			auto const error = true;
			callback_a ({ nano::telemetry_data{}, endpoint, error });
		});
	};

	if (!stopped)
	{
		if (channel_a && (channel_a->get_network_version () >= network_params.protocol.telemetry_protocol_version_min))
		{
			auto add_callback_async = [& worker = this->worker, &callback_a](telemetry_data const & telemetry_data_a, nano::endpoint const & endpoint_a) {
				telemetry_data_response telemetry_data_response_l{ telemetry_data_a, endpoint_a, false };
				worker.push_task ([telemetry_data_response_l, callback_a]() {
					callback_a (telemetry_data_response_l);
				});
			};

			// Check if this is within the cache
			nano::lock_guard<std::mutex> guard (mutex);
			auto it = recent_or_initial_request_telemetry_data.find (channel_a->get_endpoint ());
			if (it != recent_or_initial_request_telemetry_data.cend () && within_cache_cutoff (*it))
			{
				add_callback_async (it->data, it->endpoint);
			}
			else
			{
				if (it != recent_or_initial_request_telemetry_data.cend () && it->undergoing_request)
				{
					// A request is currently undergoing, add the callback
					debug_assert (callbacks.count (it->endpoint) > 0);
					callbacks[it->endpoint].push_back (callback_a);
				}
				else
				{
					if (it == recent_or_initial_request_telemetry_data.cend ())
					{
						recent_or_initial_request_telemetry_data.emplace (channel_a->get_endpoint (), nano::telemetry_data (), std::chrono::steady_clock::now (), true);
						it = recent_or_initial_request_telemetry_data.find (channel_a->get_endpoint ());
					}
					else
					{
						recent_or_initial_request_telemetry_data.modify (it, [](nano::telemetry_info & telemetry_info_a) {
							telemetry_info_a.undergoing_request = true;
							telemetry_info_a.last_request = std::chrono::steady_clock::now ();
						});
					}
					callbacks[it->endpoint].push_back (callback_a);
					fire_request_message (channel_a);
				}
			}
		}
		else
		{
			invoke_callback_with_error ();
		}
	}
	else
	{
		invoke_callback_with_error ();
	}
}

nano::telemetry_data_response nano::telemetry::get_metrics_single_peer (std::shared_ptr<nano::transport::channel> const & channel_a)
{
	std::promise<telemetry_data_response> promise;
	get_metrics_single_peer_async (channel_a, [&promise](telemetry_data_response const & single_metric_data_a) {
		promise.set_value (single_metric_data_a);
	});

	return promise.get_future ().get ();
}

void nano::telemetry::fire_request_message (std::shared_ptr<nano::transport::channel> const & channel)
{
	// Fire off a telemetry request to all passed in channels
	debug_assert (channel->get_network_version () >= network_params.protocol.telemetry_protocol_version_min);

	uint64_t round_l;
	{
		auto it = recent_or_initial_request_telemetry_data.find (channel->get_endpoint ());
		recent_or_initial_request_telemetry_data.modify (it, [](nano::telemetry_info & telemetry_info_a) {
			++telemetry_info_a.round;
		});
		round_l = it->round;
	}

	std::weak_ptr<nano::telemetry> this_w (shared_from_this ());
	nano::telemetry_req message;
	// clang-format off
	channel->send (message, [this_w, endpoint = channel->get_endpoint ()](boost::system::error_code const & ec, size_t size_a) {
		if (auto this_l = this_w.lock ())
		{
			if (ec)
			{
				// Error sending the telemetry_req message
				nano::lock_guard<std::mutex> guard (this_l->mutex);
				this_l->channel_processed (endpoint, true);
			}
		}
	},
	nano::buffer_drop_policy::no_socket_drop);
	// clang-format on

	// If no response is seen after a certain period of time remove it
	alarm.add (std::chrono::steady_clock::now () + response_time_cutoff, [round_l, this_w, endpoint = channel->get_endpoint ()]() {
		if (auto this_l = this_w.lock ())
		{
			nano::lock_guard<std::mutex> guard (this_l->mutex);
			auto it = this_l->recent_or_initial_request_telemetry_data.find (endpoint);
			if (it != this_l->recent_or_initial_request_telemetry_data.cend () && it->undergoing_request && round_l == it->round)
			{
				this_l->channel_processed (endpoint, true);
			}
		}
	});
}

void nano::telemetry::channel_processed (nano::endpoint const & endpoint_a, bool error_a)
{
	if (recent_or_initial_request_telemetry_data.count (endpoint_a) > 0)
	{
		if (error_a)
		{
			recent_or_initial_request_telemetry_data.erase (endpoint_a);
		}
		flush_callbacks_async (endpoint_a, error_a);
	}
}

void nano::telemetry::flush_callbacks_async (nano::endpoint const & endpoint_a, bool error_a)
{
	// Post to worker so that it's truly async and not on the calling thread (same problem as std::async otherwise)
	worker.push_task ([endpoint_a, error_a, this_w = std::weak_ptr<nano::telemetry> (shared_from_this ())]() {
		if (auto this_l = this_w.lock ())
		{
			nano::unique_lock<std::mutex> lk (this_l->mutex);
			while (!this_l->callbacks[endpoint_a].empty ())
			{
				lk.unlock ();
				this_l->invoke_callbacks (endpoint_a, error_a);
				lk.lock ();
			}
		}
	});
}

void nano::telemetry::invoke_callbacks (nano::endpoint const & endpoint_a, bool error_a)
{
	std::vector<std::function<void(telemetry_data_response const &)>> callbacks_l;
	telemetry_data_response response_data{ nano::telemetry_data (), endpoint_a, error_a };
	{
		// Copy data so that it can be used outside of holding the lock
		nano::lock_guard<std::mutex> guard (mutex);

		callbacks_l = callbacks[endpoint_a];
		auto it = recent_or_initial_request_telemetry_data.find (endpoint_a);
		if (it != recent_or_initial_request_telemetry_data.end ())
		{
			response_data.telemetry_data = it->data;
		}
		callbacks.erase (endpoint_a);
	}

	// Need to account for nodes which disable telemetry data in responses
	for (auto & callback : callbacks_l)
	{
		callback (response_data);
	}
}

size_t nano::telemetry::telemetry_data_size ()
{
	nano::lock_guard<std::mutex> guard (mutex);
	return recent_or_initial_request_telemetry_data.size ();
}

nano::telemetry_info::telemetry_info (nano::endpoint const & endpoint_a, nano::telemetry_data const & data_a, std::chrono::steady_clock::time_point last_request_a, bool undergoing_request_a) :
endpoint (endpoint_a),
data (data_a),
last_request (last_request_a),
undergoing_request (undergoing_request_a)
{
}

bool nano::telemetry_info::awaiting_first_response () const
{
	return data == nano::telemetry_data ();
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (telemetry & telemetry, const std::string & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	size_t callbacks_count;
	{
		nano::lock_guard<std::mutex> guard (telemetry.mutex);
		std::unordered_map<nano::endpoint, std::vector<std::function<void(telemetry_data_response const &)>>> callbacks;
		callbacks_count = std::accumulate (callbacks.begin (), callbacks.end (), static_cast<size_t> (0), [](auto total, auto const & callback_a) {
			return total += callback_a.second.size ();
		});
	}

	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "recent_or_initial_request_telemetry_data", telemetry.telemetry_data_size (), sizeof (decltype (telemetry.recent_or_initial_request_telemetry_data)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "callbacks", callbacks_count, sizeof (decltype (telemetry.callbacks)::value_type::second_type) }));

	return composite;
}

nano::telemetry_data nano::consolidate_telemetry_data (std::vector<nano::telemetry_data> const & telemetry_datas)
{
	if (telemetry_datas.empty ())
	{
		return {};
	}
	else if (telemetry_datas.size () == 1)
	{
		// Only 1 element in the collection, so just return it.
		return telemetry_datas.front ();
	}

	std::unordered_map<uint8_t, int> protocol_versions;
	std::unordered_map<std::string, int> vendor_versions;
	std::unordered_map<uint64_t, int> bandwidth_caps;
	std::unordered_map<nano::block_hash, int> genesis_blocks;

	// Use a trimmed average which excludes the upper and lower 10% of the results
	std::multiset<uint64_t> account_counts;
	std::multiset<uint64_t> block_counts;
	std::multiset<uint64_t> cemented_counts;
	std::multiset<uint32_t> peer_counts;
	std::multiset<uint64_t> unchecked_counts;
	std::multiset<uint64_t> uptimes;
	std::multiset<uint64_t> bandwidths;
	std::multiset<uint64_t> timestamps;

	for (auto const & telemetry_data : telemetry_datas)
	{
		account_counts.insert (telemetry_data.account_count);
		block_counts.insert (telemetry_data.block_count);
		cemented_counts.insert (telemetry_data.cemented_count);

		std::ostringstream ss;
		ss << telemetry_data.major_version;
		if (telemetry_data.minor_version.is_initialized ())
		{
			ss << "." << *telemetry_data.minor_version;
			if (telemetry_data.patch_version.is_initialized ())
			{
				ss << "." << *telemetry_data.patch_version;
				if (telemetry_data.pre_release_version.is_initialized ())
				{
					ss << "." << *telemetry_data.pre_release_version;
					if (telemetry_data.maker.is_initialized ())
					{
						ss << "." << *telemetry_data.maker;
					}
				}
			}
		}

		if (telemetry_data.timestamp.is_initialized ())
		{
			timestamps.insert (std::chrono::duration_cast<std::chrono::milliseconds> (telemetry_data.timestamp->time_since_epoch ()).count ());
		}

		++vendor_versions[ss.str ()];
		++protocol_versions[telemetry_data.protocol_version];
		peer_counts.insert (telemetry_data.peer_count);
		unchecked_counts.insert (telemetry_data.unchecked_count);
		uptimes.insert (telemetry_data.uptime);
		// 0 has a special meaning (unlimited), don't include it in the average as it will be heavily skewed
		if (telemetry_data.bandwidth_cap != 0)
		{
			bandwidths.insert (telemetry_data.bandwidth_cap);
		}

		++bandwidth_caps[telemetry_data.bandwidth_cap];
		++genesis_blocks[telemetry_data.genesis_block];
	}

	// Remove 10% of the results from the lower and upper bounds to catch any outliers. Need at least 10 responses before any are removed.
	auto num_either_side_to_remove = telemetry_datas.size () / 10;

	auto strip_outliers_and_sum = [num_either_side_to_remove](auto & counts) {
		counts.erase (counts.begin (), std::next (counts.begin (), num_either_side_to_remove));
		counts.erase (std::next (counts.rbegin (), num_either_side_to_remove).base (), counts.end ());
		return std::accumulate (counts.begin (), counts.end (), nano::uint128_t (0), [](nano::uint128_t total, auto count) {
			return total += count;
		});
	};

	auto account_sum = strip_outliers_and_sum (account_counts);
	auto block_sum = strip_outliers_and_sum (block_counts);
	auto cemented_sum = strip_outliers_and_sum (cemented_counts);
	auto peer_sum = strip_outliers_and_sum (peer_counts);
	auto unchecked_sum = strip_outliers_and_sum (unchecked_counts);
	auto uptime_sum = strip_outliers_and_sum (uptimes);
	auto bandwidth_sum = strip_outliers_and_sum (bandwidths);

	nano::telemetry_data consolidated_data;
	auto size = telemetry_datas.size () - num_either_side_to_remove * 2;
	consolidated_data.account_count = boost::numeric_cast<decltype (consolidated_data.account_count)> (account_sum / size);
	consolidated_data.block_count = boost::numeric_cast<decltype (consolidated_data.block_count)> (block_sum / size);
	consolidated_data.cemented_count = boost::numeric_cast<decltype (consolidated_data.cemented_count)> (cemented_sum / size);
	consolidated_data.peer_count = boost::numeric_cast<decltype (consolidated_data.peer_count)> (peer_sum / size);
	consolidated_data.uptime = boost::numeric_cast<decltype (consolidated_data.uptime)> (uptime_sum / size);
	consolidated_data.unchecked_count = boost::numeric_cast<decltype (consolidated_data.unchecked_count)> (unchecked_sum / size);

	if (!timestamps.empty ())
	{
		auto timestamp_sum = strip_outliers_and_sum (timestamps);
		consolidated_data.timestamp = std::chrono::system_clock::time_point (std::chrono::milliseconds (boost::numeric_cast<uint64_t> (timestamp_sum / timestamps.size ())));
	}

	auto set_mode_or_average = [](auto const & collection, auto & var, auto const & sum, size_t size) {
		auto max = std::max_element (collection.begin (), collection.end (), [](auto const & lhs, auto const & rhs) {
			return lhs.second < rhs.second;
		});
		if (max->second > 1)
		{
			var = max->first;
		}
		else
		{
			var = (sum / size).template convert_to<std::remove_reference_t<decltype (var)>> ();
		}
	};

	auto set_mode = [](auto const & collection, auto & var, size_t size) {
		auto max = std::max_element (collection.begin (), collection.end (), [](auto const & lhs, auto const & rhs) {
			return lhs.second < rhs.second;
		});
		if (max->second > 1)
		{
			var = max->first;
		}
		else
		{
			// Just pick the first one
			var = collection.begin ()->first;
		}
	};

	// Use the mode of protocol version and vendor version. Also use it for bandwidth cap if there is 2 or more of the same cap.
	set_mode_or_average (bandwidth_caps, consolidated_data.bandwidth_cap, bandwidth_sum, size);
	set_mode (protocol_versions, consolidated_data.protocol_version, size);
	set_mode (genesis_blocks, consolidated_data.genesis_block, size);

	// Vendor version, needs to be parsed out of the string
	std::string version;
	set_mode (vendor_versions, version, size);

	// May only have major version, but check for optional parameters as well, only output if all are used
	std::vector<std::string> version_fragments;
	boost::split (version_fragments, version, boost::is_any_of ("."));
	debug_assert (!version_fragments.empty () && version_fragments.size () <= 5);
	consolidated_data.major_version = boost::lexical_cast<uint8_t> (version_fragments.front ());
	if (version_fragments.size () == 5)
	{
		consolidated_data.minor_version = boost::lexical_cast<uint8_t> (version_fragments[1]);
		consolidated_data.patch_version = boost::lexical_cast<uint8_t> (version_fragments[2]);
		consolidated_data.pre_release_version = boost::lexical_cast<uint8_t> (version_fragments[3]);
		consolidated_data.maker = boost::lexical_cast<uint8_t> (version_fragments[4]);
	}

	return consolidated_data;
}

nano::telemetry_data nano::local_telemetry_data (nano::ledger_cache const & ledger_cache_a, nano::network & network_a, uint64_t bandwidth_limit_a, nano::network_params const & network_params_a, std::chrono::steady_clock::time_point statup_time_a)
{
	nano::telemetry_data telemetry_data;
	telemetry_data.block_count = ledger_cache_a.block_count;
	telemetry_data.cemented_count = ledger_cache_a.cemented_count;
	telemetry_data.bandwidth_cap = bandwidth_limit_a;
	telemetry_data.protocol_version = network_params_a.protocol.protocol_version;
	telemetry_data.uptime = std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now () - statup_time_a).count ();
	telemetry_data.unchecked_count = ledger_cache_a.unchecked_count;
	telemetry_data.genesis_block = network_params_a.ledger.genesis_hash;
	telemetry_data.peer_count = nano::narrow_cast<decltype (telemetry_data.peer_count)> (network_a.size ());
	telemetry_data.account_count = ledger_cache_a.account_count;
	telemetry_data.major_version = nano::get_major_node_version ();
	telemetry_data.minor_version = nano::get_minor_node_version ();
	telemetry_data.patch_version = nano::get_patch_node_version ();
	telemetry_data.pre_release_version = nano::get_pre_release_node_version ();
	telemetry_data.maker = 0; // 0 Indicates it originated from the NF
	telemetry_data.timestamp = std::chrono::system_clock::now ();
	return telemetry_data;
}
