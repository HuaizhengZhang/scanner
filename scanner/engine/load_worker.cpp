/* Copyright 2016 Carnegie Mellon University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scanner/engine/load_worker.h"
#include "scanner/engine/column_source.h"

#include "storehouse/storage_backend.h"

#include <glog/logging.h>

using storehouse::StoreResult;
using storehouse::WriteFile;
using storehouse::RandomReadFile;

namespace scanner {
namespace internal {

// FIXME(apoms): This should be a configuration option
const i32 MAX_SOURCE_THREADS = 16;

LoadWorker::LoadWorker(const LoadWorkerArgs& args)
  : node_id_(args.node_id),
    worker_id_(args.worker_id),
    profiler_(args.profiler),
    io_packet_size_(args.io_packet_size),
    work_packet_size_(args.work_packet_size),
    thread_pool_(std::max(
        1, std::min((i32)args.source_factories.size(), MAX_SOURCE_THREADS))) {
  // Instantiate the sources and validate that it was properly constructed
  num_columns_ = 0;
  for (size_t i = 0; i < args.source_factories.size(); ++i) {
    sources_.emplace_back();
    auto& source = sources_.back();
    source.reset(
        args.source_factories[i]->new_instance(args.source_configs[i]));

    if (auto column_source = dynamic_cast<ColumnSource*>(source.get())) {
      column_source->set_table_meta(&args.table_meta);
    }

    source->set_profiler(&profiler_);

    source->validate(&args.result);
    VLOG(1) << "Source finished validation " << args.result.success();
    if (!args.result.success()) {
      LOG(ERROR) << "Source validate failed: " << args.result.msg();
      THREAD_RETURN_SUCCESS();
    }
    num_columns_ += args.source_configs[i].output_columns.size();

    source_names_.push_back(args.source_factories[i]->get_name());
  }

  source_configs_ = args.source_configs;
}

void LoadWorker::feed(LoadWorkEntry& input_entry) {
  LoadWorkEntry& load_work_entry = input_entry;

  entry_ = input_entry;
  current_row_ = 0;
  total_rows_ = 0;
  for (auto& sample : load_work_entry.source_args()) {
    total_rows_ = std::max((i64)sample.args_size(), total_rows_);
  }
}

bool LoadWorker::yield(i32 item_size,
                       EvalWorkEntry& output_entry) {
  LoadWorkEntry& load_work_entry = entry_;

  // Ignoring item size for now and just yielding one IO item at a time
  if (current_row_ >= total_rows_) {
    return false;
  }

  EvalWorkEntry eval_work_entry;
  eval_work_entry.table_id = load_work_entry.table_id();
  eval_work_entry.job_index = load_work_entry.job_index();
  eval_work_entry.task_index = load_work_entry.task_index();

  eval_work_entry.columns.resize(sources_.size());

  eval_work_entry.video_encoding_type.resize(sources_.size());
  eval_work_entry.inplace_video.resize(sources_.size());
  eval_work_entry.row_ids.resize(sources_.size());
  eval_work_entry.column_types.resize(sources_.size());
  eval_work_entry.column_handles.resize(sources_.size());

  auto load_source = [&](i32 i) {
    // For each source, pass an item_size worth of EnumeratorArgs to the
    // source to read those elements from storage
    const auto& source_args = load_work_entry.source_args(i);

    i64 total_rows = source_args.args_size();
    i64 row_start = current_row_;
    i64 row_end = std::min(current_row_ + item_size, total_rows);

    // Determine what the input and output row ids are for an item_size
    // group of rows
    const auto& sample_rows = source_args.input_row_ids();
    const auto& output_row_ids = source_args.output_row_ids();
    std::vector<i64> rows(sample_rows.begin() + row_start,
                          sample_rows.begin() + row_end);
    std::vector<i64> output_rows(output_row_ids.begin() + row_start,
                                 output_row_ids.begin() + row_end);

    // Form the element args to pass to the source
    std::vector<ElementArgs> element_args(row_end - row_start);
    for (i64 j = row_start; j < row_end; ++j) {
      auto& ea = element_args[j - row_start];
      ea.row_id = rows[j - row_start];
      ea.args = std::vector<u8>(source_args.args(j).begin(),
                                source_args.args(j).end());
    }

    // Pass to the source to read the data
    std::vector<Elements> elements(1);
    if (element_args.size() > 0) {
      sources_[i]->read(element_args, elements);
    }
    eval_work_entry.columns[i] = elements[0];

    // For each output column, insert necessary metadata
    // for (size_t out_col = 0; out_col < elements.size(); ++out_col) {
    size_t out_col = 0;
    proto::ColumnType column_type =
        source_configs_[i].output_column_types[out_col];

    // If this is a ColumnSource type, determine if the columns are video
    // encoded and if they use the inplace decoder
    if (auto column_source = dynamic_cast<ColumnSource*>(sources_[i].get())) {
      proto::VideoDescriptor::VideoCodecType codec_type;
      bool inplace_video;
      column_source->get_video_column_information(codec_type, inplace_video);
      eval_work_entry.video_encoding_type[i] = codec_type;
      eval_work_entry.inplace_video[i] = inplace_video;
    } else {
      if (column_type == ColumnType::Video) {
        eval_work_entry.video_encoding_type[i] = proto::VideoDescriptor::RAW;
      } else {
        LOG_IF(FATAL, elements[0].size() != element_args.size())
          << "Source " << source_names_[i] << " produced " << elements[0].size() << " rows, but "
          << element_args.size() << " rows were expected.";
      }
      eval_work_entry.inplace_video[i] = false;
    }

    eval_work_entry.row_ids[i] = output_rows;
    eval_work_entry.column_types[i] = column_type;
    eval_work_entry.column_handles[i] = CPU_DEVICE;
  };

  // HACK(apoms): When a pipeline has a large number of sources, loading each
  // source serially can take ages. This is a quick hack to overlap reads
  // for pipelines with a large number of sources. The real fix is to process
  // multiple sources as different Ops in a DAG scheduler.
  auto source_start = now();
  std::vector<std::future<void>> futures;
  for (size_t i = 0; i < sources_.size(); ++i) {
    futures.push_back(thread_pool_.enqueue(load_source, i));
  }

  for (auto& future : futures) {
    future.wait();
  }
  profiler_.add_interval("load_worker:read_sources", source_start, now());

  output_entry = eval_work_entry;

  current_row_ += item_size;

  return true;
}

bool LoadWorker::done() { return current_row_ >= total_rows_; }

}
}
