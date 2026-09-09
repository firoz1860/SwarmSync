#include "swarmsync/tracker.hpp"

#include "test_support.hpp"

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

SS_TEST(tracker_returns_active_authorized_peers_and_expires_stale_presence) {
  swarmsync::TrackerServer server(0U, "demo-token", 1s);
  server.start();
  swarmsync::TrackerClient client(server.endpoint(), "demo-token");

  client.announce({"swarm-a", "seed-1", 46001U, {}, true});
  const auto active = client.peers("swarm-a");
  SS_CHECK(active.size() == 1U);
  SS_CHECK(active.front().peer_id == "seed-1");
  SS_CHECK(active.front().endpoint.host == "127.0.0.1");
  SS_CHECK(active.front().has_all_chunks);

  std::this_thread::sleep_for(1100ms);
  SS_CHECK(client.peers("swarm-a").empty());
}

SS_TEST(tracker_rejects_clients_without_the_shared_swarm_token) {
  swarmsync::TrackerServer server(0U, "expected-token", 5s);
  server.start();
  swarmsync::TrackerClient unauthorized(server.endpoint(), "wrong-token");

  SS_CHECK_THROWS(unauthorized.peers("swarm-a"));
}

SS_TEST(tracker_serves_an_authorized_client_while_another_client_is_idle) {
  swarmsync::TrackerServer server(0U, "demo-token", 5s);
  server.start();

  auto idle_client = swarmsync::connect_tcp(server.endpoint(), 250ms);
  swarmsync::write_all(idle_client, "ANNOUNCE");
  std::this_thread::sleep_for(50ms);

  swarmsync::TrackerClient client(server.endpoint(), "demo-token", 500ms);
  client.announce({"swarm-a", "seed-1", 46001U, {}, true});

  const auto peers = client.peers("swarm-a");
  SS_CHECK(peers.size() == 1U);
  SS_CHECK(peers.front().peer_id == "seed-1");
}

SS_TEST(tracker_enforces_configured_metadata_and_response_limits) {
  swarmsync::TrackerLimits limits;
  limits.max_connections = 8U;
  limits.worker_count = 2U;
  limits.max_swarms = 2U;
  limits.max_peers_per_swarm = 2U;
  limits.max_response_peers = 1U;

  swarmsync::TrackerServer server(0U, "demo-token", 5s, "127.0.0.1", limits);
  server.start();
  swarmsync::TrackerClient client(server.endpoint(), "demo-token");

  client.announce({"swarm-a", "peer-1", 46001U, {}, true});
  client.announce({"swarm-a", "peer-2", 46002U, {}, true});
  SS_CHECK(client.peers("swarm-a").size() == 1U);
  SS_CHECK_THROWS(client.announce({"swarm-a", "peer-3", 46003U, {}, true}));

  client.announce({"swarm-b", "peer-1", 46004U, {}, true});
  SS_CHECK_THROWS(client.announce({"swarm-c", "peer-1", 46005U, {}, true}));
}

SS_TEST(tracker_server_can_stop_an_idle_accept_loop) {
  swarmsync::TrackerServer server(0U, "demo-token", 5s);
  server.start();
  server.stop();
}
