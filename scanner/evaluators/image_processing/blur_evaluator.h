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

#pragma once

#include "scanner/eval/evaluator.h"
#include "scanner/eval/evaluator_constructor.h"

namespace scanner {

class BlurEvaluator : public Evaluator {
public:
  BlurEvaluator(EvaluatorConfig config, int kernel_size, double sigma);

  virtual void configure(const DatasetItemMetadata& metadata) override;

  virtual void evaluate(
    char* input_buffer,
    std::vector<std::vector<char*>>& output_buffers,
    std::vector<std::vector<size_t>>& output_sizes,
    int batch_size) override;

private:
  int kernel_size_;
  int filter_left_;
  int filter_right_;
  double sigma_;

  DatasetItemMetadata metadata_;
};

class BlurEvaluatorConstructor : public EvaluatorConstructor {
public:
  BlurEvaluatorConstructor(int kernel_size, double sigma);

  virtual int get_number_of_devices() override;

  virtual DeviceType get_input_buffer_type() override;

  virtual DeviceType get_output_buffer_type() override;

  virtual int get_number_of_outputs() override;

  virtual std::vector<std::string> get_output_names() override;

  virtual char* new_input_buffer(const EvaluatorConfig& config) override;

  virtual void delete_input_buffer(
    const EvaluatorConfig& config,
    char* buffer) override;

  virtual void delete_output_buffer(
    const EvaluatorConfig& config,
    char* buffers) override;

  virtual Evaluator* new_evaluator(const EvaluatorConfig& config) override;

private:
  int kernel_size_;
  double sigma_;
};

}