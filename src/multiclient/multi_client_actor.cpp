#include "multi_client_actor.h"
#include <cstdint>
#include <random>
#include <ranges>
#include <string>
#include "request.h"
#include "td/actor/actor.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/check.h"
#include "td/utils/filesystem.h"

namespace multiclient {

namespace {

std::vector<std::string> split_global_config_by_liteservers(std::string global_config) {
  auto config_json = td::json_decode(global_config).move_as_ok();
  auto liteservers =
      get_json_object_field(config_json.get_object(), "liteservers", td::JsonValue::Type::Array, false).move_as_ok();
  const auto& ls_array = liteservers.get_array();

  std::vector<std::string> result;
  result.reserve(ls_array.size());

  for (const auto& ls_json : ls_array) {
    std::string result_ls_array;
    {
      td::JsonBuilder builder;
      auto arr = builder.enter_array();
      arr << ls_json;
      arr.leave();
      result_ls_array = builder.string_builder().as_cslice().str();
    }

    auto conf = td::json_decode(global_config).move_as_ok();

    std::string result_config_str;
    {
      td::JsonBuilder builder;
      auto obj = builder.enter_object();
      obj("dht", get_json_object_field(conf.get_object(), "dht", td::JsonValue::Type::Object).move_as_ok());
      obj("@type", get_json_object_field(conf.get_object(), "@type", td::JsonValue::Type::String).move_as_ok());
      obj("validator", get_json_object_field(conf.get_object(), "validator", td::JsonValue::Type::Object).move_as_ok());
      obj("liteservers", td::JsonRaw(result_ls_array));
      obj.leave();
      result_config_str = builder.string_builder().as_cslice().str();
    }

    result.push_back(std::move(result_config_str));
  }

  return result;
}

static auto kRandomDevice = std::random_device();
static auto kRandomEngine = std::default_random_engine(kRandomDevice());

template <typename T>
T get_random_index(T from, T to) {
  std::uniform_int_distribution<T> distribution(from, to);
  return distribution(kRandomEngine);
}

}  // namespace

void MultiClientActor::start_up() {
  CHECK(std::filesystem::exists(config_.global_config_path));

  auto global_config = td::read_file_str(config_.global_config_path.string()).move_as_ok();
  auto config_splitted_by_liteservers = split_global_config_by_liteservers(std::move(global_config));

  CHECK(!config_splitted_by_liteservers.empty());

  if (config_.key_store_root.has_value()) {
    if (std::filesystem::exists(*config_.key_store_root)) {
      if (config_.reset_key_store) {
        std::filesystem::remove_all(*config_.key_store_root);
        std::filesystem::create_directories(*config_.key_store_root);
      }
    }
  }

  LOG(INFO) << "starting " << config_splitted_by_liteservers.size() << " client workers";

  for (size_t i = 0; i < config_splitted_by_liteservers.size(); i++) {
    workers_.push_back(WorkerInfo{
        .id = td::actor::create_actor<ClientWrapper>(
            td::actor::ActorOptions().with_name("multiclient_worker_" + std::to_string(i)).with_poll(),
            ClientConfig{
                .global_config = config_splitted_by_liteservers[i],
                .key_store = config_.key_store_root.has_value() ?
                    std::make_optional<std::filesystem::path>(*config_.key_store_root / ("ls_" + std::to_string(i))) :
                    std::nullopt,
                .blockchain_name = config_.blockchain_name,
            }
        ),
    });
  }

  alarm_timestamp() = td::Timestamp::in(1.0);
  next_archival_check_ = td::Timestamp::in(2.0);
}

void MultiClientActor::alarm() {
  static constexpr double kDefaultAlarmInterval = 1.0;
  static constexpr double kCheckArchivalInterval = 10.0;

  LOG(DEBUG) << "Checking alive workers";
  check_alive();

  if (next_archival_check_.is_in_past()) {
    LOG(DEBUG) << "Checking archival workers";
    check_archival();
    next_archival_check_ = td::Timestamp::in(kCheckArchivalInterval);
  }

  alarm_timestamp() = td::Timestamp::in(kDefaultAlarmInterval);
}

void MultiClientActor::check_alive() {
  for (size_t worker_index = 0; worker_index < workers_.size(); worker_index++) {
    auto& worker = workers_[worker_index];
    if (worker.is_waiting_for_update) {
      LOG(DEBUG) << "LS #" << worker_index << " is waiting for update";
      continue;
    }

    if (!worker.is_alive) {
      if (worker.check_retry_count > config_.max_consecutive_alive_check_errors) {
        LOG(DEBUG) << "LS #" << worker_index << " is dead, retry count exceeded";
        continue;
      }

      if (worker.check_retry_after.has_value() && worker.check_retry_after->is_in_past()) {
        LOG(DEBUG) << "LS #" << worker_index << " retrying check";
        worker.check_retry_count++;
        worker.check_retry_after = std::nullopt;
      } else if (worker.check_retry_count != 0) {
        LOG(DEBUG) << "LS #" << worker_index << " waiting for retry";
        continue;
      }
    }

    worker.is_waiting_for_update = true;
    send_worker_request<ton::tonlib_api::blocks_getMasterchainInfo>(
        worker_index,
        ton::tonlib_api::blocks_getMasterchainInfo(),
        [self_id = actor_id(this), worker_index](auto result) {
          td::actor::send_closure(
              self_id,
              &MultiClientActor::on_alive_checked,
              worker_index,
              result.is_ok() ? std::make_optional(result.ok()->last_->seqno_) : std::nullopt
          );
        }
    );
  }
}

void MultiClientActor::on_alive_checked(size_t worker_index, std::optional<int32_t> last_mc_seqno) {
  static constexpr double kRetryInterval = 10.0;

  bool is_alive = last_mc_seqno.has_value();
  int32_t last_mc_seqno_value = last_mc_seqno.value_or(-1);

  LOG(DEBUG) << "LS #" << worker_index << " is_alive: " << is_alive << " last_mc_seqno: " << last_mc_seqno_value;

  auto& worker = workers_[worker_index];
  worker.is_alive = is_alive;
  worker.is_waiting_for_update = false;

  if (is_alive) {
    worker.last_mc_seqno = last_mc_seqno_value;
    worker.check_retry_count = 0;
  } else {
    worker.check_retry_after = td::Timestamp::in(kRetryInterval);
  }
}

void MultiClientActor::check_archival() {
  static constexpr int32_t kBlockWorkchain = ton::masterchainId;
  static constexpr int64_t kBlockShard = ton::shardIdAll;
  static constexpr int32_t kBlockSeqno = 3;

  static constexpr int kLookupMode = 1;
  static constexpr int kLookupLt = 0;
  static constexpr int kLookupUtime = 0;

  for (size_t worker_index = 0; worker_index < workers_.size(); worker_index++) {
    if (!workers_[worker_index].is_alive) {
      continue;
    }

    send_worker_request<ton::tonlib_api::blocks_lookupBlock>(
        worker_index,
        ton::tonlib_api::blocks_lookupBlock(
            kLookupMode,
            ton::tonlib_api::make_object<ton::tonlib_api::ton_blockId>(kBlockWorkchain, kBlockShard, kBlockSeqno),
            kLookupLt,
            kLookupUtime
        ),
        [self_id = actor_id(this), worker_index](auto result) {
          td::actor::send_closure(self_id, &MultiClientActor::on_archival_checked, worker_index, result.is_ok());
        }
    );
  }
}

void MultiClientActor::on_archival_checked(size_t worker_index, bool is_archival) {
  LOG(DEBUG) << "LS #" << worker_index << " archival: " << is_archival;
  workers_[worker_index].is_archival = is_archival;
}

std::vector<size_t> MultiClientActor::select_workers(const RequestParameters& options) const {
  std::vector<size_t> result;
  result.reserve(workers_.size());

  for (size_t i : std::views::iota(0u, workers_.size()) |
           std::views::filter([&](size_t i) { return workers_[i].is_alive; }) |
           std::views::filter([&](size_t i) { return options.archival == true ? workers_[i].is_archival : true; })) {
    result.push_back(i);
  }

  if (result.empty()) {
    return result;
  }

  switch (options.mode) {
    case RequestMode::Broadcast:
      return result;

    case RequestMode::Single: {
      if (options.lite_server_indexes.has_value()) {
        CHECK(!options.lite_server_indexes->empty() && options.lite_server_indexes->size() == 1);

        return std::find(result.begin(), result.end(), options.lite_server_indexes->front()) != result.end() ?
            std::vector<size_t>{options.lite_server_indexes.value().front()} :
            std::vector<size_t>{};
      }

      return std::vector<size_t>{result[get_random_index<size_t>(0, result.size() - 1)]};
    }

    case RequestMode::Multiple: {
      CHECK(!(options.clients_number.has_value() && options.lite_server_indexes.has_value()));
      CHECK(options.clients_number.has_value() || options.lite_server_indexes.has_value());

      if (options.lite_server_indexes.has_value()) {
        std::set<size_t> result_set(result.begin(), result.end());
        for (auto index : options.lite_server_indexes.value()) {
          if (!result_set.contains(index)) {
            result_set.erase(index);
          }
        }
        return std::vector<size_t>(result_set.begin(), result_set.end());
      }

      std::shuffle(result.begin(), result.end(), kRandomEngine);
      result.resize(std::min<size_t>(options.clients_number.value(), result.size()));
      return result;
    }
  }
}


}  // namespace multiclient
