#include <nano/core_test/testutil.hpp>
#include <nano/node/testing.hpp>
#include <nano/node/transport/udp.hpp>

#include <gtest/gtest.h>

#include <boost/iostreams/stream_buffer.hpp>
#include <boost/thread.hpp>

using namespace std::chrono_literals;

TEST (network, tcp_connection)
{
	boost::asio::io_context io_ctx;
	boost::asio::ip::tcp::acceptor acceptor (io_ctx);
	auto port = nano::get_available_port ();
	boost::asio::ip::tcp::endpoint endpoint (boost::asio::ip::address_v4::any (), port);
	acceptor.open (endpoint.protocol ());
	acceptor.set_option (boost::asio::ip::tcp::acceptor::reuse_address (true));
	acceptor.bind (endpoint);
	acceptor.listen ();
	boost::asio::ip::tcp::socket incoming (io_ctx);
	std::atomic<bool> done1 (false);
	std::string message1;
	acceptor.async_accept (incoming,
	[&done1, &message1](boost::system::error_code const & ec_a) {
		   if (ec_a)
		   {
			   message1 = ec_a.message ();
			   std::cerr << message1;
		   }
		   done1 = true; });
	boost::asio::ip::tcp::socket connector (io_ctx);
	std::atomic<bool> done2 (false);
	std::string message2;
	connector.async_connect (boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v4::loopback (), port),
	[&done2, &message2](boost::system::error_code const & ec_a) {
		if (ec_a)
		{
			message2 = ec_a.message ();
			std::cerr << message2;
		}
		done2 = true;
	});
	while (!done1 || !done2)
	{
		io_ctx.poll ();
	}
	ASSERT_EQ (0, message1.size ());
	ASSERT_EQ (0, message2.size ());
}

TEST (network, construction)
{
	auto port = nano::get_available_port ();
	nano::system system;
	system.add_node (nano::node_config (port, system.logging));
	ASSERT_EQ (1, system.nodes.size ());
	ASSERT_EQ (port, system.nodes[0]->network.endpoint ().port ());
}

TEST (network, self_discard)
{
	nano::node_flags node_flags;
	node_flags.disable_udp = false;
	nano::system system (1, nano::transport::transport_type::tcp, node_flags);
	nano::message_buffer data;
	data.endpoint = system.nodes[0]->network.endpoint ();
	ASSERT_EQ (0, system.nodes[0]->stats.count (nano::stat::type::error, nano::stat::detail::bad_sender));
	system.nodes[0]->network.udp_channels.receive_action (&data);
	ASSERT_EQ (1, system.nodes[0]->stats.count (nano::stat::type::error, nano::stat::detail::bad_sender));
}

TEST (network, send_node_id_handshake)
{
	nano::node_flags node_flags;
	node_flags.disable_udp = false;
	nano::system system;
	auto node0 = system.add_node (node_flags);
	ASSERT_EQ (0, node0->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, nano::get_available_port (), nano::unique_path (), system.alarm, system.logging, system.work, node_flags));
	node1->start ();
	system.nodes.push_back (node1);
	auto initial (node0->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in));
	auto initial_node1 (node1->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in));
	auto channel (std::make_shared<nano::transport::channel_udp> (node0->network.udp_channels, node1->network.endpoint (), node1->network_params.protocol.protocol_version));
	node0->network.send_keepalive (channel);
	ASSERT_EQ (0, node0->network.size ());
	ASSERT_EQ (0, node1->network.size ());
	system.deadline_set (10s);
	while (node1->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in) == initial_node1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (10s);
	while (node0->network.size () != 0 && node1->network.size () != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (10s);
	while (node0->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in) < initial + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (10s);
	while (node0->network.size () != 1 && node1->network.size () != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	auto list1 (node0->network.list (1));
	ASSERT_EQ (node1->network.endpoint (), list1[0]->get_endpoint ());
	auto list2 (node1->network.list (1));
	ASSERT_EQ (node0->network.endpoint (), list2[0]->get_endpoint ());
	node1->stop ();
}

TEST (network, send_node_id_handshake_tcp)
{
	nano::system system (1);
	auto node0 (system.nodes[0]);
	ASSERT_EQ (0, node0->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, nano::get_available_port (), nano::unique_path (), system.alarm, system.logging, system.work));
	node1->start ();
	system.nodes.push_back (node1);
	auto initial (node0->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in));
	auto initial_node1 (node1->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in));
	auto initial_keepalive (node0->stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in));
	std::weak_ptr<nano::node> node_w (node0);
	node0->network.tcp_channels.start_tcp (node1->network.endpoint (), [node_w](std::shared_ptr<nano::transport::channel> channel_a) {
		if (auto node_l = node_w.lock ())
		{
			node_l->network.send_keepalive (channel_a);
		}
	});
	ASSERT_EQ (0, node0->network.size ());
	ASSERT_EQ (0, node1->network.size ());
	system.deadline_set (10s);
	while (node0->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in) < initial + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (node1->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in) < initial_node1 + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (node0->stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in) < initial_keepalive + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (node1->stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in) < initial_keepalive + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node0->network.size ());
	ASSERT_EQ (1, node1->network.size ());
	auto list1 (node0->network.list (1));
	ASSERT_EQ (nano::transport::transport_type::tcp, list1[0]->get_type ());
	ASSERT_EQ (node1->network.endpoint (), list1[0]->get_endpoint ());
	auto list2 (node1->network.list (1));
	ASSERT_EQ (nano::transport::transport_type::tcp, list2[0]->get_type ());
	ASSERT_EQ (node0->network.endpoint (), list2[0]->get_endpoint ());
	node1->stop ();
}

