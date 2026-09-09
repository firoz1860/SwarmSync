#include "swarmsync/protocol.hpp"

#include "test_support.hpp"

#include <string>
#include <sys/socket.h>

SS_TEST(chunk_request_parses_a_valid_bounded_get_command) {
  const auto request = swarmsync::parse_chunk_request(
      "GET private-token aabbccddeeff0011 7");

  SS_CHECK(request.token == "private-token");
  SS_CHECK(request.swarm_id == "aabbccddeeff0011");
  SS_CHECK(request.chunk_index == 7U);
}

SS_TEST(protocol_rejects_unsafe_chunk_headers_before_network_work) {
  SS_CHECK_THROWS(swarmsync::parse_chunk_request("GET private-token swarm -1"));
  SS_CHECK_THROWS(swarmsync::parse_chunk_request("GET private-token swarm 2 extra"));
  SS_CHECK_THROWS(swarmsync::parse_chunk_request(std::string(8193U, 'x')));
}

SS_TEST(tracker_announce_requires_sorted_unique_chunk_advertisements) {
  const auto request = swarmsync::parse_tracker_request(
      "ANNOUNCE private-token swarm-1 peer-a 45123 0,2,9");

  SS_CHECK(request.kind == swarmsync::TrackerRequestKind::announce);
  SS_CHECK(request.listen_port == 45123U);
  SS_CHECK(request.chunks.size() == 3U);
  SS_CHECK(request.chunks.at(2) == 9U);
  SS_CHECK_THROWS(swarmsync::parse_tracker_request(
      "ANNOUNCE private-token swarm-1 peer-a 45123 2,2"));
}

SS_TEST(tracker_announce_allows_a_peer_with_no_verified_chunks_yet) {
  const auto request = swarmsync::parse_tracker_request(
      "ANNOUNCE private-token swarm-1 peer-a 45123 none");

  SS_CHECK(!request.has_all_chunks);
  SS_CHECK(request.chunks.empty());
}

SS_TEST(data_headers_bound_chunk_sizes_and_require_a_sha256_digest) {
  const auto header = swarmsync::parse_data_header(
      "DATA 3 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  SS_CHECK(header.byte_count == 3U);
  SS_CHECK(header.sha256 ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  SS_CHECK_THROWS(swarmsync::parse_data_header("DATA 4194305 "
                                                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

SS_TEST(protocol_write_reports_a_closed_peer_without_terminating_the_process) {
  int descriptors[2]{};
  SS_CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
  swarmsync::Socket writer(descriptors[0]);
  swarmsync::Socket reader(descriptors[1]);
  reader.close();

  SS_CHECK_THROWS(swarmsync::write_all(writer, "closed-peer"));
}
