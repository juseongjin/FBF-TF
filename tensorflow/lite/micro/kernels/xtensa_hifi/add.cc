/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/lite/kernels/internal/reference/add.h"

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/add.h"
#include "tensorflow/lite/kernels/internal/reference/process_broadcast_shapes.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/op_macros.h"
#include "tensorflow/lite/micro/kernels/xtensa_hifi/xtensa_tf_micro_common.h"
#include "tensorflow/lite/micro/memory_helpers.h"

namespace tflite {
namespace ops {
namespace micro {
namespace add {

constexpr int kInputTensor1 = 0;
constexpr int kInputTensor2 = 1;
constexpr int kOutputTensor = 0;

struct OpData {
  bool requires_broadcast;

  // These fields are used in both the general 8-bit -> 8bit quantized path,
  // and the special 16-bit -> 16bit quantized path
  int input1_shift;
  int input2_shift;
  int32 output_activation_min;
  int32 output_activation_max;

  // These fields are used only in the general 8-bit -> 8bit quantized path
  int32 input1_multiplier;
  int32 input2_multiplier;
  int32 output_multiplier;
  int output_shift;
  int left_shift;
  int32 input1_offset;
  int32 input2_offset;
  int32 output_offset;
};

TfLiteStatus CalculateOpData(TfLiteContext* context, TfLiteAddParams* params,
                             const TfLiteTensor* input1,
                             const TfLiteTensor* input2, TfLiteTensor* output,
                             OpData* data) {
  data->requires_broadcast = !HaveSameShapes(input1, input2);

  if (output->type == kTfLiteUInt8 || output->type == kTfLiteInt8) {
    // 8bit -> 8bit general quantized path, with general rescalings
    data->input1_offset = -input1->params.zero_point;
    data->input2_offset = -input2->params.zero_point;
    data->output_offset = output->params.zero_point;
    data->left_shift = 20;
    const double twice_max_input_scale =
        2 * static_cast<double>(
                std::max(input1->params.scale, input2->params.scale));
    const double real_input1_multiplier =
        static_cast<double>(input1->params.scale) / twice_max_input_scale;
    const double real_input2_multiplier =
        static_cast<double>(input2->params.scale) / twice_max_input_scale;
    const double real_output_multiplier =
        twice_max_input_scale /
        ((1 << data->left_shift) * static_cast<double>(output->params.scale));

    QuantizeMultiplierSmallerThanOneExp(
        real_input1_multiplier, &data->input1_multiplier, &data->input1_shift);

    QuantizeMultiplierSmallerThanOneExp(
        real_input2_multiplier, &data->input2_multiplier, &data->input2_shift);

    QuantizeMultiplierSmallerThanOneExp(
        real_output_multiplier, &data->output_multiplier, &data->output_shift);

    TF_LITE_ENSURE_STATUS(CalculateActivationRangeQuantized(
        context, params->activation, output, &data->output_activation_min,
        &data->output_activation_max));
  }

  return kTfLiteOk;
}

TfLiteStatus EvalAdd(TfLiteContext* context, TfLiteNode* node,
                     TfLiteAddParams* params, const OpData* data,
                     const TfLiteTensor* input1, const TfLiteTensor* input2,
                     TfLiteTensor* output) {
  float output_activation_min, output_activation_max;
  CalculateActivationRange(params->activation, &output_activation_min,
                           &output_activation_max);
  tflite::ArithmeticParams op_params;
  SetActivationParams(output_activation_min, output_activation_max, &op_params);
#define TF_LITE_ADD(opname)                                                   \
  reference_ops::opname(op_params, GetTensorShape(input1),                    \
                        GetTensorData<float>(input1), GetTensorShape(input2), \
                        GetTensorData<float>(input2), GetTensorShape(output), \
                        GetTensorData<float>(output))
  if (data->requires_broadcast) {
    TF_LITE_ADD(BroadcastAdd4DSlow);
  } else {
#if HIFI_VFPU
    int err;
    const RuntimeShape& input1_shape = GetTensorShape(input1);
    const RuntimeShape& input2_shape = GetTensorShape(input2);
    const RuntimeShape& output_shape = GetTensorShape(output);
    const int flat_size =
        MatchingElementsSize(input1_shape, input2_shape, output_shape);

    err = xa_nn_elm_add_f32xf32_f32(GetTensorData<float>(output),
                                    GetTensorData<float>(input1),
                                    GetTensorData<float>(input2), flat_size);

    CHECK_ERR_HIFI_NNLIB_KER(err, "xa_nn_elm_add_f32xf32_f32 failed");

    err = xa_nn_vec_activation_min_max_f32_f32(
        GetTensorData<float>(output), GetTensorData<float>(output),
        output_activation_min, output_activation_max, flat_size);

    CHECK_ERR_HIFI_NNLIB_KER(err,
                             "xa_nn_vec_activation_min_max_f32_f32 failed");
#else
    TF_LITE_ADD(Add);
#endif /* HIFI_VFPU */
  }
#undef TF_LITE_ADD
  return kTfLiteOk;
}

TfLiteStatus EvalAddQuantized(TfLiteContext* context, TfLiteNode* node,
                              TfLiteAddParams* params, const OpData* data,
                              const TfLiteTensor* input1,
                              const TfLiteTensor* input2,
                              TfLiteTensor* output) {
  if (output->type == kTfLiteUInt8 || output->type == kTfLiteInt8) {
    tflite::ArithmeticParams op_params;
    op_params.left_shift = data->left_shift;
    op_params.input1_offset = data->input1_offset;
    op_params.input1_multiplier = data->input1_multiplier;
    op_params.input1_shift = data->input1_shift;
    op_params.input2_offset = data->input2_offset;
    op_params.input2_multiplier = data->input2_multiplier;
    op_params.input2_shift = data->input2_shift;
    op_params.output_offset = data->output_offset;
    op_params.output_multiplier = data->output_multiplier;
    op_params.output_shift = data->output_shift;
    SetActivationParams(data->output_activation_min,
                        data->output_activation_max, &op_params);
    bool need_broadcast = reference_ops::ProcessBroadcastShapes(
        GetTensorShape(input1), GetTensorShape(input2), &op_params);
#define TF_LITE_ADD(type, opname, dtype)                             \
  type::opname(op_params, GetTensorShape(input1),                    \
               GetTensorData<dtype>(input1), GetTensorShape(input2), \
               GetTensorData<dtype>(input2), GetTensorShape(output), \
               GetTensorData<dtype>(output));
    if (output->type == kTfLiteInt8) {
      if (need_broadcast) {
        TF_LITE_ADD(reference_integer_ops, BroadcastAdd4DSlow, int8_t);
      } else {
        TF_LITE_ADD(reference_integer_ops, Add, int8_t);
      }
    } else {
      if (need_broadcast) {
        TF_LITE_ADD(reference_ops, BroadcastAdd4DSlow, uint8_t);
      } else {
        int err;
        const RuntimeShape& input1_shape = GetTensorShape(input1);
        const RuntimeShape& input2_shape = GetTensorShape(input2);
        const RuntimeShape& output_shape = GetTensorShape(output);
        const int flat_size =
            MatchingElementsSize(input1_shape, input2_shape, output_shape);

        err = xa_nn_elm_add_asym8xasym8_asym8(
            GetTensorData<uint8_t>(output), op_params.output_offset,
            op_params.output_shift, op_params.output_multiplier,
            op_params.quantized_activation_min,
            op_params.quantized_activation_max, GetTensorData<uint8_t>(input1),
            op_params.input1_offset, op_params.input1_shift,
            op_params.input1_multiplier, GetTensorData<uint8_t>(input2),
            op_params.input2_offset, op_params.input2_shift,
            op_params.input2_multiplier, op_params.left_shift, flat_size);

        CHECK_ERR_HIFI_NNLIB_KER(err, "xa_nn_elm_add_asym8xasym8_asym8 failed");
      }
    }
#undef TF_LITE_ADD
  }

  return kTfLiteOk;
}

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  TFLITE_DCHECK(context->AllocatePersistentBuffer != nullptr);
  void* data = nullptr;
  if (context->AllocatePersistentBuffer(context, sizeof(OpData), &data) ==
      kTfLiteError) {
    return nullptr;
  }
  return data;
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  const TfLiteTensor* input1 = GetInput(context, node, kInputTensor1);
  const TfLiteTensor* input2 = GetInput(context, node, kInputTensor2);
  TfLiteTensor* output = GetOutput(context, node, kOutputTensor);