TEST (network, last_contacted)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_udp = false;
	auto node0 = system.add_node (node_flags);
	ASSERT_EQ (0, node0->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, nano::get_available_port (), nano::unique_path (), system.alarm, system.logging, system.work, node_flags));
	node1->start ();
	system.nodes.push_back (node1);
	auto channel1 (std::make_shared<nano::transport::channel_udp> (node1->network.udp_channels, nano::endpoint (boost::asio::ip::address_v6::loopback (), system.nodes.front ()->network.endpoint ().port ()), node1->network_params.protocol.protocol_version));
	node1->network.send_keepalive (channel1);
	system.deadline_set (10s);

	// Wait until the handshake is complete
	while (node0->network.size () < 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (node0->network.size (), 1);

	auto channel2 (node0->network.udp_channels.channel (nano::endpoint (boost::asio::ip::address_v6::loopback (), node1->network.endpoint ().port ())));
	ASSERT_NE (nullptr, channel2);
	// Make sure last_contact gets updated on receiving a non-handshake message
	auto timestamp_before_keepalive = channel2->get_last_packet_received ();
	node1->network.send_keepalive (channel1);
	while (node0->stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in) < 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (node0->network.size (), 1);
	auto timestamp_after_keepalive = channel2->get_last_packet_received ();
	ASSERT_GT (timestamp_after_keepalive, timestamp_before_keepalive);

	node1->stop ();
}

