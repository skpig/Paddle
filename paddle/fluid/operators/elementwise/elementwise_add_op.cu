/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/framework/pten_utils.h"
#include "paddle/fluid/operators/elementwise/elementwise_add_op.h"
#include "paddle/fluid/operators/elementwise/elementwise_op_broadcast.cu.h"
#include "paddle/fluid/operators/reduce_ops/reduce_functor_op.h"
#include "paddle/fluid/operators/reduce_ops/reduce_op.cu.h"
#include "paddle/fluid/platform/complex.h"
#include "paddle/fluid/platform/float16.h"

// only can include the headers in paddle/top/api dirs
#include "paddle/pten/api/lib/utils/tensor_utils.h"
#include "paddle/pten/include/core.h"
#include "paddle/pten/include/math.h"

namespace ops = paddle::operators;
namespace plat = paddle::platform;

namespace paddle {
namespace operators {

template <typename T>
static __global__ void SimpleElemwiseAddGradCUDAKernel(
    const T* __restrict__ dout, int size, int vec_size, T* dx, T* dy) {
  int tid = blockIdx.x * blockDim.x + threadIdx.x;
  int stride = gridDim.x * blockDim.x;
  int loop = size / vec_size;
  int remainder = size % vec_size;
  const float4* dout_vec = reinterpret_cast<const float4*>(dout);
  float4* dx_vec = reinterpret_cast<float4*>(dx);
  float4* dy_vec = reinterpret_cast<float4*>(dy);
  float4 tmp_loop;

  for (int i = tid; i < loop; i += stride) {
    tmp_loop = dout_vec[i];
    dx_vec[i] = tmp_loop;
    dy_vec[i] = tmp_loop;
  }

  if (tid == loop && remainder != 0) {
    T tmp_rem;
    while (remainder) {
      int idx = size - remainder;
      remainder--;
      tmp_rem = dout[idx];
      dx[idx] = tmp_rem;
      dy[idx] = tmp_rem;
    }
  }
}

template <typename DeviceContext, typename T>
typename std::enable_if<
    std::is_same<DeviceContext, platform::CUDADeviceContext>::value>::type
default_elementwise_add_grad(const framework::ExecutionContext& ctx,
                             const framework::Tensor* x,
                             const framework::Tensor* y,
                             const framework::Tensor* out,
                             const framework::Tensor* dout,
                             framework::Tensor* dx, framework::Tensor* dy) {
  int axis = ctx.Attr<int>("axis");
  auto* dout_data = dout->data<T>();

  // dx
  if (dx != nullptr) {
    auto* dx_data = dx->mutable_data<T>(ctx.GetPlace());
    if (dx->dims() == dout->dims()) {
      if (dx_data != dout_data) {
        framework::TensorCopy(
            *dout, ctx.GetPlace(),
            ctx.template device_context<platform::DeviceContext>(), dx);
      }
    } else {
      // For inplace strategy, dx will be stored in addr of dout, which makes
      // the result of dy wrong.
      if (dx->IsSharedBufferWith(*dout)) {
        dx->clear();
        dx->mutable_data<T>(x->dims(), ctx.GetPlace());
      }
      std::vector<int> reduce_dims = GetReduceDim(x->dims(), out->dims(), axis);
      gpuStream_t stream = ctx.cuda_device_context().stream();
      TensorReduceFunctorImpl<T, T, CustomSum>(*dout, dx, reduce_dims, stream);
    }
  }
  // dy
  if (dy != nullptr) {
    auto* dy_data = dy->mutable_data<T>(ctx.GetPlace());
    if (dy->dims() == dout->dims()) {
      if (dy_data != dout_data) {
        framework::TensorCopy(
            *dout, ctx.GetPlace(),
            ctx.template device_context<platform::DeviceContext>(), dy);
      }
    } else {
      std::vector<int> reduce_dims = GetReduceDim(y->dims(), out->dims(), axis);
      gpuStream_t stream = ctx.cuda_device_context().stream();
      TensorReduceFunctorImpl<T, T, CustomSum>(*dout, dy, reduce_dims, stream);
    }
  }
}

template <typename DeviceContext, typename T>
typename std::enable_if<
    std::is_same<DeviceContext, plat::CUDADeviceContext>::value>::type
elementwise_add_grad(const framework::ExecutionContext& ctx,
                     const framework::Tensor* x, const framework::Tensor* y,
                     const framework::Tensor* out,
                     const framework::Tensor* dout, framework::Tensor* dx,
                     framework::Tensor* dy) {
  auto* dx_data = dx->mutable_data<T>(ctx.GetPlace());
  auto* dy_data = dy->mutable_data<T>(ctx.GetPlace());
  auto* dout_data = dout->data<T>();
  if (dx_data == dout_data && dy_data != dout_data) {
    VLOG(4) << "Special case when dx_data is the same as dout_data, "
               "only need copy dout to dy";
    framework::TensorCopy(
        *dout, ctx.GetPlace(),
        ctx.template device_context<platform::DeviceContext>(), dy);
  } else if (dx_data != dout_data && dy_data == dout_data) {
    VLOG(4) << "Special case when dy_data is the same as dout_data, "
               "only need copy dout to dx";
    framework::TensorCopy(
        *dout, ctx.GetPlace(),
        ctx.template device_context<platform::DeviceContext>(), dx);
  } else if (dx_data != dout_data && dy_data != dout_data) {
    auto size = x->numel();
    int vec_size = max(static_cast<int>(sizeof(float4) / sizeof(T)), 1);
    dim3 block_size = dim3(ELEMENTWISE_BLOCK_SIZE, 1);
    dim3 grid_size =
        dim3(((size + vec_size - 1) / vec_size + ELEMENTWISE_BLOCK_SIZE - 1) /
                 ELEMENTWISE_BLOCK_SIZE,
             1);
    SimpleElemwiseAddGradCUDAKernel<
        T><<<grid_size, block_size, 0,
             ctx.template device_context<plat::CUDADeviceContext>().stream()>>>(
        dout->data<T>(), size, vec_size, dx->mutable_data<T>(ctx.GetPlace()),
        dy->mutable_data<T>(ctx.GetPlace()));
  } else {
    VLOG(4) << "Special case when dy_data is the same as dout_data, "
               "and dx_data is the same as dout_data, do not need "
               "any operator";
  }
}

}  // namespace operators
}  // namespace paddle
REGISTER_OP_CUDA_KERNEL(
    elementwise_add, ops::ElementwiseAddKernel<plat::CUDADeviceContext, float>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, double>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, int>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, int64_t>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, plat::float16>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, plat::complex<float>>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, plat::complex<double>>);
