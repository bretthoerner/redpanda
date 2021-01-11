/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#include "coproc/tests/utils/router_test_fixture.h"

#include "coproc/tests/utils/utils.h"
#include "model/metadata.h"
#include "test_utils/async.h"

ss::future<> router_test_fixture::startup(log_layout_map&& llm) {
    // Data on all shards is identical
    return coproc_test_fixture::startup(std::move(llm)).then([this] {
        // assemble the active_copros from the '_coprocessors' map
        return ss::do_with(
          enable_reqs_data(), [this](enable_reqs_data& layout) {
              return enable_coprocessors(layout);
          });
    });
}

void router_test_fixture::validate_result(
  const enable_reqs_data& layout,
  result<rpc::client_context<coproc::enable_copros_reply>> r) {
    vassert(!r.has_failure(), "reply failed: {}", r.error());
    const auto& reply = r.value().data;
    vassert(
      reply.acks.size() == layout.size(),
      "Unequal sizes, reply {}, layout {}",
      reply.acks.size(),
      layout.size());
    for (const auto& ack : reply.acks) {
        const auto& [sid, topic_acks] = ack;
        const auto found = std::find_if(
          layout.cbegin(), layout.cend(), [sid = sid](const auto& data_e) {
              return data_e.id == sid;
          });
        vassert(found != layout.end(), "Missing script id: {}", sid);
        /// Could assert if all topics were inserted without error but maybe
        /// there is a test expecting this, so better to not
    }
}

ss::future<>
router_test_fixture::enable_coprocessors(enable_reqs_data& layout) {
    /// TODO(Rob) just call to .local() is enough
    return all_coprocessors()
      .invoke_on(
        ss::this_shard_id(),
        [this, &layout](const copro_map& coprocessors) {
            to_ecr_data(layout, coprocessors);
        })
      .then([this, &layout] {
          auto layout_cp = layout;
          return register_coprocessors(sm_client(), std::move(layout))
            .then([this, layout = std::move(layout_cp)](auto r) {
                validate_result(layout, std::move(r));
            });
      });
}

void router_test_fixture::to_ecr_data(
  enable_reqs_data& layout, const copro_map& coprocessors) {
    std::transform(
      coprocessors.cbegin(),
      coprocessors.cend(),
      std::inserter(layout, layout.end()),
      [](const auto& p) {
          return coproc::enable_copros_request::data{
            .id = p.first, .topics = p.second->get_input_topics()};
      });
}