TEST (network, multi_keepalive)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_udp = false;
	auto node0 = system.add_node (node_flags);
	ASSERT_EQ (0, node0->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, nano::get_available_port (), nano::unique_path (), system.alarm, system.logging, system.work, node_flags));
	ASSERT_FALSE (node1->init_error ());
	node1->start ();
	system.nodes.push_back (node1);
	ASSERT_EQ (0, node1->network.size ());
	auto channel1 (std::make_shared<nano::transport::channel_udp> (node1->network.udp_channels, node0->network.endpoint (), node1->network_params.protocol.protocol_version));
	node1->network.send_keepalive (channel1);
	ASSERT_EQ (0, node1->network.size ());
	ASSERT_EQ (0, node0->network.size ());
	system.deadline_set (10s);
	while (node0->network.size () != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	auto node2 = system.add_node (node_flags);
	ASSERT_FALSE (node2->init_error ());
	auto channel2 (std::make_shared<nano::transport::channel_udp> (node2->network.udp_channels, node0->network.endpoint (), node2->network_params.protocol.protocol_version));
	node2->network.send_keepalive (channel2);
	system.deadline_set (10s);
	while (node1->network.size () != 2 || node0->network.size () != 2 || node2->network.size () != 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
	node2->stop ();
}

TEST (network, send_discarded_publish)
{
	nano::system system (2);
	auto & node1 (*system.nodes[0]);
	auto & node2 (*system.nodes[1]);
	auto block (std::make_shared<nano::send_block> (1, 1, 2, nano::keypair ().prv, 4, *system.work.generate (nano::root (1))));
	nano::genesis genesis;
	{
		auto transaction (node1.store.tx_begin_read ());
		node1.network.flood_block (block);
		ASSERT_EQ (genesis.hash (), node1.ledger.latest (transaction, nano::test_genesis_key.pub));
		ASSERT_EQ (genesis.hash (), node2.latest (nano::test_genesis_key.pub));
	}
	system.deadline_set (10s);
	while (node2.stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	auto transaction (node1.store.tx_begin_read ());
	ASSERT_EQ (genesis.hash (), node1.ledger.latest (transaction, nano::test_genesis_key.pub));
	ASSERT_EQ (genesis.hash (), node2.latest (nano::test_genesis_key.pub));
}

TEST (network, send_invalid_publish)
{
	nano::system system (2);
	auto & node1 (*system.nodes[0]);
	auto & node2 (*system.nodes[1]);
	nano::genesis genesis;
	auto block (std::make_shared<nano::send_block> (1, 1, 20, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (nano::root (1))));
	{
		auto transaction (node1.store.tx_begin_read ());
		node1.network.flood_block (block);
		ASSERT_EQ (genesis.hash (), node1.ledger.latest (transaction, nano::test_genesis_key.pub));
		ASSERT_EQ (genesis.hash (), node2.latest (nano::test_genesis_key.pub));
	}
	system.deadline_set (10s);
	while (node2.stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	auto transaction (node1.store.tx_begin_read ());
	ASSERT_EQ (genesis.hash (), node1.ledger.latest (transaction, nano::test_genesis_key.pub));
	ASSERT_EQ (genesis.hash (), node2.latest (nano::test_genesis_key.pub));
}

TEST (network, send_valid_confirm_ack)
{
	std::vector<nano::transport::transport_type> types{ nano::transport::transport_type::tcp, nano::transport::transport_type::udp };
	for (auto & type : types)
	{
		nano::node_flags node_flags;
		if (type == nano::transport::transport_type::udp)
		{
			node_flags.disable_tcp_realtime = true;
			node_flags.disable_bootstrap_listener = true;
			node_flags.disable_udp = false;
		}
		nano::system system (2, type, node_flags);
		auto & node1 (*system.nodes[0]);
		auto & node2 (*system.nodes[1]);
		nano::keypair key2;
		system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
		system.wallet (1)->insert_adhoc (key2.prv);
		nano::block_hash latest1 (node1.latest (nano::test_genesis_key.pub));
		nano::send_block block2 (latest1, key2.pub, 50, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest1));
		nano::block_hash latest2 (node2.latest (nano::test_genesis_key.pub));
		node1.process_active (std::make_shared<nano::send_block> (block2));
		system.deadline_set (10s);
		// Keep polling until latest block changes
		while (node2.latest (nano::test_genesis_key.pub) == latest2)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
		// Make sure the balance has decreased after processing the block.
		ASSERT_EQ (50, node2.balance (nano::test_genesis_key.pub));
	}
}

TEST (network, send_valid_publish)
{
	std::vector<nano::transport::transport_type> types{ nano::transport::transport_type::tcp, nano::transport::transport_type::udp };
	for (auto & type : types)
	{
		nano::node_flags node_flags;
		if (type == nano::transport::transport_type::udp)
		{
			node_flags.disable_tcp_realtime = true;
			node_flags.disable_bootstrap_listener = true;
			node_flags.disable_udp = false;
		}
		nano::system system (2, type, node_flags);
		auto & node1 (*system.nodes[0]);
		auto & node2 (*system.nodes[1]);
		node1.bootstrap_initiator.stop ();
		node2.bootstrap_initiator.stop ();
		system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
		nano::keypair key2;
		system.wallet (1)->insert_adhoc (key2.prv);
		nano::block_hash latest1 (node1.latest (nano::test_genesis_key.pub));
		nano::send_block block2 (latest1, key2.pub, 50, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest1));
		auto hash2 (block2.hash ());
		nano::block_hash latest2 (node2.latest (nano::test_genesis_key.pub));
		node2.process_active (std::make_shared<nano::send_block> (block2));
		system.deadline_set (10s);
		while (node1.stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) == 0)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
		ASSERT_NE (hash2, latest2);
		system.deadline_set (10s);
		while (node2.latest (nano::test_genesis_key.pub) == latest2)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
		ASSERT_EQ (50, node2.balance (nano::test_genesis_key.pub));
	}
}

