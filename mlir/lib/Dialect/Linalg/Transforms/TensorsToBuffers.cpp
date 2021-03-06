//===- TensorsToBuffers.cpp - Transformation from tensors to buffers ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the conversion from tensors to buffers on Linalg
// operations.
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Operation.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/BufferPlacement.h"

using namespace mlir;
using ReturnOpConverter =
    NonVoidToVoidReturnOpConverter<mlir::ReturnOp, mlir::ReturnOp,
                                   linalg::CopyOp>;

namespace {
/// A pattern to convert Generic Linalg operations which work on tensors to
/// use buffers. A buffer is allocated using BufferAssignmentPlacer for
/// each operation result. BufferPlacement pass should be later used to move
/// Alloc operations to the correct positions and insert the missing Dealloc
/// operations in the correct places.
class GenericOpConverter
    : public BufferAssignmentOpConversionPattern<linalg::GenericOp> {
public:
  using BufferAssignmentOpConversionPattern<
      linalg::GenericOp>::BufferAssignmentOpConversionPattern;

  LogicalResult
  matchAndRewrite(linalg::GenericOp op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const final {
    Location loc = op.getLoc();
    ResultRange results = op.getOperation()->getResults();
    SmallVector<Value, 2> newArgs, newResults;
    newArgs.reserve(operands.size() + results.size());
    newArgs.append(operands.begin(), operands.end());
    newResults.reserve(results.size());

    // Update all types to memref types.
    for (auto result : results) {
      auto type = result.getType().cast<ShapedType>();
      assert(type && "tensor to buffer conversion expects ranked results");
      if (!type.hasStaticShape())
        return rewriter.notifyMatchFailure(
            op, "dynamic shapes not currently supported");
      auto memrefType = MemRefType::get(type.getShape(), type.getElementType());

      // Compute alloc position and insert a custom allocation node.
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.restoreInsertionPoint(
          bufferAssignment->computeAllocPosition(result));
      auto alloc = rewriter.create<AllocOp>(loc, memrefType);
      newArgs.push_back(alloc);
      newResults.push_back(alloc);
    }

    // Generate a new linalg operation that works on buffers.
    auto linalgOp = rewriter.create<linalg::GenericOp>(
        loc, llvm::None, newArgs, rewriter.getI64IntegerAttr(operands.size()),
        rewriter.getI64IntegerAttr(results.size()), op.indexing_maps(),
        op.iterator_types(), op.docAttr(), op.library_callAttr());

    // Create a new block in the region of the new Generic Op.
    Block &oldBlock = op.getRegion().front();
    Region &newRegion = linalgOp.region();
    Block *newBlock = rewriter.createBlock(&newRegion, newRegion.begin(),
                                           oldBlock.getArgumentTypes());

    // Add the result arguments to the new block.
    for (auto result : newResults)
      newBlock->addArgument(
          result.getType().cast<ShapedType>().getElementType());

    // Clone the body of the old block to the new block.
    BlockAndValueMapping mapping;
    for (unsigned i = 0; i < oldBlock.getNumArguments(); i++)
      mapping.map(oldBlock.getArgument(i), newBlock->getArgument(i));
    rewriter.setInsertionPointToEnd(newBlock);
    for (auto &op : oldBlock.getOperations()) {
      Operation *clonedOp = rewriter.clone(op, mapping);
      mapping.map(op.getResults(), clonedOp->getResults());
    }

    // Replace the results of the old Generic Op with the results of the new
    // one.
    rewriter.replaceOp(op, newResults);
    return success();
  }
};

/// Populate the given list with patterns to convert Linalg operations on
/// tensors to buffers.
static void populateConvertLinalgOnTensorsToBuffersPattern(
    MLIRContext *context, BufferAssignmentPlacer *placer,
    TypeConverter *converter, OwningRewritePatternList *patterns) {
  // clang-format off
  patterns->insert<FunctionAndBlockSignatureConverter,
                   GenericOpConverter,
                   ReturnOpConverter>(context, placer, converter);
  // clang-format on
}

/// Converts Linalg operations that work on tensor-type operands or results to
/// work on buffers.
struct ConvertLinalgOnTensorsToBuffers
    : public LinalgOnTensorsToBuffersBase<ConvertLinalgOnTensorsToBuffers> {
  void runOnOperation() override {
    MLIRContext &context = getContext();
    ConversionTarget target(context);
    BufferAssignmentTypeConverter converter;

    // Mark all Standard operations legal.
    target.addLegalDialect<StandardOpsDialect>();

    // Mark all Linalg operations illegal as long as they work on tensors.
    auto isIllegalType = [&](Type type) { return !converter.isLegal(type); };
    auto isLegalOperation = [&](Operation *op) {
      return llvm::none_of(op->getOperandTypes(), isIllegalType) &&
             llvm::none_of(op->getResultTypes(), isIllegalType);
    };
    target.addDynamicallyLegalDialect<linalg::LinalgDialect>(
        Optional<ConversionTarget::DynamicLegalityCallbackFn>(
            isLegalOperation));

    // TODO: Considering the following dynamic legality checks, the current
    // implementation of FunctionAndBlockSignatureConverter of Buffer Assignment
    // will convert the function signature incorrectly. This converter moves
    // all the return values of the function to the input argument list without
    // considering the return value types and creates a void function. However,
    // the NonVoidToVoidReturnOpConverter doesn't change the return operation if
    // its operands are not tensors. The following example leaves the IR in a
    // broken state.
    //
    // @function(%arg0: f32, %arg1: tensor<4xf32>) -> (f32, f32) {
    //    %0 = mulf %arg0, %arg0 : f32
    //    return %0, %0 : f32, f32
    // }
    //
    // broken IR after conversion:
    //
    // func @function(%arg0: f32, %arg1: memref<4xf32>, f32, f32) {
    //    %0 = mulf %arg0, %arg0 : f32
    //    return %0, %0 : f32, f32
    // }
    //
    // This issue must be fixed in FunctionAndBlockSignatureConverter and
    // NonVoidToVoidReturnOpConverter.

    // Mark Standard Return operations illegal as long as one operand is tensor.
    target.addDynamicallyLegalOp<mlir::ReturnOp>([&](mlir::ReturnOp returnOp) {
      return llvm::none_of(returnOp.getOperandTypes(), isIllegalType);
    });

    // Mark the function operation illegal as long as an argument is tensor.
    target.addDynamicallyLegalOp<FuncOp>([&](FuncOp funcOp) {
      return converter.isSignatureLegal(funcOp.getType()) &&
             llvm::none_of(funcOp.getType().getResults(),
                           [&](Type type) { return type.isa<MemRefType>(); });
    });

    // Walk over all the functions to apply buffer assignment.
    getOperation().walk([&](FuncOp function) {
      OwningRewritePatternList patterns;
      BufferAssignmentPlacer placer(function);
      populateConvertLinalgOnTensorsToBuffersPattern(&context, &placer,
                                                     &converter, &patterns);

      // Applying full conversion
      return WalkResult(
          applyFullConversion(function, target, patterns, &converter));
    });
  }
};
} // end anonymous namespace

std::unique_ptr<OperationPass<ModuleOp>>
mlir::createConvertLinalgOnTensorsToBuffersPass() {
  return std::make_unique<ConvertLinalgOnTensorsToBuffers>();
}
