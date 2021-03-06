/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "absl/memory/memory.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"  // from @llvm-project
#include "mlir/Dialect/LoopOps/LoopOps.h"  // from @llvm-project
#include "mlir/Dialect/StandardOps/IR/Ops.h"  // from @llvm-project
#include "mlir/IR/StandardTypes.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Transforms/DialectConversion.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/xla/ir/lhlo_ops.h"

namespace mlir {
namespace xla_lhlo {
namespace {

// Converts a block with LHLO ops and with signature:
//   ^bb(%lhs: memref<f32>, %rhs: memref<f32>, %res: memref<f32>):
// into a reduction operator of loop.reduce by doing buffer allocation for
// scalar arguments and the result of `loop.reduce` to make it compatible with
// LHLO ops.
void ConvertToReductionOperator(Location loc, loop::ReduceOp reduce_op,
                                Block* lhlo_block,
                                ConversionPatternRewriter* rewriter) {
  Block& loop_reduce_op_body = reduce_op.reductionOperator().front();
  rewriter->setInsertionPointToStart(&loop_reduce_op_body);

  // Allocate buffers to hold arguments of reduction operator block to stay
  // compatible with the LHLO dialect ops in the reduction body.
  Value elem_arg = lhlo_block->getArgument(0);
  Value elem_buf =
      rewriter->create<AllocOp>(loc, elem_arg.getType().cast<MemRefType>());
  rewriter->create<StoreOp>(loc, loop_reduce_op_body.getArgument(0), elem_buf);
  Value acc_arg = lhlo_block->getArgument(1);
  Value acc_buf =
      rewriter->create<AllocOp>(loc, acc_arg.getType().cast<MemRefType>());
  rewriter->create<StoreOp>(loc, loop_reduce_op_body.getArgument(1), acc_buf);

  // Clone the ops from `xla_lhlo.reduce` into reduction operator block.
  BlockAndValueMapping mapping;
  mapping.map(lhlo_block->getArguments(),
              ValueRange{elem_buf, acc_buf, acc_buf});
  for (auto& nested : lhlo_block->without_terminator()) {
    auto clone = rewriter->clone(nested, mapping);
    mapping.map(nested.getResults(), clone->getResults());
  }
  Value acc_result = rewriter->create<LoadOp>(loc, acc_buf);
  rewriter->create<loop::ReduceReturnOp>(loc, acc_result);
}

// Returns result of ConstantOp if `dim` is static, otherwise uses DimOp to
// extract dimension at runtime.
Value GetStaticOrDynamicDim(mlir::Location loc, Value shaped_value,
                            size_t dim_index, int64_t dim,
                            ConversionPatternRewriter* rewriter) {
  return dim == ShapedType::kDynamicSize
             ? rewriter->create<DimOp>(loc, shaped_value, dim_index).getResult()
             : rewriter->create<ConstantIndexOp>(loc, dim);
}

// Converts `xla_lhlo.ReduceOp` into two loop::ParallelOp and a loop::ReduceOp.
// The outper `ParallelOp` refers to the parallel loops if there are
// any. The inner `ParalleOp` refers to the reduction loops and `ReduceOp`
// contains the reduction operator.
//
// Example:
//
//  "xla_lhlo.reduce"(%buffer, %init_buf, %result) ( {
//    ^bb0(%lhs: memref<f32>, %rhs: memref<f32>, %res: memref<f32>):
//      <LHLO ops>
//    } ) {dimensions = dense<[1]> : tensor<1xi64>}
//      : (memref<100x10x5xf32>, memref<f32>, memref<100x5xf32>) -> ()
//
//  is roughly converted into:
//
//  %init = load %init_buf[] : memref<f32>
//  loop.parallel (%i, %k) = (%c0, %c0) to (%c100, %c5) step (%c1, %c1) {
//    %result = loop.parallel (%j) = (%c0) to (%c10) step (%c1) init (%init) {
//      %elem_to_reduce = load %buffer[%i, %j, %k] : memref<100x10x5xf32>
//      loop.reduce(%elem_to_reduce)  {
//        ^bb0(%elem: f32, %acc: f32):   // no predecessors
//          elem_buf = alloc() : memref<f32>
//          store %elem, elem_buf[] : memref<f32>
//          acc_buf = alloc() : memref<f32>
//          store %acc, acc_buf[] : memref<f32>
//          <LHLO_ops>
//          %acc_result = load acc_buf[] : memref<f32>
//          loop.reduce.return %acc_result : f32
//      } : f32
//      loop.yield
//    } : f32
//    loop.yield
//  }
class ReduceOpConverter : public OpConversionPattern<xla_lhlo::ReduceOp> {
 public:
  using OpConversionPattern<xla_lhlo::ReduceOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      xla_lhlo::ReduceOp xla_reduce_op, ArrayRef<Value> /*args*/,
      ConversionPatternRewriter& rewriter) const final {
    // TODO(b/137624192) Implement variadic reduce.
    if (xla_reduce_op.out().size() != 1) return failure();

    loop::ReduceOp reduce_op =
        CreateReduceOpInNestedParallelLoops(xla_reduce_op, &rewriter);
    ConvertToReductionOperator(xla_reduce_op.getLoc(), reduce_op,
                               &xla_reduce_op.body().front(), &rewriter);
    rewriter.replaceOp(xla_reduce_op, llvm::None);
    return success();
  }