TEST (network, send_insufficient_work)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_udp = false;
	auto & node1 = *system.add_node (node_flags);
	auto & node2 = *system.add_node (node_flags);
	auto block (std::make_shared<nano::send_block> (0, 1, 20, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	nano::publish publish (block);
	nano::transport::channel_udp channel (node1.network.udp_channels, node2.network.endpoint (), node1.network_params.protocol.protocol_version);
	channel.send (publish, [](boost::system::error_code const & ec, size_t size) {});
	ASSERT_EQ (0, node1.stats.count (nano::stat::type::error, nano::stat::detail::insufficient_work));
	system.deadline_set (10s);
	while (node2.stats.count (nano::stat::type::error, nano::stat::detail::insufficient_work) == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node2.stats.count (nano::stat::type::error, nano::stat::detail::insufficient_work));
}

TEST (receivable_processor, confirm_insufficient_pos)
{
	nano::system system (1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	auto block1 (std::make_shared<nano::send_block> (genesis.hash (), 0, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*block1);
	ASSERT_EQ (nano::process_result::progress, node1.process (*block1).code);
	node1.active.insert (block1);
	nano::keypair key1;
	auto vote (std::make_shared<nano::vote> (key1.pub, key1.prv, 0, block1));
	nano::confirm_ack con1 (vote);
	node1.network.process_message (con1, node1.network.udp_channels.create (node1.network.endpoint ()));
}

TEST (receivable_processor, confirm_sufficient_pos)
{
	nano::system system (1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	auto block1 (std::make_shared<nano::send_block> (genesis.hash (), 0, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*block1);
	ASSERT_EQ (nano::process_result::progress, node1.process (*block1).code);
	node1.active.insert (block1);
	auto vote (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, block1));
	nano::confirm_ack con1 (vote);
	node1.network.process_message (con1, node1.network.udp_channels.create (node1.network.endpoint ()));
}

TEST (receivable_processor, send_with_receive)
{
	std::vector<nano::transport::transport_type> types{ nano::transport::transport_type::tcp, nano::transport::transport_type::udp };
	for (auto & type : types)
	{
		nano::node_flags node_flags;
		if (type == nano::transport::transport_type::udp)
		{
			node_flags.disable_tcp_realtime = true;
			node_flags.disable_bootstrap_listener = true;
			node_flags.disable_udp = false;
		}
		nano::system system (2, type, node_flags);
		auto & node1 (*system.nodes[0]);
		auto & node2 (*system.nodes[1]);
		auto amount (std::numeric_limits<nano::uint128_t>::max ());
		nano::keypair key2;
		system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
		nano::block_hash latest1 (node1.latest (nano::test_genesis_key.pub));
		system.wallet (1)->insert_adhoc (key2.prv);
		auto block1 (std::make_shared<nano::send_block> (latest1, key2.pub, amount - node1.config.receive_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (latest1)));
		ASSERT_EQ (amount, node1.balance (nano::test_genesis_key.pub));
		ASSERT_EQ (0, node1.balance (key2.pub));
		ASSERT_EQ (amount, node2.balance (nano::test_genesis_key.pub));
		ASSERT_EQ (0, node2.balance (key2.pub));
		node1.process_active (block1);
		node1.block_processor.flush ();
		node2.process_active (block1);
		node2.block_processor.flush ();
		ASSERT_EQ (amount - node1.config.receive_minimum.number (), node1.balance (nano::test_genesis_key.pub));
		ASSERT_EQ (0, node1.balance (key2.pub));
		ASSERT_EQ (amount - node1.config.receive_minimum.number (), node2.balance (nano::test_genesis_key.pub));
		ASSERT_EQ (0, node2.balance (key2.pub));
		system.deadline_set (10s);
		while (node1.balance (key2.pub) != node1.config.receive_minimum.number () || node2.balance (key2.pub) != node1.config.receive_minimum.number ())
		{
			ASSERT_NO_ERROR (system.poll ());
		}
		ASSERT_EQ (amount - node1.config.receive_minimum.number (), node1.balance (nano::test_genesis_key.pub));
		ASSERT_EQ (node1.config.receive_minimum.number (), node1.balance (key2.pub));
		ASSERT_EQ (amount - node1.config.receive_minimum.number (), node2.balance (nano::test_genesis_key.pub));
		ASSERT_EQ (node1.config.receive_minimum.number (), node2.balance (key2.pub));
	}
}

TEST (network, receive_weight_change)
{
	nano::system system (2);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::keypair key2;
	system.wallet (1)->insert_adhoc (key2.prv);
	{
		auto transaction (system.nodes[1]->wallets.tx_begin_write ());
		system.wallet (1)->store.representative_set (transaction, key2.pub);
	}
	ASSERT_NE (nullptr, system.wallet (0)->send_action (nano::test_genesis_key.pub, key2.pub, system.nodes[0]->config.receive_minimum.number ()));
	system.deadline_set (10s);
	while (std::any_of (system.nodes.begin (), system.nodes.end (), [&](std::shared_ptr<nano::node> const & node_a) { return node_a->weight (key2.pub) != system.nodes[0]->config.receive_minimum.number (); }))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (parse_endpoint, valid)
{
	std::string string ("::1:24000");
	nano::endpoint endpoint;
	ASSERT_FALSE (nano::parse_endpoint (string, endpoint));
	ASSERT_EQ (boost::asio::ip::address_v6::loopback (), endpoint.address ());
	ASSERT_EQ (24000, endpoint.port ());
}

TEST (parse_endpoint, invalid_port)
{
	std::string string ("::1:24a00");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, invalid_address)
{
	std::string string ("::q:24000");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, no_address)
{
	std::string string (":24000");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, no_port)
{
	std::string string ("::1:");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, no_colon)
{
	std::string string ("::1");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

TEST (network, ipv6)
{
	boost::asio::ip::address_v6 address (boost::asio::ip::make_address_v6 ("::ffff:127.0.0.1"));
	ASSERT_TRUE (address.is_v4_mapped ());
	nano::endpoint endpoint1 (address, 16384);
	std::vector<uint8_t> bytes1;
	{
		nano::vectorstream stream (bytes1);
		nano::write (stream, address.to_bytes ());
	}
	ASSERT_EQ (16, bytes1.size ());
	for (auto i (bytes1.begin ()), n (bytes1.begin () + 10); i != n; ++i)
	{
		ASSERT_EQ (0, *i);
	}
	ASSERT_EQ (0xff, bytes1[10]);
	ASSERT_EQ (0xff, bytes1[11]);
	std::array<uint8_t, 16> bytes2;
	nano::bufferstream stream (bytes1.data (), bytes1.size ());
	auto error (nano::try_read (stream, bytes2));
	ASSERT_FALSE (error);
	nano::endpoint endpoint2 (boost::asio::ip::address_v6 (bytes2), 16384);
	ASSERT_EQ (endpoint1, endpoint2);
}

TEST (network, ipv6_from_ipv4)
{
	nano::endpoint endpoint1 (boost::asio::ip::address_v4::loopback (), 16000);
	ASSERT_TRUE (endpoint1.address ().is_v4 ());
	nano::endpoint endpoint2 (boost::asio::ip::address_v6::v4_mapped (endpoint1.address ().to_v4 ()), 16000);
	ASSERT_TRUE (endpoint2.address ().is_v6 ());
}

TEST (network, ipv6_bind_send_ipv4)
{
	boost::asio::io_context io_ctx;
	auto port1 = nano::get_available_port ();
	auto port2 = nano::get_available_port ();
	nano::endpoint endpoint1 (boost::asio::ip::address_v6::any (), port1);
	nano::endpoint endpoint2 (boost::asio::ip::address_v4::any (), port2);
	std::array<uint8_t, 16> bytes1;
	auto finish1 (false);
	nano::endpoint endpoint3;
	boost::asio::ip::udp::socket socket1 (io_ctx, endpoint1);
	socket1.async_receive_from (boost::asio::buffer (bytes1.data (), bytes1.size ()), endpoint3, [&finish1](boost::system::error_code const & error, size_t size_a) {
		ASSERT_FALSE (error);
		ASSERT_EQ (16, size_a);
		finish1 = true;
	});
	boost::asio::ip::udp::socket socket2 (io_ctx, endpoint2);
	nano::endpoint endpoint5 (boost::asio::ip::address_v4::loopback (), port1);
	nano::endpoint endpoint6 (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4::loopback ()), port2);
	socket2.async_send_to (boost::asio::buffer (std::array<uint8_t, 16>{}, 16), endpoint5, [](boost::system::error_code const & error, size_t size_a) {
		ASSERT_FALSE (error);
		ASSERT_EQ (16, size_a);
	});
	auto iterations (0);
	while (!finish1)
	{
		io_ctx.poll ();
		++iterations;
		ASSERT_LT (iterations, 200);
	}
	ASSERT_EQ (endpoint6, endpoint3);
	std::array<uint8_t, 16> bytes2;
	nano::endpoint endpoint4;
	socket2.async_receive_from (boost::asio::buffer (bytes2.data (), bytes2.size ()), endpoint4, [](boost::system::error_code const & error, size_t size_a) {
		ASSERT_FALSE (!error);
		ASSERT_EQ (16, size_a);
	});
	socket1.async_send_to (boost::asio::buffer (std::array<uint8_t, 16>{}, 16), endpoint6, [](boost::system::error_code const & error, size_t size_a) {
		ASSERT_FALSE (error);
		ASSERT_EQ (16, size_a);
	});
}

TEST (network, endpoint_bad_fd)
{
	nano::system system (1);
	system.nodes[0]->stop ();
	auto endpoint (system.nodes[0]->network.endpoint ());
	ASSERT_TRUE (endpoint.address ().is_loopback ());
	// The endpoint is invalidated asynchronously
	system.deadline_set (10s);
	while (system.nodes[0]->network.endpoint ().port () != 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (network, reserved_address)
{
	nano::system system (1);
	// 0 port test
	ASSERT_TRUE (nano::transport::reserved_address (nano::endpoint (boost::asio::ip::make_address_v6 ("2001::"), 0)));
	// Valid address test
	ASSERT_FALSE (nano::transport::reserved_address (nano::endpoint (boost::asio::ip::make_address_v6 ("2001::"), 1)));
	nano::endpoint loopback (boost::asio::ip::make_address_v6 ("::1"), 1);
	ASSERT_FALSE (nano::transport::reserved_address (loopback));
	nano::endpoint private_network_peer (boost::asio::ip::make_address_v6 ("::ffff:10.0.0.0"), 1);
	ASSERT_TRUE (nano::transport::reserved_address (private_network_peer, false));
	ASSERT_FALSE (nano::transport::reserved_address (private_network_peer, true));
}

TEST (node, port_mapping)
{
	nano::system system (1);
	auto node0 (system.nodes[0]);
	node0->port_mapping.refresh_devices ();
	node0->port_mapping.start ();
	auto end (std::chrono::steady_clock::now () + std::chrono::seconds (500));
	(void)end;
	//while (std::chrono::steady_clock::now () < end)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (message_buffer_manager, one_buffer)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 1);
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	buffer.enqueue (buffer1);
	auto buffer2 (buffer.dequeue ());
	ASSERT_EQ (buffer1, buffer2);
	buffer.release (buffer2);
	auto buffer3 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer3);
}

TEST (message_buffer_manager, two_buffers)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 2);
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	auto buffer2 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer2);
	ASSERT_NE (buffer1, buffer2);
	buffer.enqueue (buffer2);
	buffer.enqueue (buffer1);
	auto buffer3 (buffer.dequeue ());
	ASSERT_EQ (buffer2, buffer3);
	auto buffer4 (buffer.dequeue ());
	ASSERT_EQ (buffer1, buffer4);
	buffer.release (buffer3);
	buffer.release (buffer4);
	auto buffer5 (buffer.allocate ());
	ASSERT_EQ (buffer2, buffer5);
	auto buffer6 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer6);
}

TEST (message_buffer_manager, one_overflow)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 1);
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	buffer.enqueue (buffer1);
	auto buffer2 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer2);
}