REGISTER_OP_CUDA_KERNEL(
    elementwise_add_grad,
    ops::ElementwiseAddGradKernel<plat::CUDADeviceContext, float>,
    ops::ElementwiseAddGradKernel<plat::CUDADeviceContext, double>,
    ops::ElementwiseAddGradKernel<plat::CUDADeviceContext, int>,
    ops::ElementwiseAddGradKernel<plat::CUDADeviceContext, int64_t>,
    ops::ElementwiseAddGradKernel<plat::CUDADeviceContext, plat::float16>,
    ops::ElementwiseAddGradKernel<plat::CUDADeviceContext,
                                  plat::complex<float>>,
    ops::ElementwiseAddGradKernel<plat::CUDADeviceContext,
                                  plat::complex<double>>);
REGISTER_OP_CUDA_KERNEL(
    elementwise_add_grad_grad,
    ops::ElementwiseAddDoubleGradKernel<plat::CUDADeviceContext, float>,
    ops::ElementwiseAddDoubleGradKernel<plat::CUDADeviceContext, double>,
    ops::ElementwiseAddDoubleGradKernel<plat::CUDADeviceContext, int>,
    ops::ElementwiseAddDoubleGradKernel<plat::CUDADeviceContext, int64_t>,
    ops::ElementwiseAddDoubleGradKernel<plat::CUDADeviceContext, plat::float16>,
    ops::ElementwiseAddDoubleGradKernel<plat::CUDADeviceContext,
                                        plat::complex<float>>,
    ops::ElementwiseAddDoubleGradKernel<plat::CUDADeviceContext,
                                        plat::complex<double>>);
REGISTER_OP_CUDA_KERNEL(
    elementwise_add_triple_grad,
    ops::ElementwiseAddTripleGradKernel<plat::CUDADeviceContext, float>,
    ops::ElementwiseAddTripleGradKernel<plat::CUDADeviceContext, double>,
    ops::ElementwiseAddTripleGradKernel<plat::CUDADeviceContext, int>,
    ops::ElementwiseAddTripleGradKernel<plat::CUDADeviceContext, int64_t>,
    ops::ElementwiseAddTripleGradKernel<plat::CUDADeviceContext, plat::float16>,
    ops::ElementwiseAddTripleGradKernel<plat::CUDADeviceContext,
                                        plat::complex<float>>,
    ops::ElementwiseAddTripleGradKernel<plat::CUDADeviceContext,
                                        plat::complex<double>>);

REGISTER_OP_CUDA_KERNEL(
    grad_add, ops::ElementwiseAddKernel<plat::CUDADeviceContext, float>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, double>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, int>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, int64_t>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, plat::float16>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, plat::complex<float>>,
    ops::ElementwiseAddKernel<plat::CUDADeviceContext, plat::complex<double>>);