 private:
  // Creates nested `loop.parallel` ops with `loop.reduce`. The outer ParallelOp
  // refers to the parallel dimensions of `xla_reduce_op` if any and the inner
  // ParallelOp refers to the reduction dimensions. The loop.reduce op is
  // returned.
  //
  // If the reduction argument is a memref<100x10x5xf32> and the
  // reduction is performed along dimension 1 then this method will generate
  //
  //  %init = load %init_buf[] : memref<f32>
  //  loop.parallel (%i, %k) = (%c0, %c0) to (%c100, %c5) step (%c1, %c1) {
  //    %result = loop.parallel (%j) = (%c0) to (%c10) step (%c1) init (%init) {
  //      %elem_to_reduce = load %buffer[%i, %j, %k] : memref<100x10x5xf32>
  //      loop.reduce(%elem_to_reduce)  {
  //        <THE BLOCK PTR TO BE RETURNED>
  //      } : f32
  //      loop.yield
  //    } : f32
  //    loop.yield
  //  }
  loop::ReduceOp CreateReduceOpInNestedParallelLoops(
      xla_lhlo::ReduceOp xla_reduce_op,
      ConversionPatternRewriter* rewriter) const {
    auto loc = xla_reduce_op.getLoc();
    DenseSet<int> reducing_dims;
    for (auto rdim : xla_reduce_op.dimensions().getIntValues()) {
      reducing_dims.insert(rdim.getSExtValue());
    }

    Value operand = *xla_reduce_op.operands().begin();
    Value out = *xla_reduce_op.out().begin();
    SmallVector<Value, 2> parallel_lower, parallel_upper, parallel_step;
    SmallVector<Value, 2> reduce_lower, reduce_upper, reduce_step;
    auto operand_shape = operand.getType().cast<MemRefType>().getShape();
    for (auto dim : llvm::enumerate(operand_shape)) {
      const bool is_reducing_dim = reducing_dims.count(dim.index());

      Value ub = GetStaticOrDynamicDim(loc, operand, dim.index(), dim.value(),
                                       rewriter);
      Value lb = rewriter->create<ConstantIndexOp>(loc, 0);
      Value step = rewriter->create<ConstantIndexOp>(loc, 1);
      (is_reducing_dim ? reduce_lower : parallel_lower).push_back(lb);
      (is_reducing_dim ? reduce_upper : parallel_upper).push_back(ub);
      (is_reducing_dim ? reduce_step : parallel_step).push_back(step);
    }
    // Load initial value from memref<element_type>.
    SmallVector<Value, 1> init_value = {
        rewriter->create<LoadOp>(loc, *xla_reduce_op.init_values().begin())};
    // Outer ParallelOp is not needed if it is a reduction across all dims.
    loop::ParallelOp outer;
    if (!parallel_lower.empty()) {
      outer = rewriter->create<loop::ParallelOp>(loc, parallel_lower,
                                                 parallel_upper, parallel_step);
      rewriter->setInsertionPointToStart(outer.getBody());
    }
    loop::ParallelOp inner = rewriter->create<loop::ParallelOp>(
        loc, reduce_lower, reduce_upper, reduce_step, init_value);
    Value reduction_result = *inner.getResults().begin();

    SmallVector<Value, 1> out_indices;
    if (outer != nullptr) {
      out_indices.reserve(outer.getNumLoops());
      for (auto& iv : outer.getInductionVars()) {
        out_indices.push_back(iv);
      }
    } else {
      out_indices.push_back(rewriter->create<ConstantIndexOp>(loc, 0));
    }

    rewriter->create<StoreOp>(loc, reduction_result, out, out_indices);

    // Load the element to reduce.
    SmallVector<Value, 2> indices;
    indices.reserve(operand_shape.size());
    Block::args_iterator outer_ivs_it =
        outer ? outer.getInductionVars().begin() : nullptr;
    Block::args_iterator inner_ivs_it = inner.getInductionVars().begin();
    for (unsigned i = 0, e = operand_shape.size(); i < e; ++i) {
      indices.push_back(reducing_dims.count(i) ? *inner_ivs_it++
                                               : *outer_ivs_it++);
    }

    rewriter->setInsertionPointToStart(inner.getBody());
    Value elem = rewriter->create<mlir::LoadOp>(
        loc, *xla_reduce_op.operands().begin(), indices);
    return rewriter->create<loop::ReduceOp>(loc, elem);
  }
};

// Pseudocode:
// for each index O in output
//   accumulator = neutral_value
//   in_bounds = true
//   for each index W in window
//     for each dimension i from 0 to rank - 1
//       index = O[i] * stride[i] + W[i] - pad_low[i]
//       in_bounds = inbounds && (index `ult` shape[i])
//       I[i] = index
//     if (in_bounds)
//       value = input[I]
//     else
//       value = neutral_value
//     accumulator = reduction_operator(output[O], value)
//   output[O] = accumulator
//
// Converts `xla_lhlo.ReduceWindowOp` into two loop::ParallelOp and a
// loop::ReduceOp.
// The outper `ParallelOp` refers to the parallel loops that traverese output
// buffer. The inner `ParalleOp` refers to the reduction loops that traverse
// reduction windows and `ReduceOp` contains the reduction operator.
//
// Example:
//
// func @reduce_window(%arg: memref<112x112xf32>,
//              %init: memref<f32>,
//              %result: memref<56x56xf32>) {
//   "xla_lhlo.reduce_window"(%arg, %init, %result) ( {
//     ^bb0(%lhs: memref<f32>, %rhs: memref<f32>, %res: memref<f32>):
//       "xla_lhlo.maximum"(%lhs, %rhs, %res)
//         : (memref<f32>, memref<f32>, memref<f32>) -> ()
//       "xla_lhlo.terminator"() : () -> ()
//     }) {
//       padding = dense<[[0, 1], [0, 1]]> : tensor<2x2xi64>,
//       window_dimensions = dense<[3, 3]> : tensor<2xi64>,
//       window_strides = dense<[2, 2]> : tensor<2xi64>
//     } : (memref<112x112xf32>, memref<f32>, memref<56x56xf32>) -> ()
//   return
// }
//
// is roughly converted into:
//
//    %neutral_elem = load %init_buf[] : memref<f32>
//    loop.parallel (%i, %j) = (%c0, %c0) to (%c56, %c56) step (%c1, %c1) {
//      %result = loop.parallel (%iw, %jw) = (%c0, %c0)
//                  to (%c3, %c3) step (%c1, %c1) neutral_elem (%0) -> f32 {
//        %in_bounds = <COMPUTE IF INDEX IS IN OPERAND'S pad>
//        %elem = load %operand[%computed_i, %computed_j]
//        %elem_or_neutral = select %in_bounds, %elem, %neutral_elem : f32
//        loop.reduce(%elem_to_reduce)  : f32 {
//          ^bb0(%arg7: f32, %arg8: f32):
//            <LHLO ops>
//        }
//        loop.yield
//      }
//      store %result, %output_buffer[%i, %j] : memref<56x56xf32>
//      loop.yield
//    }
//    return
//  }
class ReduceWindowOpConverter
    : public OpConversionPattern<xla_lhlo::ReduceWindowOp> {
 public:
  using OpConversionPattern<xla_lhlo::ReduceWindowOp>::OpConversionPattern;

  LogicalResult matchAndRewrite(
      xla_lhlo::ReduceWindowOp xla_reduce_window_op, ArrayRef<Value> /*args*/,
      ConversionPatternRewriter& rewriter) const final {
    loop::ParallelOp output_loop, window_loop;
    std::tie(output_loop, window_loop) =
        CreateParallelLoopsToTraverseOutputAndWindow(xla_reduce_window_op,
                                                     &rewriter);

    loop::ReduceOp reduce_op = CreateReduceOpInNestedParallelLoops(
        xla_reduce_window_op, output_loop, window_loop, &rewriter);

    ConvertToReductionOperator(xla_reduce_window_op.getLoc(), reduce_op,
                               &xla_reduce_window_op.body().front(), &rewriter);
    rewriter.replaceOp(xla_reduce_window_op, llvm::None);
    return success();
  }

 private:
  std::pair<loop::ParallelOp, loop::ParallelOp>
  CreateParallelLoopsToTraverseOutputAndWindow(
      xla_lhlo::ReduceWindowOp xla_reduce_window_op,
      ConversionPatternRewriter* rewriter) const {
    auto loc = xla_reduce_window_op.getLoc();
    Value init_value =
        rewriter->create<LoadOp>(loc, xla_reduce_window_op.init_value());

    Value zero = rewriter->create<ConstantIndexOp>(loc, 0);
    Value one = rewriter->create<ConstantIndexOp>(loc, 1);

    // Create an outer parallel loop that spans the output of ReduceWindowOp.
    Value xla_output = xla_reduce_window_op.out();
    auto output_shape = xla_output.getType().cast<MemRefType>().getShape();
    SmallVector<Value, 2> parallel_lower, parallel_upper, parallel_step;
    for (auto dim : llvm::enumerate(output_shape)) {
      parallel_upper.push_back(GetStaticOrDynamicDim(
          loc, xla_output, dim.index(), dim.value(), rewriter));
      parallel_lower.push_back(zero);
      parallel_step.push_back(one);
    }
    auto output_loop = rewriter->create<loop::ParallelOp>(
        loc, parallel_lower, parallel_upper, parallel_step);

    // Create a nested loop that traverses the window.
    rewriter->setInsertionPointToStart(output_loop.getBody());
    SmallVector<Value, 2> window_lower, window_upper, window_step;
    for (const auto& window_dim : xla_reduce_window_op.window_dimensions()) {
      window_step.push_back(one);
      window_lower.push_back(zero);
      window_upper.push_back(
          rewriter->create<ConstantIndexOp>(loc, window_dim.getSExtValue()));
    }
    auto window_loop = rewriter->create<loop::ParallelOp>(
        loc, window_lower, window_upper, window_step, init_value);

    Value reduction_result = *window_loop.getResults().begin();
    auto output_ivs = output_loop.getInductionVars();
    rewriter->create<StoreOp>(
        loc, reduction_result, xla_output,
        llvm::makeArrayRef(output_ivs.begin(), output_ivs.end()));
    return std::make_pair(output_loop, window_loop);
  }

  loop::ReduceOp CreateReduceOpInNestedParallelLoops(
      xla_lhlo::ReduceWindowOp xla_reduce_window_op,
      loop::ParallelOp output_loop, loop::ParallelOp window_loop,
      ConversionPatternRewriter* rewriter) const {
    rewriter->setInsertionPointToStart(window_loop.getBody());
    auto loc = xla_reduce_window_op.getLoc();

    if (!xla_reduce_window_op.window_strides().hasValue()) {
      xla_reduce_window_op.emitOpError("No window strides specified.");
    }
    if (!xla_reduce_window_op.padding().hasValue()) {
      xla_reduce_window_op.emitOpError("No padding specified.");
    }
    if (xla_reduce_window_op.base_dilations().hasValue() ||
        xla_reduce_window_op.window_dilations().hasValue()) {
      xla_reduce_window_op.emitRemark(
          "Lowering to parallel loops does not support `base_dilations` or "
          "`window_dilations` attributes yet. The attributes will be ignored.");
    }

    Value xla_operand = xla_reduce_window_op.operand();
    auto xla_operand_type = xla_operand.getType().cast<MemRefType>();
    auto xla_operand_shape = xla_operand_type.getShape();

    auto output_ivs = llvm::to_vector<2>(output_loop.getInductionVars());
    auto window_ivs = llvm::to_vector<2>(window_loop.getInductionVars());
    auto window_strides = xla_reduce_window_op.window_strides().getValue();
    auto padding = xla_reduce_window_op.padding().getValue();

    SmallVector<Value, 2> operand_indices;
    // `in_bounds` is false when the element in the reduce window is in the
    // padding area, true otherwise.
    Value in_bounds = rewriter->create<mlir::ConstantOp>(
        loc, rewriter->getI1Type(),
        rewriter->getIntegerAttr(rewriter->getI1Type(), 1));
    for (unsigned i = 0, e = output_loop.getNumLoops(); i < e; ++i) {
      auto stride = window_strides.getValue<llvm::APInt>(i);
      auto pad_low = padding.getValue<llvm::APInt>({i, 0});

      Value stride_val =
          rewriter->create<ConstantIndexOp>(loc, stride.getSExtValue());
      Value pad_low_val =
          rewriter->create<ConstantIndexOp>(loc, pad_low.getSExtValue());

      Value center = rewriter->create<MulIOp>(loc, output_ivs[i], stride_val);
      Value offset = rewriter->create<SubIOp>(loc, window_ivs[i], pad_low_val);
      Value index = rewriter->create<AddIOp>(loc, center, offset);
      operand_indices.push_back(index);
      Value upper_bound = GetStaticOrDynamicDim(loc, xla_operand, i,
                                                xla_operand_shape[i], rewriter);
      // We must check whether 0 <= index_i < shape_i, as otherwise we are in
      // the pad and then we have to use the neutral element for reduction.
      // Equivalently, it can be computed as the unsigned comparison index_i <
      // shape_i, since a negative value wraps to a large positive value.
      in_bounds = rewriter->create<mlir::AndOp>(
          loc, in_bounds,
          rewriter->create<CmpIOp>(loc, CmpIPredicate::ult, index,
                                   upper_bound));
    }

    auto elem_or_init =
        rewriter->create<loop::IfOp>(loc, xla_operand_type.getElementType(),
                                     in_bounds, /*withElseRegion=*/true);

    OpBuilder then_builder = elem_or_init.getThenBodyBuilder();
    Value elem = then_builder.create<mlir::LoadOp>(
        loc, xla_reduce_window_op.operand(), operand_indices);
    then_builder.create<loop::YieldOp>(loc, elem);

    OpBuilder else_builder = elem_or_init.getElseBodyBuilder();
    else_builder.create<loop::YieldOp>(loc, *window_loop.initVals().begin());

    return rewriter->create<loop::ReduceOp>(loc,
                                            *elem_or_init.results().begin());
  }
};

struct LhloLegalizeToParallelLoops
    : public FunctionPass<LhloLegalizeToParallelLoops> {
  void runOnFunction() override {
    auto func = getFunction();

    OwningRewritePatternList patterns;
    patterns.insert<ReduceOpConverter, ReduceWindowOpConverter>(
        func.getContext());

    ConversionTarget target(getContext());
    target.addLegalDialect<linalg::LinalgDialect, StandardOpsDialect,
                           loop::LoopOpsDialect, XlaLhloDialect>();
    target.addIllegalOp<xla_lhlo::ReduceOp>();
    target.addIllegalOp<xla_lhlo::ReduceWindowOp>();

    if (failed(applyPartialConversion(func, target, patterns, nullptr))) {
      signalPassFailure();
    }
  }
};

}  // namespace

std::unique_ptr<OpPassBase<FuncOp>> createLegalizeLhloToParallelLoopsPass() {
  return absl::make_unique<LhloLegalizeToParallelLoops>();
}

static PassRegistration<LhloLegalizeToParallelLoops> legalize_lhlo_pass(
    "lhlo-legalize-to-parallel-loops",
    "Legalize from LHLO dialect to parallel loops.");

}  // namespace xla_lhlo
}  // namespace mlir