TEST (message_buffer_manager, two_overflow)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 2);
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	buffer.enqueue (buffer1);
	auto buffer2 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer2);
	ASSERT_NE (buffer1, buffer2);
	buffer.enqueue (buffer2);
	auto buffer3 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer3);
	auto buffer4 (buffer.allocate ());
	ASSERT_EQ (buffer2, buffer4);
}

TEST (message_buffer_manager, one_buffer_multithreaded)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 1);
	boost::thread thread ([&buffer]() {
		auto done (false);
		while (!done)
		{
			auto item (buffer.dequeue ());
			done = item == nullptr;
			if (item != nullptr)
			{
				buffer.release (item);
			}
		}
	});
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	buffer.enqueue (buffer1);
	auto buffer2 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer2);
	buffer.stop ();
	thread.join ();
}

TEST (message_buffer_manager, many_buffers_multithreaded)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 16);
	std::vector<boost::thread> threads;
	for (auto i (0); i < 4; ++i)
	{
		threads.push_back (boost::thread ([&buffer]() {
			auto done (false);
			while (!done)
			{
				auto item (buffer.dequeue ());
				done = item == nullptr;
				if (item != nullptr)
				{
					buffer.release (item);
				}
			}
		}));
	}
	std::atomic_int count (0);
	for (auto i (0); i < 4; ++i)
	{
		threads.push_back (boost::thread ([&buffer, &count]() {
			auto done (false);
			for (auto i (0); !done && i < 1000; ++i)
			{
				auto item (buffer.allocate ());
				done = item == nullptr;
				if (item != nullptr)
				{
					buffer.enqueue (item);
					++count;
					if (count > 3000)
					{
						buffer.stop ();
					}
				}
			}
		}));
	}
	buffer.stop ();
	for (auto & i : threads)
	{
		i.join ();
	}
}