  OpData* data = static_cast<OpData*>(node->user_data);
  auto* params = reinterpret_cast<TfLiteAddParams*>(node->builtin_data);

  TF_LITE_ENSURE_STATUS(
      CalculateOpData(context, params, input1, input2, output, data));

  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  auto* params = reinterpret_cast<TfLiteAddParams*>(node->builtin_data);

  TFLITE_DCHECK(node->user_data != nullptr);
  const OpData* data = static_cast<const OpData*>(node->user_data);

  const TfLiteTensor* input1 = GetInput(context, node, kInputTensor1);
  const TfLiteTensor* input2 = GetInput(context, node, kInputTensor2);
  TfLiteTensor* output = GetOutput(context, node, kOutputTensor);

  if (output->type == kTfLiteFloat32) {
    TF_LITE_ENSURE_OK(
        context, EvalAdd(context, node, params, data, input1, input2, output));
  } else if (output->type == kTfLiteUInt8 || output->type == kTfLiteInt8) {
    TF_LITE_ENSURE_OK(context, EvalAddQuantized(context, node, params, data,
                                                input1, input2, output));
  } else {
    TF_LITE_KERNEL_LOG(context, "Type %s (%d) not supported.",
                       TfLiteTypeGetName(output->type), output->type);
    return kTfLiteError;
  }

  return kTfLiteOk;
}

}  // namespace add

TfLiteRegistration Register_ADD() {
  return {/*init=*/add::Init,
          /*free=*/nullptr,
          /*prepare=*/add::Prepare,
          /*invoke=*/add::Eval,
          /*profiling_string=*/nullptr,
          /*builtin_code=*/0,
          /*custom_name=*/nullptr,
          /*version=*/0};
}

}  // namespace micro
}  // namespace ops
}  // namespace tflite