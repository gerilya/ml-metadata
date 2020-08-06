/* Copyright 2020 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "ml_metadata/tools/mlmd_bench/fill_events_workload.h"

#include <random>
#include <vector>

#include "ml_metadata/metadata_store/metadata_store.h"
#include "ml_metadata/metadata_store/types.h"
#include "ml_metadata/proto/metadata_store_service.pb.h"
#include "ml_metadata/tools/mlmd_bench/proto/mlmd_bench.pb.h"
#include "ml_metadata/tools/mlmd_bench/util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/logging.h"

namespace ml_metadata {
namespace {

// Generates and returns a categorical distribution with Dirichlet Prior
// specified by `dist`.
std::discrete_distribution<int64>
GenerateCategoricalDistributionWithDirichletPrior(
    const int64 sample_size, const CategoricalDistribution& dist,
    std::minstd_rand0& gen) {
  // With a source of Gamma-distributed random variates, draws `sample_size`
  // independent random samples and store in `weights`.
  std::gamma_distribution<double> gamma_distribution(dist.dirichlet_alpha(),
                                                     1.0);
  std::vector<double> weights(sample_size);
  for (int64 i = 0; i < sample_size; ++i) {
    weights[i] = gamma_distribution(gen);
  }
  // Uses these random number generated w.r.t. a Dirichlet distribution with
  // `concentration_param` to represent the possibility of being chosen for each
  // integer within [0, sample_size) in a discrete distribution.
  return std::discrete_distribution<int64>{weights.begin(), weights.end()};
}

bool CheckDuplicateOutputArtifactInCurrentSetUp(
    const int64 output_artifact_id,
    std::unordered_set<int64>& output_artifact_ids) {
  if (output_artifact_ids.find(output_artifact_id) !=
      output_artifact_ids.end()) {
    return true;
  }
  output_artifact_ids.insert(output_artifact_id);
  return false;
}

tensorflow::Status CheckDuplicateOutputArtifactInDb(
    const int64 output_artifact_id, MetadataStore& store) {
  GetEventsByArtifactIDsRequest request;
  request.add_artifact_ids(output_artifact_id);
  GetEventsByArtifactIDsResponse response;
  TF_RETURN_IF_ERROR(store.GetEventsByArtifactIDs(request, &response));
  for (const auto& event : response.events()) {
    if (event.type() == Event::OUTPUT) {
      return tensorflow::errors::AlreadyExists(
          ("The current artifact has been outputted by another output event!"));
    }
  }
  return tensorflow::Status::OK();
}

int64 GetTransferredBytes(const Event& event) {
  int bytes = 0;
  bytes += 8 * 2 + 1;
  for (const auto& step : event.path().steps()) {
    bytes += step.key().size();
  }
  return bytes;
}

void SetEvent(const FillEventsConfig& fill_events_config,
              const int64 artifact_id, const int64 execution_id,
              PutEventsRequest& put_request, int64& curr_bytes) {
  Event* event = put_request.add_events();
  switch (fill_events_config.specification()) {
    case FillEventsConfig::INPUT: {
      event->set_type(Event::INPUT);
      break;
    }
    case FillEventsConfig::OUTPUT: {
      event->set_type(Event::OUTPUT);
      break;
    }
    default:
      LOG(FATAL) << "Wrong specification for FillEvents!";
  }
  event->set_artifact_id(artifact_id);
  event->set_execution_id(execution_id);
  event->mutable_path()->add_steps()->set_key("foo");
  curr_bytes += GetTransferredBytes(*event);
}

tensorflow::Status GenerateEvent(
    const FillEventsConfig& fill_events_config,
    const std::vector<Node>& existing_artifact_nodes,
    const std::vector<Node>& existing_execution_nodes, const int64 num_events,
    std::discrete_distribution<int64>& artifact_index_dist,
    std::discrete_distribution<int64>& execution_index_dist,
    std::minstd_rand0& gen, MetadataStore& store,
    std::unordered_set<int64>& output_artifact_ids,
    PutEventsRequest& put_request, int64& curr_bytes) {
  int i = 0;
  while (i < num_events) {
    const int64 artifact_id =
        absl::get<Artifact>(existing_artifact_nodes[artifact_index_dist(gen)])
            .id();
    const int64 execution_id =
        absl::get<Execution>(
            existing_execution_nodes[execution_index_dist(gen)])
            .id();
    if (fill_events_config.specification() == FillEventsConfig::OUTPUT) {
      bool artifact_has_been_outputted_in_setup =
          CheckDuplicateOutputArtifactInCurrentSetUp(artifact_id,
                                                     output_artifact_ids);
      tensorflow::Status status =
          CheckDuplicateOutputArtifactInDb(artifact_id, store);
      if (!status.ok() && status.code() != tensorflow::error::ALREADY_EXISTS) {
        return status;
      }
      // Rejection sampling.
      if (artifact_has_been_outputted_in_setup || !status.ok()) {
        continue;
      }
    }
    SetEvent(fill_events_config, artifact_id, execution_id, put_request,
             curr_bytes);
    i++;
  }
  return tensorflow::Status::OK();
}

}  // namespace

FillEvents::FillEvents(const FillEventsConfig& fill_events_config,
                       int64 num_operations)
    : fill_events_config_(fill_events_config),
      num_operations_(num_operations),
      name_(absl::StrCat("FILL_EVENTS_",
                         fill_events_config_.Specification_Name(
                             fill_events_config_.specification()))) {}

tensorflow::Status FillEvents::SetUpImpl(MetadataStore* store) {
  LOG(INFO) << "Setting up ...";

  int64 curr_bytes = 0;
  std::uniform_int_distribution<int64> num_events_dist{
      fill_events_config_.num_events().minimum(),
      fill_events_config_.num_events().maximum()};

  std::vector<Node> existing_artifact_nodes;
  std::vector<Node> existing_execution_nodes;
  TF_RETURN_IF_ERROR(GetExistingNodes(fill_events_config_, *store,
                                      existing_artifact_nodes,
                                      existing_execution_nodes));

  std::minstd_rand0 gen(absl::ToUnixMillis(absl::Now()));
  // TODO(briansong) Adds Zipf distribution implementation for artifact index
  // distribution.
  std::discrete_distribution<int64> artifact_index_dist =
      GenerateCategoricalDistributionWithDirichletPrior(
          existing_artifact_nodes.size(),
          fill_events_config_.artifact_node_popularity_categorical(), gen);
  std::discrete_distribution<int64> execution_index_dist =
      GenerateCategoricalDistributionWithDirichletPrior(
          existing_execution_nodes.size(),
          fill_events_config_.execution_node_popularity(), gen);

  for (int64 i = 0; i < num_operations_; ++i) {
    curr_bytes = 0;
    PutEventsRequest put_request;
    const int64 num_events = num_events_dist(gen);
    TF_RETURN_IF_ERROR(GenerateEvent(
        fill_events_config_, existing_artifact_nodes, existing_execution_nodes,
        num_events, artifact_index_dist, execution_index_dist, gen, *store,
        output_artifact_ids_, put_request, curr_bytes));
    work_items_.emplace_back(put_request, curr_bytes);
  }

  return tensorflow::Status::OK();
}

tensorflow::Status FillEvents::RunOpImpl(const int64 work_items_index,
                                         MetadataStore* store) {
  PutEventsRequest put_request = work_items_[work_items_index].first;
  PutEventsResponse put_response;
  return store->PutEvents(put_request, &put_response);
}

tensorflow::Status FillEvents::TearDownImpl() {
  work_items_.clear();
  return tensorflow::Status::OK();
}

std::string FillEvents::GetName() { return name_; }

}  // namespace ml_metadata