TEST (message_buffer_manager, stats)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 1);
	auto buffer1 (buffer.allocate ());
	buffer.enqueue (buffer1);
	buffer.allocate ();
	ASSERT_EQ (1, stats.count (nano::stat::type::udp, nano::stat::detail::overflow));
}

TEST (tcp_listener, tcp_node_id_handshake)
{
	nano::system system (1);
	auto socket (std::make_shared<nano::socket> (system.nodes[0]));
	auto bootstrap_endpoint (system.nodes[0]->bootstrap.endpoint ());
	auto cookie (system.nodes[0]->network.syn_cookies.assign (nano::transport::map_tcp_to_endpoint (bootstrap_endpoint)));
	nano::node_id_handshake node_id_handshake (cookie, boost::none);
	auto input (node_id_handshake.to_shared_const_buffer ());
	std::atomic<bool> write_done (false);
	socket->async_connect (bootstrap_endpoint, [&input, socket, &write_done](boost::system::error_code const & ec) {
		ASSERT_FALSE (ec);
		socket->async_write (input, [&input, &write_done](boost::system::error_code const & ec, size_t size_a) {
			ASSERT_FALSE (ec);
			ASSERT_EQ (input.size (), size_a);
			write_done = true;
		});
	});

	system.deadline_set (std::chrono::seconds (5));
	while (!write_done)
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	boost::optional<std::pair<nano::account, nano::signature>> response_zero (std::make_pair (nano::account (0), nano::signature (0)));
	nano::node_id_handshake node_id_handshake_response (boost::none, response_zero);
	auto output (node_id_handshake_response.to_bytes ());
	std::atomic<bool> done (false);
	socket->async_read (output, output->size (), [&output, &done](boost::system::error_code const & ec, size_t size_a) {
		ASSERT_FALSE (ec);
		ASSERT_EQ (output->size (), size_a);
		done = true;
	});
	system.deadline_set (std::chrono::seconds (5));
	while (!done)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (tcp_listener, tcp_listener_timeout_empty)
{
	nano::system system (1);
	auto node0 (system.nodes[0]);
	auto socket (std::make_shared<nano::socket> (node0));
	std::atomic<bool> connected (false);
	socket->async_connect (node0->bootstrap.endpoint (), [&connected](boost::system::error_code const & ec) {
		ASSERT_FALSE (ec);
		connected = true;
	});
	system.deadline_set (std::chrono::seconds (5));
	while (!connected)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	bool disconnected (false);
	system.deadline_set (std::chrono::seconds (6));
	while (!disconnected)
	{
		{
			nano::lock_guard<std::mutex> guard (node0->bootstrap.mutex);
			disconnected = node0->bootstrap.connections.empty ();
		}
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (tcp_listener, tcp_listener_timeout_node_id_handshake)
{
	nano::system system (1);
	auto node0 (system.nodes[0]);
	auto socket (std::make_shared<nano::socket> (node0));
	auto cookie (node0->network.syn_cookies.assign (nano::transport::map_tcp_to_endpoint (node0->bootstrap.endpoint ())));
	nano::node_id_handshake node_id_handshake (cookie, boost::none);
	auto input (node_id_handshake.to_shared_const_buffer ());
	socket->async_connect (node0->bootstrap.endpoint (), [&input, socket](boost::system::error_code const & ec) {
		ASSERT_FALSE (ec);
		socket->async_write (input, [&input](boost::system::error_code const & ec, size_t size_a) {
			ASSERT_FALSE (ec);
			ASSERT_EQ (input.size (), size_a);
		});
	});
	system.deadline_set (std::chrono::seconds (5));
	while (node0->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake) == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	{
		nano::lock_guard<std::mutex> guard (node0->bootstrap.mutex);
		ASSERT_EQ (node0->bootstrap.connections.size (), 1);
	}
	bool disconnected (false);
	system.deadline_set (std::chrono::seconds (20));
	while (!disconnected)
	{
		{
			nano::lock_guard<std::mutex> guard (node0->bootstrap.mutex);
			disconnected = node0->bootstrap.connections.empty ();
		}
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (network, replace_port)
{
	nano::system system;
	nano::node_flags node_flags;
	node_flags.disable_udp = false;
	node_flags.disable_ongoing_telemetry_requests = true;
	auto node0 = system.add_node (node_flags);
	ASSERT_EQ (0, node0->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, nano::get_available_port (), nano::unique_path (), system.alarm, system.logging, system.work, node_flags));
	node1->start ();
	system.nodes.push_back (node1);
	{
		auto channel (node0->network.udp_channels.insert (nano::endpoint (node1->network.endpoint ().address (), 23000), node1->network_params.protocol.protocol_version));
		if (channel)
		{
			channel->set_node_id (node1->node_id.pub);
		}
	}
	auto peers_list (node0->network.list (std::numeric_limits<size_t>::max ()));
	ASSERT_EQ (peers_list[0]->get_node_id (), node1->node_id.pub);
	auto channel (std::make_shared<nano::transport::channel_udp> (node0->network.udp_channels, node1->network.endpoint (), node1->network_params.protocol.protocol_version));
	node0->network.send_keepalive (channel);
	system.deadline_set (5s);
	while (!node0->network.udp_channels.channel (node1->network.endpoint ()))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (node0->network.udp_channels.size () > 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (node0->network.udp_channels.size (), 1);
	auto list1 (node0->network.list (1));
	ASSERT_EQ (node1->network.endpoint (), list1[0]->get_endpoint ());
	auto list2 (node1->network.list (1));
	ASSERT_EQ (node0->network.endpoint (), list2[0]->get_endpoint ());
	// Remove correct peer (same node ID)
	node0->network.udp_channels.clean_node_id (nano::endpoint (node1->network.endpoint ().address (), 23000), node1->node_id.pub);
	system.deadline_set (5s);
	while (node0->network.udp_channels.size () > 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
}

// The test must be completed in less than 1 second
TEST (bandwidth_limiter, validate)
{
	nano::system system;
	size_t const message_size (1024);
	nano::bandwidth_limiter limiter_0 (0);
	auto message_limit = 3;
	nano::bandwidth_limiter limiter_3 (message_size * message_limit);
	ASSERT_FALSE (limiter_0.should_drop (message_size)); // never drops
	auto start (std::chrono::steady_clock::now ());
	for (unsigned i = 0; i < message_limit; ++i)
	{
		limiter_3.add (message_size);
		ASSERT_FALSE (limiter_3.should_drop (message_size));
	}
	system.deadline_set (300ms);
	// Wait for the trended rate to catch up
	while (limiter_3.get_rate () < limiter_3.get_limit ())
	{
		// Force an update
		limiter_3.add (0);
		ASSERT_NO_ERROR (system.poll (10ms));
	}
	ASSERT_EQ (limiter_3.get_rate (), limiter_3.get_limit ());
	ASSERT_LT (std::chrono::steady_clock::now () - 1s, start);
	// A new message would drop
	ASSERT_TRUE (limiter_3.should_drop (message_size));
	// So adding it will not increase the rate
	limiter_3.add (message_size);
	ASSERT_EQ (limiter_3.get_rate (), limiter_3.get_limit ());
	// Unless the message is forced (e.g. non-droppable packets)
	limiter_3.add (message_size, true);
	// Limiter says it should drop, but the rate will have increased
	// Wait for the trended rate to catch up
	while (limiter_3.get_rate () < limiter_3.get_limit () + message_size)
	{
		// Force an update
		limiter_3.add (0);
		ASSERT_NO_ERROR (system.poll (10ms));
	}
	ASSERT_TRUE (limiter_3.should_drop (message_size));
	ASSERT_EQ (limiter_3.get_rate (), limiter_3.get_limit () + message_size);
	ASSERT_LT (std::chrono::steady_clock::now () - 1s, start);
}
