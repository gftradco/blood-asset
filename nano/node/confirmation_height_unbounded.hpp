#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/secure/blockstore.hpp>

#include <chrono>
#include <unordered_map>

namespace nano
{
class ledger;
class read_transaction;
class logger_mt;
class write_database_queue;

class confirmation_height_unbounded final
{
public:
	confirmation_height_unbounded (nano::ledger &, nano::write_database_queue &, std::chrono::milliseconds, nano::logger_mt &, std::atomic<bool> &, nano::block_hash const &, std::function<void(std::vector<std::shared_ptr<nano::block>> const &)> const &, std::function<void(nano::block_hash const &)> const &, std::function<uint64_t ()> const &);
	bool pending_empty () const;
	void prepare_new ();
	void process ();
	bool cement_blocks ();

private:
	class confirmed_iterated_pair
	{
	public:
		confirmed_iterated_pair (uint64_t confirmed_height_a, uint64_t iterated_height_a);
		uint64_t confirmed_height;
		uint64_t iterated_height;
	};

	class conf_height_details final
	{
	public:
		conf_height_details (nano::account const &, nano::block_hash const &, uint64_t, uint64_t, std::vector<nano::block_hash> const &);

		nano::account account;
		nano::block_hash hash;
		uint64_t height;
		uint64_t num_blocks_confirmed;
		std::vector<nano::block_hash> block_callback_data;
		std::vector<nano::block_hash> source_block_callback_data;
	};

	class receive_source_pair final
	{
	public:
		receive_source_pair (std::shared_ptr<conf_height_details> const &, const nano::block_hash &);

		std::shared_ptr<conf_height_details> receive_details;
		nano::block_hash source_hash;
	};

	std::unordered_map<account, confirmed_iterated_pair> confirmed_iterated_pairs;
	std::atomic<uint64_t> confirmed_iterated_pairs_size{ 0 };
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::block>> block_cache;
	std::atomic<uint64_t> block_cache_size{ 0 };
	std::shared_ptr<nano::block> get_block_and_sideband (nano::block_hash const &, nano::transaction const &);
	std::deque<conf_height_details> pending_writes;
	std::atomic<uint64_t> pending_writes_size{ 0 };
	std::vector<nano::block_hash> orig_block_callback_data;
	std::atomic<uint64_t> orig_block_callback_data_size{ 0 };

	std::unordered_map<nano::block_hash, std::weak_ptr<conf_height_details>> implicit_receive_cemented_mapping;
	std::atomic<uint64_t> implicit_receive_cemented_mapping_size{ 0 };
	nano::timer<std::chrono::milliseconds> timer;

	class preparation_data final
	{
	public:
		uint64_t block_height;
		uint64_t confirmation_height;
		uint64_t iterated_height;
		decltype (confirmed_iterated_pairs.begin ()) account_it;
		nano::account const & account;
		std::shared_ptr<conf_height_details> receive_details;
		bool already_traversed;
		nano::block_hash const & current;
		std::vector<nano::block_hash> const & block_callback_data;
	};

	void collect_unconfirmed_receive_and_sources_for_account (uint64_t, uint64_t, nano::block_hash const &, nano::account const &, nano::read_transaction const &, std::vector<receive_source_pair> &, std::vector<nano::block_hash> &);
	void prepare_iterated_blocks_for_cementing (preparation_data &);

	nano::ledger & ledger;
	nano::write_database_queue & write_database_queue;
	std::chrono::milliseconds batch_separate_pending_min_time;
	nano::logger_mt & logger;
	std::atomic<bool> & stopped;
	nano::block_hash const & original_hash;
	std::function<void(std::vector<std::shared_ptr<nano::block>> const &)> notify_observers_callback;
	std::function<void(nano::block_hash const &)> notify_block_already_cemented_observers_callback;
	std::function<uint64_t ()> awaiting_processing_size_callback;

	friend std::unique_ptr<nano::container_info_component> collect_container_info (confirmation_height_unbounded &, const std::string & name_a);
};

std::unique_ptr<nano::container_info_component> collect_container_info (confirmation_height_unbounded &, const std::string & name_a);
}
