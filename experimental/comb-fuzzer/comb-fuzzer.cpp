//===- tblgen-extract.cpp --------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "guide.h"

#include "GeneratorInfo.h"

#include "mlir/InitAllDialects.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"

using namespace mlir;

constexpr int MAX_WIDTH = 6;
std::string binaryOps[] = {"comb.divu", "comb.divs", "comb.modu", "comb.mods",
                           "comb.shl",  "comb.shru", "comb.shrs", "comb.sub"};
std::string variadicOps[] = {"comb.add", "comb.mul", "comb.and", "comb.or",
                             "comb.xor"};

Value addBinary(GeneratorInfo &info) {
  std::vector<Type> types;
  for (auto val : info.dominatingValues) {
    if (val.second.size() > 1) {
      types.push_back(val.first);
    }
  }

  assert(!types.empty() && "Not enough values to create a binary operation");

  auto type = types[info.chooser->choose(types.size())];
  auto op_name = binaryOps[info.chooser->choose(8)];
  auto lhs = info.dominatingValues[type][info.chooser->choose(
      info.dominatingValues[type].size())];
  auto rhs = info.dominatingValues[type][info.chooser->choose(
      info.dominatingValues[type].size())];

  if (lhs == rhs && !(op_name == "comb.shl" || op_name == "comb.shru" ||
                      op_name == "comb.shrs"))
    return nullptr;

  auto ctx = info.builder.getContext();
  auto *operation =
      info.builder.create(UnknownLoc::get(ctx), StringAttr::get(ctx, op_name),
                          {lhs, rhs}, {lhs.getType()});
  info.addDominatingValue(operation->getResult(0));
  return operation->getResult(0);
}

Value addVariadic(GeneratorInfo &info) {
  std::vector<Type> types;
  for (auto val : info.dominatingValues) {
    if (val.second.size() > 1) {
      types.push_back(val.first);
    }
  }

  assert(!types.empty() && "Not enough values to create a binary operation");

  // TODO: Handle variadic operations with more than 2 operands
  auto type = types[info.chooser->choose(types.size())];
  auto op_name = variadicOps[info.chooser->choose(5)];
  auto lhs = info.dominatingValues[type][info.chooser->choose(
      info.dominatingValues[type].size())];
  auto rhs = info.dominatingValues[type][info.chooser->choose(
      info.dominatingValues[type].size())];

  if (lhs == rhs &&
      !(op_name == "comb.and" || op_name == "comb.or" || op_name == "comb.xor"))
    return nullptr;

  auto ctx = info.builder.getContext();
  auto *operation =
      info.builder.create(UnknownLoc::get(ctx), StringAttr::get(ctx, op_name),
                          {lhs, rhs}, {lhs.getType()});
  info.addDominatingValue(operation->getResult(0));
  return operation->getResult(0);
}

/// Add an operation
Value addOperation(GeneratorInfo &info) {
  bool canUseBinary = false;
  for (auto val : info.dominatingValues) {
    if (val.second.size() > 1) {
      canUseBinary = true;
      break;
    }
  }

  bool hasi1 = info.dominatingValues.find(
                   IntegerType::get(info.builder.getContext(), 1)) !=
               info.dominatingValues.end();

  if (info.chooser->choose(2)) {
    return addBinary(info);
  } else {
    return addVariadic(info);
  }
  // assert(false && "Not implemented");
}

OwningOpRef<ModuleOp> createProgram(MLIRContext &ctx,
                                    tree_guide::Chooser *chooser, int fuel) {
  // Create an empty module.
  auto unknownLoc = UnknownLoc::get(&ctx);
  OwningOpRef<ModuleOp> module(ModuleOp::create(unknownLoc));

  // Create the builder, and set its insertion point in the module.
  OpBuilder builder(&ctx);
  auto &moduleBlock = module->getRegion().getBlocks().front();
  builder.setInsertionPoint(&moduleBlock, moduleBlock.begin());

  // Create an empty function, and set the insertion point in it.
  auto func = builder.create<func::FuncOp>(unknownLoc, "foo",
                                           FunctionType::get(&ctx, {}, {}));
  func.setPrivate();
  auto &funcBlock = func.getBody().emplaceBlock();
  builder.setInsertionPoint(&funcBlock, funcBlock.begin());

  GeneratorInfo info(chooser, {}, builder,
                     ctx.getOrLoadDialect<irdl::IRDLDialect>()->irdlContext);

  // Add arguments to the function.
  func.insertArgument(0, IntegerType::get(&ctx, 1), {},
                      mlir::UnknownLoc::get(builder.getContext()));
  info.addDominatingValue(func.getArgument(0));

  // Add arguments to the function.
  func.insertArgument(1, IntegerType::get(&ctx, 1), {},
                      mlir::UnknownLoc::get(builder.getContext()));
  info.addDominatingValue(func.getArgument(1));

  // Add arguments to the function.
  func.insertArgument(2, IntegerType::get(&ctx, 1), {},
                      mlir::UnknownLoc::get(builder.getContext()));
  info.addDominatingValue(func.getArgument(2));

  for (int i = 0; i < fuel - 1; i++) {
    auto value = addOperation(info);
    if (value == nullptr)
      return nullptr;
  }
  auto value = addOperation(info);
  for (auto type : info.dominatingValues) {
    for (auto val : type.second) {
      if (val == value)
        continue;
      if (val.getUses().empty())
        return nullptr;
    }
  }

  builder.create<func::ReturnOp>(unknownLoc);
  return module;
}

int main(int argc, char **argv) {

  // The IRDL file containing the dialects that we want to generate
  static llvm::cl::opt<std::string> outputFolder(
      "o", llvm::cl::desc("Output folder"), llvm::cl::init("-"));

  llvm::InitLLVM y(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "MLIR enumerator");

  MLIRContext ctx;
  ctx.allowUnregisteredDialects();

  // Register all dialects
  DialectRegistry registry;
  registerAllDialects(registry);
  ctx.appendDialectRegistry(registry);
  ctx.loadAllAvailableDialects();

  auto guide = tree_guide::BFSGuide(42);

  int n = 0;
  int nPassed = 0;
  while (auto chooser = guide.makeChooser()) {
    auto module = createProgram(ctx, chooser.get(), 3);
    n++;
    if (!module)
      continue;
    nPassed++;
    module->print(llvm::outs());
    llvm::errs() << "Printed " << nPassed << "modules"
                 << " over " << n << "total\n";
  }

  llvm::errs() << n << " modules generated\n";

  return 0;
}