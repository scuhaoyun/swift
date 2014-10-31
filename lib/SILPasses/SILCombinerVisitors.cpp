//===---- SILCombinerVisitors.cpp - SILCombiner Visitor Impl -*- C++ -*----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-combine"
#include "SILCombiner.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILAnalysis/ValueTracking.h"
#include "swift/SILPasses/Utils/Local.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"

using namespace swift;
using namespace swift::PatternMatch;

SILInstruction *SILCombiner::visitStructExtractInst(StructExtractInst *SEI) {
  // If our operand has archetypes or our field is not trivial, do not do
  // anything.
  SILValue Op = SEI->getOperand();
  SILType OpType = Op.getType();
  if (OpType.hasArchetype() || OpType.isTrivial(SEI->getModule()))
    return nullptr;

  // (struct_extract (unchecked_ref_bit_cast X->Y x) #z)
  //    ->
  // (unchecked_ref_bit_cast X->Z x)
  //
  // Where #z is a Z typed field of single field struct Y.
  auto *URBCI = dyn_cast<UncheckedRefBitCastInst>(Op);
  if (!URBCI)
    return nullptr;

  // If we only have one stored property, then we are layout compatible with
  // that property and can perform the operation.
  StructDecl *S = SEI->getStructDecl();
  auto R = S->getStoredProperties();
  auto B = R.begin();
  if (B == R.end())
    return nullptr;
  ++B;
  if (B != R.end())
    return nullptr;

  return new (SEI->getModule()) UncheckedRefBitCastInst(SEI->getLoc(),
                                                        URBCI->getOperand(),
                                                        SEI->getType());
}

static bool isFirstPayloadedCase(EnumDecl *E, EnumElementDecl *Elt) {
  for (EnumElementDecl *Iter : E->getAllElements())
    if (Iter->hasArgumentType())
      return Iter == Elt;
  return false;
}

SILInstruction *
SILCombiner::
visitUncheckedEnumDataInst(UncheckedEnumDataInst *UEDI) {
  // First to be safe, do not perform this optimization on unchecked_enum_data
  // on bounded generic nominal types.
  SILValue Op = UEDI->getOperand();
  SILType OpType = Op.getType();
  if (OpType.hasArchetype() || OpType.isTrivial(UEDI->getModule()))
    return nullptr;

  // (unchecked_enum_data (unchecked_ref_bit_cast X->Y x) #z)
  //    ->
  // (unchecked_ref_bit_cast X->Z x)
  //
  // Where #z is the payload of type Z of the first payloaded case of the enum
  // Y.
  auto *URBCI = dyn_cast<UncheckedRefBitCastInst>(Op);
  if (!URBCI)
    return nullptr;

  // A UEDI performs a layout compatible operation if it is extracting the first
  // argument case of the enum.
  EnumDecl *E = OpType.getEnumOrBoundGenericEnum();
  if (!isFirstPayloadedCase(E, UEDI->getElement()))
    return nullptr;

  return new (UEDI->getModule()) UncheckedRefBitCastInst(UEDI->getLoc(),
                                                         URBCI->getOperand(),
                                                         UEDI->getType());
}

SILInstruction *SILCombiner::visitSwitchEnumAddrInst(SwitchEnumAddrInst *SEAI) {
  // Promote switch_enum_addr to switch_enum if the enum is loadable.
  //   switch_enum_addr %ptr : $*Optional<SomeClass>, case ...
  //     ->
  //   %value = load %ptr
  //   switch_enum %value
  SILType Ty = SEAI->getOperand().getType();
  if (!Ty.isLoadable(SEAI->getModule()))
    return nullptr;

  SmallVector<std::pair<EnumElementDecl*, SILBasicBlock*>, 8> Cases;
  for (int i = 0, e = SEAI->getNumCases(); i < e; ++i)
    Cases.push_back(SEAI->getCase(i));


  SILBasicBlock *Default = SEAI->hasDefault() ? SEAI->getDefaultBB() : 0;
  LoadInst *EnumVal = Builder->createLoad(SEAI->getLoc(), SEAI->getOperand());
  EnumVal->setDebugScope(SEAI->getDebugScope());
  Builder->createSwitchEnum(SEAI->getLoc(), EnumVal, Default, Cases)
    ->setDebugScope(SEAI->getDebugScope());
  return eraseInstFromFunction(*SEAI);
}

SILInstruction *SILCombiner::visitSelectEnumAddrInst(SelectEnumAddrInst *SEAI) {
  // Promote select_enum_addr to select_enum if the enum is loadable.
  //   = select_enum_addr %ptr : $*Optional<SomeClass>, case ...
  //     ->
  //   %value = load %ptr
  //   = select_enum %value
  SILType Ty = SEAI->getEnumOperand().getType();
  if (!Ty.isLoadable(SEAI->getModule()))
    return nullptr;

  SmallVector<std::pair<EnumElementDecl*, SILValue>, 8> Cases;
  for (int i = 0, e = SEAI->getNumCases(); i < e; ++i)
    Cases.push_back(SEAI->getCase(i));

  SILValue Default = SEAI->hasDefault() ? SEAI->getDefaultResult() : SILValue();
  LoadInst *EnumVal = Builder->createLoad(SEAI->getLoc(),
                                          SEAI->getEnumOperand());
  EnumVal->setDebugScope(SEAI->getDebugScope());
  auto *I = SelectEnumInst::create(SEAI->getLoc(), EnumVal, SEAI->getType(),
                                   Default, Cases,
                                   *SEAI->getParent()->getParent());
  I->setDebugScope(SEAI->getDebugScope());
  return I;
}

SILInstruction *SILCombiner::visitAllocStackInst(AllocStackInst *AS) {
  // init_existential instructions behave like memory allocation within
  // the allocated object. We can promote the init_existential allocation
  // into a dedicated allocation.

  // Detect this pattern
  // %0 = alloc_stack $LogicValue
  // %1 = init_existential %0#1 : $*LogicValue, $*Bool
  // ...
  // use of %1
  // ...
  // destroy_addr %0#1 : $*LogicValue
  // dealloc_stack %0#0 : $*@local_storage LogicValue
  bool LegalUsers = true;
  InitExistentialInst *IEI = nullptr;
  // Scan all of the uses of the AllocStack and check if it is not used for
  // anything other than the init_existential container.
  for (Operand *Op: AS->getUses()) {
    // Destroy and dealloc are both fine.
    if (isa<DestroyAddrInst>(Op->getUser()) ||
        isa<DeallocStackInst>(Op->getUser()))
      continue;

    // Make sure there is exactly one init_existential.
    if (auto *I = dyn_cast<InitExistentialInst>(Op->getUser())) {
      if (IEI) {
        LegalUsers = false;
        break;
      }
      IEI = I;
      continue;
    }

    // All other instructions are illegal.
    LegalUsers = false;
    break;
  }

  // Save the original insertion point.
  auto OrigInsertionPoint = Builder->getInsertionPoint();

  // If the only users of the alloc_stack are alloc, destroy and
  // init_existential then we can promote the allocation of the init
  // existential.
  if (LegalUsers && IEI) {
    auto *ConcAlloc = Builder->createAllocStack(AS->getLoc(),
                                                IEI->getLoweredConcreteType());
    ConcAlloc->setDebugScope(AS->getDebugScope());
    SILValue(IEI, 0).replaceAllUsesWith(ConcAlloc->getAddressResult());
    eraseInstFromFunction(*IEI);


    for (Operand *Op: AS->getUses()) {
      if (auto *DA = dyn_cast<DestroyAddrInst>(Op->getUser())) {
        Builder->setInsertionPoint(DA);
        Builder->createDestroyAddr(DA->getLoc(), SILValue(ConcAlloc, 1))
          ->setDebugScope(DA->getDebugScope());
        eraseInstFromFunction(*DA);

      }
      if (auto *DS = dyn_cast<DeallocStackInst>(Op->getUser())) {
        Builder->setInsertionPoint(DS);
        Builder->createDeallocStack(DS->getLoc(), SILValue(ConcAlloc, 0))
          ->setDebugScope(DS->getDebugScope());
        eraseInstFromFunction(*DS);
      }
    }

    eraseInstFromFunction(*AS);
    // Restore the insertion point.
    Builder->setInsertionPoint(OrigInsertionPoint);
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitLoadInst(LoadInst *LI) {
  // (load (upcast-ptr %x)) -> (upcast-ref (load %x))
  if (auto *UI = dyn_cast<UpcastInst>(LI->getOperand())) {
    auto NewLI = Builder->createLoad(LI->getLoc(), UI->getOperand());
    NewLI->setDebugScope(LI->getDebugScope());
    return new (UI->getModule()) UpcastInst(LI->getLoc(), NewLI,
                                            LI->getType());
  }

  // Given a load with multiple struct_extracts/tuple_extracts and no other
  // uses, canonicalize the load into several (struct_element_addr (load))
  // pairs.
  using ProjInstPairTy = std::pair<Projection, SILInstruction *>;

  // Go through the loads uses and add any users that are projections to the
  // projection list.
  llvm::SmallVector<ProjInstPairTy, 8> Projections;
  for (auto *UI : LI->getUses()) {
    if (auto *SEI = dyn_cast<StructExtractInst>(UI->getUser())) {
      Projections.push_back({Projection(SEI), SEI});
      continue;
    }

    if (auto *TEI = dyn_cast<TupleExtractInst>(UI->getUser())) {
      Projections.push_back({Projection(TEI), TEI});
      continue;
    }

    // If we have any non SEI, TEI instruction, don't do anything here.
    return nullptr;
  }

  // Sort the list.
  std::sort(Projections.begin(), Projections.end());

  // Go through our sorted list creating new GEPs only when we need to.
  Projection *LastProj = nullptr;
  LoadInst *LastNewLoad = nullptr;
  for (auto &Pair : Projections) {
    auto &Proj = Pair.first;
    auto *Inst = Pair.second;

    // If this projection is the same as the last projection we processed, just
    // replace all uses of the projection with the load we created previously.
    if (LastProj && Proj == *LastProj) {
      replaceInstUsesWith(*Inst, LastNewLoad, 0);
      eraseInstFromFunction(*Inst);
      continue;
    }

    // Ok, we have started to visit the range of instructions associated with
    // a new projection. If we have a VarDecl, create a struct_element_addr +
    // load. Make sure to update LastProj, LastNewLoad.
    if (ValueDecl *V = Proj.getDecl()) {
      assert(isa<StructExtractInst>(Inst) && "A projection with a VarDecl "
             "should be associated with a struct_extract.");

      LastProj = &Proj;
      auto *SEA =
        Builder->createStructElementAddr(LI->getLoc(), LI->getOperand(),
                                         cast<VarDecl>(V),
                                         Inst->getType(0).getAddressType());
      SEA->setDebugScope(LI->getDebugScope());
      LastNewLoad = Builder->createLoad(LI->getLoc(), SEA);
      LastNewLoad->setDebugScope(LI->getDebugScope());
      replaceInstUsesWith(*Inst, LastNewLoad, 0);
      eraseInstFromFunction(*Inst);
      continue;
    }

    // If we have an index, then create a new tuple_element_addr + load.
    assert(isa<TupleExtractInst>(Inst) && "A projection with an integer "
           "should be associated with a tuple_extract.");

    LastProj = &Proj;
    auto *TEA =
      Builder->createTupleElementAddr(LI->getLoc(), LI->getOperand(),
                                      Proj.getIndex(),
                                      Inst->getType(0).getAddressType());
    TEA->setDebugScope(LI->getDebugScope());
    LastNewLoad = Builder->createLoad(LI->getLoc(), TEA);
    LastNewLoad->setDebugScope(LI->getDebugScope());
    replaceInstUsesWith(*Inst, LastNewLoad, 0);
    eraseInstFromFunction(*Inst);
  }

  // Erase the old load.
  return eraseInstFromFunction(*LI);
}

SILInstruction *SILCombiner::visitReleaseValueInst(ReleaseValueInst *RVI) {
  SILValue Operand = RVI->getOperand();
  SILType OperandTy = Operand.getType();

  // Destroy value of an enum with a trivial payload or no-payload is a no-op.
  if (auto *EI = dyn_cast<EnumInst>(Operand)) {
    if (!EI->hasOperand() ||
        EI->getOperand().getType().isTrivial(EI->getModule()))
      return eraseInstFromFunction(*RVI);

    // retain_value of an enum_inst where we know that it has a payload can be
    // reduced to a retain_value on the payload.
    if (EI->hasOperand()) {
      return new (RVI->getModule()) ReleaseValueInst(RVI->getLoc(),
                                                     EI->getOperand());
    }
  }

  // ReleaseValueInst of a reference type is a strong_release.
  if (OperandTy.hasReferenceSemantics())
    return new (RVI->getModule()) StrongReleaseInst(RVI->getLoc(), Operand);

  // ReleaseValueInst of a trivial type is a no-op.
  if (OperandTy.isTrivial(RVI->getModule()))
    return eraseInstFromFunction(*RVI);

  // Do nothing for non-trivial non-reference types.
  return nullptr;
}

SILInstruction *SILCombiner::visitRetainValueInst(RetainValueInst *RVI) {
  SILValue Operand = RVI->getOperand();
  SILType OperandTy = Operand.getType();

  // retain_value of an enum with a trivial payload or no-payload is a no-op +
  // RAUW.
  if (auto *EI = dyn_cast<EnumInst>(Operand)) {
    if (!EI->hasOperand() ||
        EI->getOperand().getType().isTrivial(RVI->getModule())) {
      return eraseInstFromFunction(*RVI);
    }

    // retain_value of an enum_inst where we know that it has a payload can be
    // reduced to a retain_value on the payload.
    if (EI->hasOperand()) {
      return new (RVI->getModule()) RetainValueInst(RVI->getLoc(),
                                                    EI->getOperand());
    }
  }

  // RetainValueInst of a reference type is a strong_release.
  if (OperandTy.hasReferenceSemantics()) {
    return new (RVI->getModule()) StrongRetainInst(RVI->getLoc(), Operand);
  }

  // RetainValueInst of a trivial type is a no-op + use propogation.
  if (OperandTy.isTrivial(RVI->getModule())) {
    return eraseInstFromFunction(*RVI);
  }

  // Sometimes in the stdlib due to hand offs, we will see code like:
  //
  // release_value %0
  // retain_value %0
  //
  // with the matching retain_value to the release_value in a predecessor basic
  // block and the matching release_value for the retain_value_retain in a
  // successor basic block.
  //
  // Due to the matching pairs being in different basic blocks, the ARC
  // Optimizer (which is currently local to one basic block does not handle
  // it). But that does not mean that we can not eliminate this pair with a
  // peephole.

  // If we are not the first instruction in this basic block...
  if (RVI != &*RVI->getParent()->begin()) {
    SILBasicBlock::iterator Pred = RVI;
    --Pred;

    // ...and the predecessor instruction is a release_value on the same value
    // as our retain_value...
    if (ReleaseValueInst *Release = dyn_cast<ReleaseValueInst>(&*Pred))
      // Remove them...
      if (Release->getOperand() == RVI->getOperand()) {
        eraseInstFromFunction(*Release);
        return eraseInstFromFunction(*RVI);
      }
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitPartialApplyInst(PartialApplyInst *PAI) {
  // partial_apply without any substitutions or arguments is just a
  // thin_to_thick_function.
  if (!PAI->hasSubstitutions() && (PAI->getNumArguments() == 0))
    return new (PAI->getModule()) ThinToThickFunctionInst(PAI->getLoc(),
                                                          PAI->getCallee(),
                                                          PAI->getType());

  // Delete dead closures of this form:
  //
  // %X = partial_apply %x(...)    // has 1 use.
  // strong_release %X;

  // Only handle PartialApplyInst with one use.
  if (!PAI->hasOneUse())
    return nullptr;

  SILLocation Loc = PAI->getLoc();

  // The single user must be the StrongReleaseInst.
  if (auto *SRI = dyn_cast<StrongReleaseInst>(PAI->use_begin()->getUser())) {
    SILFunctionType *ClosureTy =
      dyn_cast<SILFunctionType>(PAI->getCallee().getType().getSwiftType());
    if (!ClosureTy)
      return nullptr;

    // Emit a destroy value for each captured closure argument.
    auto Params = ClosureTy->getParameters();
    auto Args = PAI->getArguments();
    unsigned Delta = Params.size() - Args.size();
    assert(Delta <= Params.size() && "Error, more Args to partial apply than "
           "params in its interface.");

    // Set the insertion point of the release_value to be that of the release,
    // which is the end of the lifetime of the partial_apply.
    auto OrigInsertPoint = Builder->getInsertionPoint();
    SILInstruction *SingleUser = PAI->use_begin()->getUser();
    Builder->setInsertionPoint(SingleUser);

    for (unsigned AI = 0, AE = Args.size(); AI != AE; ++AI) {
      SILValue Arg = Args[AI];
      auto Param = Params[AI + Delta];

      if (!Param.isIndirect() && Param.isConsumed())
        if (!Arg.getType().isAddress())
          Builder->createReleaseValue(Loc, Arg)
            ->setDebugScope(PAI->getDebugScope());
    }

    Builder->setInsertionPoint(OrigInsertPoint);

    // Delete the strong_release.
    eraseInstFromFunction(*SRI);
    // Delete the partial_apply.
    return eraseInstFromFunction(*PAI);
  }
  return nullptr;
}

SILInstruction *
SILCombiner::optimizeApplyOfPartialApply(ApplyInst *AI, PartialApplyInst *PAI) {
  // Don't handle generic applys.
  if (AI->hasSubstitutions())
    return nullptr;

  // Make sure that the substitution list of the PAI does not contain any
  // archetypes.
  ArrayRef<Substitution> Subs = PAI->getSubstitutions();
  for (Substitution S : Subs)
    if (S.getReplacement()->getCanonicalType()->hasArchetype())
      return nullptr;

  FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(PAI->getCallee());
  if (!FRI)
    return nullptr;

  // Prepare the args.
  SmallVector<SILValue, 8> Args;
  // First the ApplyInst args.
  for (auto Op : AI->getArguments())
    Args.push_back(Op);
  // Next, the partial apply args.
  for (auto Op : PAI->getArguments())
    Args.push_back(Op);

  // The thunk that implements the partial apply calls the closure function
  // that expects all arguments to be consumed by the function. However, the
  // captured arguments are not arguments of *this* apply, so they are not
  // pre-incremented. When we combine the partial_apply and this apply into
  // a new apply we need to retain all of the closure non-address type
  // arguments.
  for (auto Arg : PAI->getArguments())
    if (!Arg.getType().isAddress())
      Builder->emitRetainValueOperation(PAI->getLoc(), Arg);

  SILFunction *F = FRI->getReferencedFunction();
  SILType FnType = F->getLoweredType();
  SILType ResultTy = F->getLoweredFunctionType()->getSILResult();
  if (!Subs.empty()) {
    FnType = FnType.substGenericArgs(PAI->getModule(), Subs);
    ResultTy = FnType.getAs<SILFunctionType>()->getSILResult();
  }

  ApplyInst *NAI = Builder->createApply(AI->getLoc(), FRI, FnType, ResultTy,
                                        Subs, Args, AI->isTransparent());
  NAI->setDebugScope(AI->getDebugScope());

  // We also need to release the partial_apply instruction itself because it
  // is consumed by the apply_instruction.
  Builder->createStrongRelease(AI->getLoc(), PAI)
    ->setDebugScope(AI->getDebugScope());

  replaceInstUsesWith(*AI, NAI);
  return eraseInstFromFunction(*AI);
}

SILInstruction *SILCombiner::optimizeBuiltinCanBeObjCClass(BuiltinInst *BI) {
  assert(BI->hasSubstitutions() && "Expected substitutions for canBeClass");

  auto const &Subs = BI->getSubstitutions();
  assert((Subs.size() == 1) &&
         "Expected one substitution in call to canBeClass");

  auto Ty = Subs[0].getReplacement()->getCanonicalType();
  switch (Ty->canBeClass()) {
  case TypeTraitResult::IsNot:
    return IntegerLiteralInst::create(BI->getLoc(), BI->getType(),
                                      APInt(8, 0), *BI->getFunction());
  case TypeTraitResult::Is:
    return IntegerLiteralInst::create(BI->getLoc(), BI->getType(),
                                      APInt(8, 1), *BI->getFunction());
  case TypeTraitResult::CanBe:
    return nullptr;
  }
}

SILInstruction *SILCombiner::optimizeBuiltinCompareEq(BuiltinInst *BI,
                                                      bool NegateResult) {
  IsZeroKind LHS = isZeroValue(BI->getArguments()[0]);
  IsZeroKind RHS = isZeroValue(BI->getArguments()[1]);

  // Can't handle unknown values.
  if (LHS == IsZeroKind::Unknown || RHS == IsZeroKind::Unknown)
    return nullptr;

  // Can't handle non-zero ptr values.
  if (LHS == IsZeroKind::NotZero && RHS == IsZeroKind::NotZero)
    return nullptr;

  // Set to true if both sides are zero. Set to false if only one side is zero.
  bool Val = (LHS == RHS) ^ NegateResult;

  return IntegerLiteralInst::create(BI->getLoc(), BI->getType(), APInt(1, Val),
                                    *BI->getFunction());
}

SILInstruction *
SILCombiner::optimizeApplyOfConvertFunctionInst(ApplyInst *AI,
                                                ConvertFunctionInst *CFI) {
  // We only handle simplification of static function references. If we don't
  // have one, bail.
  FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(CFI->getOperand());
  if (!FRI)
    return nullptr;

  // Grab our relevant callee types...
  CanSILFunctionType SubstCalleeTy = AI->getSubstCalleeType();
  auto ConvertCalleeTy =
      CFI->getOperand().getType().castTo<SILFunctionType>();

  // ... and make sure they have no unsubstituted generics. If they do, bail.
  if (SubstCalleeTy->hasArchetype() || ConvertCalleeTy->hasArchetype())
    return nullptr;

  // Ok, we can now perform our transformation. Grab AI's operands and the
  // relevant types from the ConvertFunction function type and AI.
  OperandValueArrayRef Ops = AI->getArgumentsWithoutIndirectResult();
  auto OldOpTypes = SubstCalleeTy->getParameterSILTypes();
  auto NewOpTypes = ConvertCalleeTy->getParameterSILTypes();

  assert(Ops.size() == OldOpTypes.size() &&
         "Ops and op types must have same size.");
  assert(Ops.size() == NewOpTypes.size() &&
         "Ops and op types must have same size.");

  llvm::SmallVector<SILValue, 8> Args;
  for (unsigned i = 0, e = Ops.size(); i != e; ++i) {
    SILValue Op = Ops[i];
    SILType OldOpType = OldOpTypes[i];
    SILType NewOpType = NewOpTypes[i];

    // Convert function takes refs to refs, address to addresses, and leaves
    // other types alone.
    if (OldOpType.isAddress()) {
      assert(NewOpType.isAddress() && "Addresses should map to addresses.");
      auto UAC = Builder->createUncheckedAddrCast(AI->getLoc(), Op, NewOpType);
      UAC->setDebugScope(AI->getDebugScope());
      Args.push_back(UAC);
    } else if (OldOpType.isHeapObjectReferenceType()) {
      assert(NewOpType.isHeapObjectReferenceType() &&
             "refs should map to refs.");
      auto URC = Builder->createUncheckedRefCast(AI->getLoc(), Op, NewOpType);
      URC->setDebugScope(AI->getDebugScope());
      Args.push_back(URC);
    } else {
      Args.push_back(Op);
    }
  }

  SILType CCSILTy = SILType::getPrimitiveObjectType(ConvertCalleeTy);
  // Create the new apply inst.
  auto NAI = ApplyInst::create(AI->getLoc(), FRI, CCSILTy,
                               ConvertCalleeTy->getSILResult(),
                               ArrayRef<Substitution>(), Args, false,
                               *FRI->getReferencedFunction());
  NAI->setDebugScope(AI->getDebugScope());
  return NAI;
}

typedef SmallVector<SILInstruction*, 4> UserListTy;
/// \brief Returns a list of instructions that project or perform reference
/// counting operations on the instruction or its uses in argument \p Inst.
/// The function returns False if there are non-ARC instructions.
static bool recursivelyCollectARCUsers(UserListTy &Uses, SILInstruction *Inst) {
  Uses.push_back(Inst);
  for (auto Inst : Inst->getUses()) {
    if (isa<RefCountingInst>(Inst->getUser()) ||
        isa<DebugValueInst>(Inst->getUser())) {
      Uses.push_back(Inst->getUser());
      continue;
    }
    if (auto SI = dyn_cast<StructExtractInst>(Inst->getUser()))
      if (recursivelyCollectARCUsers(Uses, SI))
        continue;

    return false;
  }

  return true;
}

/// This is a helper class that performs optimization of string literals
/// concatenation.
class StringConcatenationOptimizer {
  /// Apply instruction being optimized.
  ApplyInst *AI;
  /// Builder to be used for creation of new instructions.
  SILBuilder *Builder;
  /// Left string literal operand of a string concatenation.
  StringLiteralInst *SLILeft = nullptr;
  /// Right string literal operand of a string concatenation.
  StringLiteralInst *SLIRight = nullptr;
  /// Function used to construct the left string literal.
  FunctionRefInst *FRILeft = nullptr;
  /// Function used to construct the right string literal.
  FunctionRefInst *FRIRight = nullptr;
  /// Apply instructions used to construct left string literal.
  ApplyInst *AILeft = nullptr;
  /// Apply instructions used to construct right string literal.
  ApplyInst *AIRight = nullptr;
  /// String literal conversion function to be used.
  FunctionRefInst *FRIConvertFromBuiltin = nullptr;
  /// Set if a String literal conversion function to be used is transparent.
  bool IsTransparent = false;
  /// Result type of a function producing the concatenated string literal.
  SILValue FuncResultType;

  /// Internal helper methods
  bool extractStringConcatOperands();
  void adjustEncodings();
  APInt getConcatenatedLength();
  bool isAscii() const;

public:
  StringConcatenationOptimizer(ApplyInst *AI, SILBuilder *Builder): AI(AI),
    Builder(Builder) { }

  /// Tries to optimize a given apply instruction if it is a
  /// concatenation of string literals.
  ///
  /// Returns a new instruction if optimization was possible.
  SILInstruction *optimize();
};

/// Checks operands of a string concatenation operation to see if
/// optimization is applicable.
///
/// Returns false if optimization is not possible.
/// Returns true and initializes internal fields if optimization is possible.
bool StringConcatenationOptimizer::extractStringConcatOperands() {
  auto *FRI = dyn_cast<FunctionRefInst>(AI->getCallee());
  if (!FRI)
    return false;

  auto *FRIFun = FRI->getReferencedFunction();

  if (AI->getNumOperands() != 3 ||
      !FRIFun->hasSemanticsString("string.concat"))
    return false;

  // Left and right operands of a string concatenation operation.
  AILeft = dyn_cast<ApplyInst>(AI->getOperand(1));
  AIRight = dyn_cast<ApplyInst>(AI->getOperand(2));

  if (!AILeft || !AIRight)
    return false;

  FRILeft = dyn_cast<FunctionRefInst>(AILeft->getCallee());
  FRIRight = dyn_cast<FunctionRefInst>(AIRight->getCallee());

  if (!FRILeft || !FRIRight)
    return false;

  auto *FRILeftFun = FRILeft->getReferencedFunction();
  auto *FRIRightFun = FRIRight->getReferencedFunction();

  if (FRILeftFun->getEffectsInfo() >= EffectsKind::ReadWrite ||
      FRIRightFun->getEffectsInfo() >= EffectsKind::ReadWrite)
    return false;

  if (!FRILeftFun->hasDefinedSemantics() ||
      !FRIRightFun->hasDefinedSemantics())
    return false;

  auto SemanticsLeft = FRILeftFun->getSemanticsString();
  auto SemanticsRight = FRIRightFun->getSemanticsString();
  auto AILeftOperandsNum = AILeft->getNumOperands();
  auto AIRightOperandsNum = AIRight->getNumOperands();

  // makeUTF16 should have following parameters:
  // (start: RawPointer, numberOfCodeUnits: Word)
  // makeUTF8 should have following parameters:
  // (start: RawPointer, byteSize: Word, isASCII: Int1)
  if (!((SemanticsLeft == "string.makeUTF16" && AILeftOperandsNum == 4) ||
        (SemanticsLeft == "string.makeUTF8" && AILeftOperandsNum == 5) ||
        (SemanticsRight == "string.makeUTF16" && AIRightOperandsNum == 4) ||
        (SemanticsRight == "string.makeUTF8" && AIRightOperandsNum == 5)))
    return false;

  SLILeft = dyn_cast<StringLiteralInst>(AILeft->getOperand(1));
  SLIRight = dyn_cast<StringLiteralInst>(AIRight->getOperand(1));

  if (!SLILeft || !SLIRight)
    return false;

  // Only UTF-8 and UTF-16 encoded string literals are supported by this
  // optimization.
  if (SLILeft->getEncoding() != StringLiteralInst::Encoding::UTF8 &&
      SLILeft->getEncoding() != StringLiteralInst::Encoding::UTF16)
    return false;

  if (SLIRight->getEncoding() != StringLiteralInst::Encoding::UTF8 &&
      SLIRight->getEncoding() != StringLiteralInst::Encoding::UTF16)
    return false;

  return true;
}

/// Ensures that both string literals to be concatenated use the same
/// UTF encoding. Converts UTF-8 into UTF-16 if required.
void StringConcatenationOptimizer::adjustEncodings() {
  if (SLILeft->getEncoding() == SLIRight->getEncoding()) {
    FRIConvertFromBuiltin = FRILeft;
    IsTransparent = AILeft->isTransparent();
    if (SLILeft->getEncoding() == StringLiteralInst::Encoding::UTF8) {
      FuncResultType = AILeft->getOperand(4);
    } else {
      FuncResultType = AILeft->getOperand(3);
    }
    return;
  }

  // If one of the string literals is UTF8 and another one is UTF16,
  // convert the UTF8-encoded string literal into UTF16-encoding first.
  if (SLILeft->getEncoding() == StringLiteralInst::Encoding::UTF8 &&
      SLIRight->getEncoding() == StringLiteralInst::Encoding::UTF16) {
    FuncResultType = AIRight->getOperand(3);
    FRIConvertFromBuiltin = FRIRight;
    IsTransparent = AIRight->isTransparent();
    // Convert UTF8 representation into UTF16.
    SLILeft = Builder->createStringLiteral(AI->getLoc(), SLILeft->getValue(),
                                           StringLiteralInst::Encoding::UTF16);
    SLILeft->setDebugScope(AI->getDebugScope());
  }

  if (SLIRight->getEncoding() == StringLiteralInst::Encoding::UTF8 &&
      SLILeft->getEncoding() == StringLiteralInst::Encoding::UTF16) {
    FuncResultType = AILeft->getOperand(3);
    FRIConvertFromBuiltin = FRILeft;
    IsTransparent = AILeft->isTransparent();
    // Convert UTF8 representation into UTF16.
    SLIRight = Builder->createStringLiteral(AI->getLoc(), SLIRight->getValue(),
                                            StringLiteralInst::Encoding::UTF16);
    SLIRight->setDebugScope(AI->getDebugScope());
  }

  // It should be impossible to have two operands with different
  // encodings at this point.
  assert(SLILeft->getEncoding() == SLIRight->getEncoding() &&
        "Both operands of string concatenation should have the same encoding");
}

/// Computes the length of a concatenated string literal.
APInt StringConcatenationOptimizer::getConcatenatedLength() {
  // Real length of string literals computed based on its contents.
  // Length is in code units.
  auto SLILenLeft = SLILeft->getCodeUnitCount();
  auto SLILenRight = SLIRight->getCodeUnitCount();

  // Length of string literals as reported by string.make functions.
  auto *LenLeft = dyn_cast<IntegerLiteralInst>(AILeft->getOperand(2));
  auto *LenRight = dyn_cast<IntegerLiteralInst>(AIRight->getOperand(2));

  // Real and reported length should be the same.
  assert(SLILenLeft == LenLeft->getValue() &&
         "Size of string literal in @semantics(string.make) is wrong");

  assert(SLILenRight == LenRight->getValue() &&
         "Size of string literal in @semantics(string.make) is wrong");


  // Compute length of the concatenated literal.
  return LenLeft->getValue() + LenRight->getValue();
}

/// Computes the isAscii flag of a concatenated UTF8-encoded string literal.
bool StringConcatenationOptimizer::isAscii() const{
  // Add the isASCII argument in case of UTF8.
  // IsASCII is true only if IsASCII of both literals is true.
  auto *AsciiLeft = dyn_cast<IntegerLiteralInst>(AILeft->getOperand(3));
  auto *AsciiRight = dyn_cast<IntegerLiteralInst>(AIRight->getOperand(3));
  auto IsAsciiLeft = AsciiLeft->getValue() == 1;
  auto IsAsciiRight = AsciiRight->getValue() == 1;
  return IsAsciiLeft && IsAsciiRight;
}

SILInstruction *StringConcatenationOptimizer::optimize() {
  // Bail out if string literals concatenation optimization is
  // not possible.
  if (!extractStringConcatOperands())
    return nullptr;

  // Perform string literal encodings adjustments if needed.
  adjustEncodings();

  // Arguments of the new StringLiteralInst to be created.
  SmallVector<SILValue, 4> Arguments;

  // Encoding to be used for the concatenated string literal.
  auto Encoding = SLILeft->getEncoding();

  // Create a concatenated string literal.
  auto LV = SLILeft->getValue();
  auto RV = SLIRight->getValue();
  auto *NewSLI = Builder->createStringLiteral(AI->getLoc(),
                                              LV + Twine(RV),
                                              Encoding);
  NewSLI->setDebugScope(AI->getDebugScope());
  Arguments.push_back(NewSLI);

  // Length of the concatenated literal according to its encoding.
  auto *Len = Builder->createIntegerLiteral(AI->getLoc(),
                                            AILeft->getOperand(2).getType(),
                                            getConcatenatedLength());
  Len->setDebugScope(AI->getDebugScope());
  Arguments.push_back(Len);

  // isAscii flag for UTF8-encoded string literals.
  if (Encoding == StringLiteralInst::Encoding::UTF8) {
    bool IsAscii = isAscii();
    auto ILType = AILeft->getOperand(3).getType();
    auto *Ascii = Builder->createIntegerLiteral(AI->getLoc(),
                                                ILType,
                                                intmax_t(IsAscii));
    Ascii->setDebugScope(AI->getDebugScope());
    Arguments.push_back(Ascii);
  }

  // Type.
  Arguments.push_back(FuncResultType);

  auto FnTy = FRIConvertFromBuiltin->getType();
  auto STResultType = FnTy.castTo<SILFunctionType>()->getResult().getSILType();
  return ApplyInst::create(AI->getLoc(),
                           FRIConvertFromBuiltin,
                           FnTy,
                           STResultType,
                           ArrayRef<Substitution>(),
                           Arguments,
                           IsTransparent,
                           *FRIConvertFromBuiltin->getReferencedFunction());
}

SILInstruction *
SILCombiner::optimizeConcatenationOfStringLiterals(ApplyInst *AI) {
  // String literals concatenation optimizer.
  StringConcatenationOptimizer SLConcatenationOptimizer(AI, Builder);
  return SLConcatenationOptimizer.optimize();
}

/// \brief Returns a list of instructions that only write into the uninitialized
/// array \p Inst.
static bool recursivelyCollectArrayWritesInstr(UserListTy &Uses,
                                               SILInstruction *Inst) {
  Uses.push_back(Inst);
  for (auto Op : Inst->getUses()) {
    if (isa<RefCountingInst>(Op->getUser()) ||
        // The store must not store the array but only to the array.
        (isa<StoreInst>(Op->getUser()) &&
         dyn_cast<StoreInst>(Op->getUser())->getSrc().getDef() != Inst) ||
        isa<DebugValueInst>(Op->getUser())) {
      Uses.push_back(Op->getUser());
      continue;
    }

    SILInstruction *Proj;
    if ((Proj = dyn_cast<TupleExtractInst>(Op->getUser())) ||
        (Proj = dyn_cast<StructExtractInst>(Op->getUser())) ||
        (Proj = dyn_cast<IndexAddrInst>(Op->getUser())) ||
        (Proj = dyn_cast<PointerToAddressInst>(Op->getUser())))
      if (recursivelyCollectArrayWritesInstr(Uses, Proj))
        continue;

    return false;
  }

  return true;
}

SILInstruction *SILCombiner::visitBuiltinInst(BuiltinInst *I) {
  if (I->getBuiltinInfo().ID == BuiltinValueKind::CanBeObjCClass)
    return optimizeBuiltinCanBeObjCClass(I);

  if (I->getBuiltinInfo().ID == BuiltinValueKind::ICMP_EQ)
    return optimizeBuiltinCompareEq(I, /*Negate Eq result*/ false);

  if (I->getBuiltinInfo().ID == BuiltinValueKind::ICMP_NE)
    return optimizeBuiltinCompareEq(I, /*Negate Eq result*/ true);

  // Optimize sub(x - x) -> 0.
  if (I->getNumOperands() == 2 &&
      match(I, m_ApplyInst(BuiltinValueKind::Sub, m_ValueBase())) &&
      I->getOperand(0) == I->getOperand(1))
    if (auto DestTy = I->getType().getAs<BuiltinIntegerType>())
      return IntegerLiteralInst::create(I->getLoc(), I->getType(),
                                        APInt(DestTy->getGreatestWidth(), 0),
                                        *I->getFunction());

  // Optimize sub(ptrtoint(index_raw_pointer(v, x)), ptrtoint(v)) -> x.
  BuiltinInst *Bytes2;
  IndexRawPointerInst *Indexraw;
  if (I->getNumOperands() == 2 &&
      match(I, m_BuiltinInst(BuiltinValueKind::Sub,
                             m_BuiltinInst(BuiltinValueKind::PtrToInt,
                                           m_IndexRawPointerInst(Indexraw)),
                             m_BuiltinInst(Bytes2)))) {
    if (match(Bytes2,
              m_BuiltinInst(BuiltinValueKind::PtrToInt, m_ValueBase()))) {
      if (Indexraw->getOperand(0) == Bytes2->getOperand(0) &&
          Indexraw->getOperand(1).getType() == I->getType()) {
        replaceInstUsesWith(*I, Indexraw->getOperand(1).getDef());
        return eraseInstFromFunction(*I);
      }
    }
  }

  // Canonicalize multiplication by a stride to be such that the stride is
  // always the second argument.
  if (I->getNumOperands() != 3)
    return nullptr;

  if (match(I, m_ApplyInst(BuiltinValueKind::SMulOver,
                            m_ApplyInst(BuiltinValueKind::Strideof),
                            m_ValueBase(), m_IntegerLiteralInst())) ||
      match(I, m_ApplyInst(BuiltinValueKind::SMulOver,
                            m_ApplyInst(BuiltinValueKind::StrideofNonZero),
                            m_ValueBase(), m_IntegerLiteralInst()))) {
    I->swapOperands(0, 1);
    return I;
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitApplyInst(ApplyInst *AI) {
  // Optimize apply{partial_apply(x,y)}(z) -> apply(z,x,y).
  if (auto *PAI = dyn_cast<PartialApplyInst>(AI->getCallee()))
    return optimizeApplyOfPartialApply(AI, PAI);

  if (auto *CFI = dyn_cast<ConvertFunctionInst>(AI->getCallee()))
    return optimizeApplyOfConvertFunctionInst(AI, CFI);

  // Optimize readonly functions with no meaningful users.
  FunctionRefInst *FRI = dyn_cast<FunctionRefInst>(AI->getCallee());
  if (FRI && FRI->getReferencedFunction()->getEffectsInfo() <
      EffectsKind::ReadWrite){
    UserListTy Users;
    if (recursivelyCollectARCUsers(Users, AI)) {
      // When deleting Apply instructions make sure to release any owned
      // arguments.
      auto FT = FRI->getFunctionType();
      for (int i = 0, e = AI->getNumArguments(); i < e; ++i) {
        SILParameterInfo PI = FT->getParameters()[i];
        auto Arg = AI->getArgument(i);
        if (PI.isConsumed() && !Arg.getType().isAddress())
          Builder->emitReleaseValueOperation(AI->getLoc(), Arg);
      }

      // Erase all of the reference counting instructions and the Apply itself.
      for (auto rit = Users.rbegin(), re = Users.rend(); rit != re; ++rit)
        eraseInstFromFunction(**rit);

      return nullptr;
    }
    // We found a user that we can't handle.
  }

  if (FRI) {
    auto *SF = FRI->getReferencedFunction();
    if (SF->getEffectsInfo() < EffectsKind::ReadWrite) {
      // Try to optimize string concatenation.
      if (auto I = optimizeConcatenationOfStringLiterals(AI)) {
        return I;
      }
    }
    if (SF->hasSemanticsString("array.uninitialized")) {
      UserListTy Users;
      // If the uninitialized array is only written into then it can be removed.
      if (recursivelyCollectArrayWritesInstr(Users, AI)) {
        // Erase all of the reference counting instructions and the array
        // allocation instruction.
        for (auto rit = Users.rbegin(), re = Users.rend(); rit != re; ++rit)
          eraseInstFromFunction(**rit);
      }
    }
  }


  // (apply (thin_to_thick_function f)) to (apply f)
  if (auto *TTTFI = dyn_cast<ThinToThickFunctionInst>(AI->getCallee())) {
    // TODO: Handle substitutions and indirect results
    if (AI->hasSubstitutions() || AI->hasIndirectResult())
      return nullptr;
    SmallVector<SILValue, 4> Arguments;
    for (auto &Op : AI->getArgumentOperands()) {
      Arguments.push_back(Op.get());
    }
    // The type of the substition is the source type of the thin to thick
    // instruction.
    SILType substTy = TTTFI->getOperand().getType();
    return ApplyInst::create(AI->getLoc(), TTTFI->getOperand(),
                             substTy, AI->getType(),
                             AI->getSubstitutions(), Arguments,
                             AI->isTransparent(),
                             *AI->getFunction());
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitCondFailInst(CondFailInst *CFI) {
  // Remove runtime asserts such as overflow checks and bounds checks.
  if (RemoveCondFails)
    return eraseInstFromFunction(*CFI);

  // Erase. (cond_fail 0)
  if (auto *I = dyn_cast<IntegerLiteralInst>(CFI->getOperand()))
    if (!I->getValue().getBoolValue())
      return eraseInstFromFunction(*CFI);

  return nullptr;
}

SILInstruction *SILCombiner::visitStrongRetainInst(StrongRetainInst *SRI) {
  // Retain of ThinToThickFunction is a no-op.
  if (isa<ThinToThickFunctionInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  if (isa<ObjCExistentialMetatypeToObjectInst>(SRI->getOperand()) ||
      isa<ObjCMetatypeToObjectInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  // Sometimes in the stdlib due to hand offs, we will see code like:
  //
  // strong_release %0
  // strong_retain %0
  //
  // with the matching strong_retain to the strong_release in a predecessor
  // basic block and the matching strong_release for the strong_retain in a
  // successor basic block.
  //
  // Due to the matching pairs being in different basic blocks, the ARC
  // Optimizer (which is currently local to one basic block does not handle
  // it). But that does not mean that we can not eliminate this pair with a
  // peephole.

  // If we are not the first instruction in this basic block...
  if (SRI != &*SRI->getParent()->begin()) {
    SILBasicBlock::iterator Pred = SRI;
    --Pred;

    // ...and the predecessor instruction is a strong_release on the same value
    // as our strong_retain...
    if (StrongReleaseInst *Release = dyn_cast<StrongReleaseInst>(&*Pred))
      // Remove them...
      if (Release->getOperand() == SRI->getOperand()) {
        eraseInstFromFunction(*Release);
        return eraseInstFromFunction(*SRI);
      }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::visitRefToRawPointerInst(RefToRawPointerInst *RRPI) {
  // Ref to raw pointer consumption of other ref casts.
  //
  // (ref_to_raw_pointer (unchecked_ref_cast x))
  //    -> (ref_to_raw_pointer x)
  if (auto *ROPI = dyn_cast<UncheckedRefCastInst>(RRPI->getOperand())) {
    RRPI->setOperand(ROPI->getOperand());
    return ROPI->use_empty() ? eraseInstFromFunction(*ROPI) : nullptr;
  }

  // (ref_to_raw_pointer (open_existential_ref (init_existential_ref x))) ->
  // (ref_to_raw_pointer x)
  if (auto *OER = dyn_cast<OpenExistentialRefInst>(RRPI->getOperand()))
    if (auto *IER = dyn_cast<InitExistentialRefInst>(OER->getOperand()))
      return new (RRPI->getModule()) RefToRawPointerInst(
          RRPI->getLoc(), IER->getOperand(), RRPI->getType());
  return nullptr;
}


/// Simplify the following two frontend patterns:
///
///   %payload_addr = init_enum_data_addr %payload_allocation
///   store %payload to %payload_addr
///   inject_enum_addr %payload_allocation, $EnumType.case
///
///   inject_enum_add %nopayload_allocation, $EnumType.case
///
/// for a concrete enum type $EnumType.case to:
///
///   %1 = enum $EnumType, $EnumType.case, %payload
///   store %1 to %payload_addr
///
///   %1 = enum $EnumType, $EnumType.case
///   store %1 to %nopayload_addr
///
/// We leave the cleaning up to mem2reg.
SILInstruction *
SILCombiner::visitInjectEnumAddrInst(InjectEnumAddrInst *IEAI) {
  // Given an inject_enum_addr of a concrete type without payload, promote it to
  // a store of an enum. Mem2reg/load forwarding will clean things up for us. We
  // can't handle the payload case here due to the flow problems caused by the
  // dependency in between the enum and its data.
  assert(IEAI->getOperand().getType().isAddress() && "Must be an address");
  if (IEAI->getOperand().getType().isAddressOnly(IEAI->getModule()))
    return nullptr;

  // If the enum does not have a payload create the enum/store since we don't
  // need to worry about payloads.
  if (!IEAI->getElement()->hasArgumentType()) {
    EnumInst *E =
      Builder->createEnum(IEAI->getLoc(), SILValue(), IEAI->getElement(),
                          IEAI->getOperand().getType().getObjectType());
    E->setDebugScope(IEAI->getDebugScope());
    Builder->createStore(IEAI->getLoc(), E, IEAI->getOperand())
      ->setDebugScope(IEAI->getDebugScope());
    return eraseInstFromFunction(*IEAI);
  }

  // Ok, we have a payload enum, make sure that we have a store previous to
  // us...
  SILBasicBlock::iterator II = IEAI;
  if (II == IEAI->getParent()->begin())
    return nullptr;
  --II;
  auto *SI = dyn_cast<StoreInst>(&*II);
  if (!SI)
    return nullptr;

  // ... whose destination is taken from an init_enum_data_addr whose only user
  // is the store that points to the same allocation as our inject_enum_addr. We
  // enforce such a strong condition as being directly previously since we want
  // to avoid any flow issues.
  auto *IEDAI = dyn_cast<InitEnumDataAddrInst>(SI->getDest().getDef());
  if (!IEDAI || IEDAI->getOperand() != IEAI->getOperand() ||
      !IEDAI->hasOneUse())
    return nullptr;

  // In that case, create the payload enum/store.
  EnumInst *E =
      Builder->createEnum(IEDAI->getLoc(), SI->getSrc(), IEDAI->getElement(),
                          IEDAI->getOperand().getType().getObjectType());
  E->setDebugScope(IEDAI->getDebugScope());
  Builder->createStore(IEDAI->getLoc(), E, IEDAI->getOperand())
    ->setDebugScope(IEDAI->getDebugScope());
  // Cleanup.
  eraseInstFromFunction(*SI);
  eraseInstFromFunction(*IEDAI);
  return eraseInstFromFunction(*IEAI);
}

SILInstruction *SILCombiner::visitUpcastInst(UpcastInst *UCI) {
  // Ref to raw pointer consumption of other ref casts.
  //
  // (upcast (upcast x)) -> (upcast x)
  if (auto *Op = dyn_cast<UpcastInst>(UCI->getOperand())) {
    UCI->setOperand(Op->getOperand());
    return Op->use_empty() ? eraseInstFromFunction(*Op) : nullptr;
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitPointerToAddressInst(PointerToAddressInst *PTAI) {
  // If we reach this point, we know that the types must be different since
  // otherwise simplifyInstruction would have handled the identity case. This is
  // always legal to do since address-to-pointer pointer-to-address implies
  // layout compatibility.
  //
  // (pointer-to-address (address-to-pointer %x)) -> unchecked_
  if (auto *ATPI = dyn_cast<AddressToPointerInst>(PTAI->getOperand())) {
    return new (PTAI->getModule()) UncheckedAddrCastInst(PTAI->getLoc(),
                                                         ATPI->getOperand(),
                                                         PTAI->getType());
  }

  // Turn:
  //
  //   %stride = Builtin.strideof(T) * %distance
  //   %ptr' = index_raw_pointer %ptr, %stride
  //   %result = pointer_to_address %ptr, $T'
  //
  // To:
  //
  //   %addr = pointer_to_address %ptr, $T
  //   %result = index_addr %addr, %distance
  //
  BuiltinInst *Bytes;
  MetatypeInst *Metatype;
  if (match(PTAI->getOperand(),
            m_IndexRawPointerInst(m_ValueBase(),
                                  m_TupleExtractInst(m_BuiltinInst(Bytes),
                                                     0)))) {
    if (match(Bytes, m_ApplyInst(BuiltinValueKind::SMulOver, m_ValueBase(),
                                 m_ApplyInst(BuiltinValueKind::Strideof,
                                             m_MetatypeInst(Metatype)),
                                 m_ValueBase())) ||
        match(Bytes, m_ApplyInst(BuiltinValueKind::SMulOver, m_ValueBase(),
                                 m_ApplyInst(BuiltinValueKind::StrideofNonZero,
                                             m_MetatypeInst(Metatype)),
                                 m_ValueBase()))) {
      SILType InstanceType =
        Metatype->getType().getMetatypeInstanceType(PTAI->getModule());

      // Make sure that the type of the metatype matches the type that we are
      // casting to so we stride by the correct amount.
      if (InstanceType.getAddressType() != PTAI->getType())
        return nullptr;

      auto IRPI = cast<IndexRawPointerInst>(PTAI->getOperand().getDef());
      SILValue Ptr = IRPI->getOperand(0);
      SILValue Distance = Bytes->getArguments()[0];
      auto *NewPTAI =
          Builder->createPointerToAddress(PTAI->getLoc(), Ptr, PTAI->getType());
      NewPTAI->setDebugScope(PTAI->getDebugScope());
      return new (PTAI->getModule())
          IndexAddrInst(PTAI->getLoc(), NewPTAI, Distance);
    }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::visitUncheckedAddrCastInst(UncheckedAddrCastInst *UADCI) {
  SILModule &Mod = UADCI->getModule();

  // (unchecked-addr-cast (unchecked-addr-cast x X->Y) Y->Z)
  //   ->
  // (unchecked-addr-cast x X->Z)
  if (auto *OtherUADCI = dyn_cast<UncheckedAddrCastInst>(UADCI->getOperand()))
    return new (Mod) UncheckedAddrCastInst(
        UADCI->getLoc(), OtherUADCI->getOperand(), UADCI->getType());

  // (unchecked-addr-cast cls->superclass) -> (upcast cls->superclass)
  if (UADCI->getType() != UADCI->getOperand().getType() &&
      UADCI->getType().isSuperclassOf(UADCI->getOperand().getType()))
    return new (Mod) UpcastInst(UADCI->getLoc(), UADCI->getOperand(),
                                UADCI->getType());

  // See if we have all loads from this unchecked_addr_cast. If we do, load the
  // original type and create the appropriate bitcast.

  // First if our UADCI has not users, bail. This will be eliminated by DCE.
  if (UADCI->use_empty())
    return nullptr;

  SILType InputTy = UADCI->getOperand().getType();
  SILType OutputTy = UADCI->getType();

  // If either type is address only, do not do anything here.
  if (InputTy.isAddressOnly(Mod) || OutputTy.isAddressOnly(Mod))
    return nullptr;

  bool InputIsTrivial = InputTy.isTrivial(Mod);
  bool OutputIsTrivial = OutputTy.isTrivial(Mod);

  // If our input is trivial and our output type is not, do not do
  // anything. This is to ensure that we do not change any types reference
  // semantics from trivial -> reference counted.
  if (InputIsTrivial && !OutputIsTrivial)
    return nullptr;

  // The structs could have different size. We have code in the stdlib that
  // casts pointers to differently sized integer types. This code prevents that
  // we bitcast the values.
  if (InputTy.getStructOrBoundGenericStruct() &&
      OutputTy.getStructOrBoundGenericStruct())
    return nullptr;

  // For each user U of the unchecked_addr_cast...
  for (auto U : UADCI->getUses())
    // Check if it is load. If it is not a load, bail...
    if (!isa<LoadInst>(U->getUser()))
      return nullptr;

  SILValue Op = UADCI->getOperand();
  SILLocation Loc = UADCI->getLoc();
  SILDebugScope *Scope = UADCI->getDebugScope();

  // Ok, we have all loads. Lets simplify this. Go back through the loads a
  // second time, rewriting them into a load + bitcast from our source.
  for (auto U : UADCI->getUses()) {
    // Grab the original load.
    LoadInst *L = cast<LoadInst>(U->getUser());

    // Insert a new load from our source and bitcast that as appropriate.
    LoadInst *NewLoad = Builder->createLoad(Loc, Op);
    NewLoad->setDebugScope(Scope);
    SILInstruction *BitCast = nullptr;
    if (OutputIsTrivial)
      BitCast = Builder->createUncheckedTrivialBitCast(Loc, NewLoad,
                                                       OutputTy.getObjectType());
    else
      BitCast = Builder->createUncheckedRefBitCast(Loc, NewLoad,
                                                   OutputTy.getObjectType());
    BitCast->setDebugScope(Scope);

    // Replace all uses of the old load with the new bitcasted result and erase
    // the old load.
    replaceInstUsesWith(*L, BitCast, 0);
    eraseInstFromFunction(*L);
  }

  // Delete the old cast.
  return eraseInstFromFunction(*UADCI);
}

SILInstruction *
SILCombiner::visitUncheckedRefCastInst(UncheckedRefCastInst *URCI) {
  // (unchecked-ref-cast (unchecked-ref-cast x X->Y) Y->Z)
  //   ->
  // (unchecked-ref-cast x X->Z)
  if (auto *OtherURCI = dyn_cast<UncheckedRefCastInst>(URCI->getOperand()))
    return new (URCI->getModule()) UncheckedRefCastInst(
        URCI->getLoc(), OtherURCI->getOperand(), URCI->getType());

  // (unchecked_ref_cast (upcast x X->Y) Y->Z) -> (unchecked_ref_cast x X->Z)
  if (auto *UI = dyn_cast<UpcastInst>(URCI->getOperand()))
    return new (URCI->getModule())
        UncheckedRefCastInst(URCI->getLoc(), UI->getOperand(), URCI->getType());

  if (URCI->getType() != URCI->getOperand().getType() &&
      URCI->getType().isSuperclassOf(URCI->getOperand().getType()))
    return new (URCI->getModule())
        UpcastInst(URCI->getLoc(), URCI->getOperand(), URCI->getType());

  // (unchecked_ref_cast (open_existential_ref (init_existential_ref X))) ->
  // (unchecked_ref_cast X)
  if (auto *OER = dyn_cast<OpenExistentialRefInst>(URCI->getOperand()))
    if (auto *IER = dyn_cast<InitExistentialRefInst>(OER->getOperand()))
      return new (URCI->getModule()) UncheckedRefCastInst(
          URCI->getLoc(), IER->getOperand(), URCI->getType());

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUnconditionalCheckedCastInst(UnconditionalCheckedCastInst *UCCI) {
  // FIXME: rename from RemoveCondFails to RemoveRuntimeAsserts.
  if (RemoveCondFails) {
    SILModule &Mod = UCCI->getModule();
    SILValue Op = UCCI->getOperand();
    SILLocation Loc = UCCI->getLoc();

    if (Op.getType().isAddress()) {
      // unconditional_checked_cast -> unchecked_addr_cast
      return new (Mod) UncheckedAddrCastInst(Loc, Op, UCCI->getType());
    } else if (Op.getType().isHeapObjectReferenceType()) {
      // unconditional_checked_cast -> unchecked_ref_cast
      return new (Mod) UncheckedRefCastInst(Loc, Op, UCCI->getType());
    }
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitRawPointerToRefInst(RawPointerToRefInst *RawToRef) {
  // (raw_pointer_to_ref (ref_to_raw_pointer x X->Y) Y->Z)
  //   ->
  // (unchecked_ref_cast X->Z)
  if (auto *RefToRaw = dyn_cast<RefToRawPointerInst>(RawToRef->getOperand())) {
    return new (RawToRef->getModule()) UncheckedRefCastInst(
        RawToRef->getLoc(), RefToRaw->getOperand(), RawToRef->getType());
  }

  return nullptr;
}

/// We really want to eliminate unchecked_take_enum_data_addr. Thus if we find
/// one go through all of its uses and see if they are all loads and address
/// projections (in many common situations this is true). If so, perform:
///
/// (load (unchecked_take_enum_data_addr x)) -> (unchecked_enum_data (load x))
///
/// FIXME: Implement this for address projections.
SILInstruction *
SILCombiner::
visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *TEDAI) {
  // If our TEDAI has no users, there is nothing to do.
  if (TEDAI->use_empty())
    return nullptr;

  // If our enum type is address only, we can not do anything here. The key
  // thing to remember is that an enum is address only if any of its cases are
  // address only. So we *could* have a loadable payload resulting from the
  // TEDAI without the TEDAI being loadable itself.
  if (TEDAI->getOperand().getType().isAddressOnly(TEDAI->getModule()))
    return nullptr;

  // For each user U of the take_enum_data_addr...
  for (auto U : TEDAI->getUses())
    // Check if it is load. If it is not a load, bail...
    if (!isa<LoadInst>(U->getUser()))
      return nullptr;

  // Grab the EnumAddr.
  SILLocation Loc = TEDAI->getLoc();
  SILDebugScope *Scope = TEDAI->getDebugScope();
  SILValue EnumAddr = TEDAI->getOperand();
  EnumElementDecl *EnumElt = TEDAI->getElement();
  SILType PayloadType = TEDAI->getType().getObjectType();

  // Go back through a second time now that we know all of our users are
  // loads. Perform the transformation on each load.
  for (auto U : TEDAI->getUses()) {
    // Grab the load.
    LoadInst *L = cast<LoadInst>(U->getUser());

    // Insert a new Load of the enum and extract the data from that.
    auto *Load = Builder->createLoad(Loc, EnumAddr);
    Load->setDebugScope(Scope);
    auto *D = Builder->createUncheckedEnumData(
        Loc, Load, EnumElt, PayloadType);
    D->setDebugScope(Scope);

    // Replace all uses of the old load with the data and erase the old load.
    replaceInstUsesWith(*L, D, 0);
    eraseInstFromFunction(*L);
  }

  return eraseInstFromFunction(*TEDAI);
}

SILInstruction *SILCombiner::visitStrongReleaseInst(StrongReleaseInst *SRI) {
  // Release of ThinToThickFunction is a no-op.
  if (isa<ThinToThickFunctionInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  if (isa<ObjCExistentialMetatypeToObjectInst>(SRI->getOperand()) ||
      isa<ObjCMetatypeToObjectInst>(SRI->getOperand()))
    return eraseInstFromFunction(*SRI);

  return nullptr;
}

SILInstruction *SILCombiner::visitCondBranchInst(CondBranchInst *CBI) {
  // cond_br(xor(x, 1)), t_label, f_label -> cond_br x, f_label, t_label
  SILValue X;
  if (match(CBI->getCondition(), m_ApplyInst(BuiltinValueKind::Xor,
                                             m_SILValue(X), m_One()))) {
    SmallVector<SILValue, 4> OrigTrueArgs, OrigFalseArgs;
    for (const auto &Op : CBI->getTrueArgs())
      OrigTrueArgs.push_back(Op);
    for (const auto &Op : CBI->getFalseArgs())
      OrigFalseArgs.push_back(Op);
    return CondBranchInst::create(CBI->getLoc(), X,
                                  CBI->getFalseBB(), OrigFalseArgs,
                                  CBI->getTrueBB(), OrigTrueArgs,
                                  *CBI->getFunction());
  }
  return nullptr;
}

SILInstruction *
SILCombiner::
visitUncheckedRefBitCastInst(UncheckedRefBitCastInst *URBCI) {
  // (unchecked_ref_bit_cast Y->Z (unchecked_ref_bit_cast X->Y x))
  //   ->
  // (unchecked_ref_bit_cast X->Z x)
  if (auto *Op = dyn_cast<UncheckedRefBitCastInst>(URBCI->getOperand())) {
    return new (URBCI->getModule()) UncheckedRefBitCastInst(URBCI->getLoc(),
                                                            Op->getOperand(),
                                                            URBCI->getType());
  }

  return nullptr;
}

SILInstruction *
SILCombiner::
visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *UTBCI) {
  // (unchecked_trivial_bit_cast Y->Z
  //                                 (unchecked_trivial_bit_cast X->Y x))
  //   ->
  // (unchecked_trivial_bit_cast X->Z x)
  SILValue Op = UTBCI->getOperand();
  if (auto *OtherUTBCI = dyn_cast<UncheckedTrivialBitCastInst>(Op)) {
    SILModule &Mod = UTBCI->getModule();
    return new (Mod) UncheckedTrivialBitCastInst(UTBCI->getLoc(),
                                                 OtherUTBCI->getOperand(),
                                                 UTBCI->getType());
  }

  // (unchecked_trivial_bit_cast Y->Z
  //                                 (unchecked_ref_bit_cast X->Y x))
  //   ->
  // (unchecked_trivial_bit_cast X->Z x)
  if (auto *URBCI = dyn_cast<UncheckedRefBitCastInst>(Op)) {
    SILModule &Mod = UTBCI->getModule();
    return new (Mod) UncheckedTrivialBitCastInst(UTBCI->getLoc(),
                                                 URBCI->getOperand(),
                                                 UTBCI->getType());
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitSelectEnumInst(SelectEnumInst *EIT) {
  // TODO: We should be able to flat-out replace the select_enum instruction
  // with the selected value in another pass. For parity with the enum_is_tag
  // combiner pass, handle integer literals for now.
  auto *EI = dyn_cast<EnumInst>(EIT->getEnumOperand());
  if (!EI)
    return nullptr;

  SILValue selected;
  for (unsigned i = 0, e = EIT->getNumCases(); i < e; ++i) {
    auto casePair = EIT->getCase(i);
    if (casePair.first == EI->getElement()) {
      selected = casePair.second;
      break;
    }
  }
  if (!selected)
    selected = EIT->getDefaultResult();

  if (auto inst = dyn_cast<IntegerLiteralInst>(selected)) {
    return IntegerLiteralInst::create(inst->getLoc(), inst->getType(),
                                      inst->getValue(), *EIT->getFunction());
  }

  return nullptr;
}

/// Helper function for simplifying convertions between
/// thick and objc metatypes.
static SILInstruction *
visitMetatypeConversionInst(ConversionInst *MCI,
                            MetatypeRepresentation Representation) {
  SILValue Op = MCI->getOperand(0);
  SILModule &Mod = MCI->getModule();
  // Instruction has a proper target type already.
  SILType Ty = MCI->getType();
  auto MetatypeTy = Op.getType().getAs<AnyMetatypeType>();

  if (MetatypeTy->getRepresentation() != Representation)
    return nullptr;

  if (dyn_cast<MetatypeInst>(Op)) {
    return new (Mod) MetatypeInst(MCI->getLoc(), Ty);
  } else if (auto *VMI = dyn_cast<ValueMetatypeInst>(Op)) {
    return new (Mod) ValueMetatypeInst(MCI->getLoc(),
                                       Ty,
                                       VMI->getOperand());
  } else if (auto *EMI = dyn_cast<ExistentialMetatypeInst>(Op)) {
    return new (Mod) ExistentialMetatypeInst(MCI->getLoc(),
                                             Ty,
                                             EMI->getOperand());
  }
  return nullptr;
}

SILInstruction *
SILCombiner::visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *TTOCMI) {
  // Perform the following transformations:
  // (thick_to_objc_metatype (metatype @thick)) ->
  // (metatype @objc_metatype)
  //
  // (thick_to_objc_metatype (value_metatype @thick)) ->
  // (value_metatype @objc_metatype)
  //
  // (thick_to_objc_metatype (existential_metatype @thick)) ->
  // (existential_metatype @objc_metatype)
  return visitMetatypeConversionInst(TTOCMI, MetatypeRepresentation::Thick);
}

SILInstruction *
SILCombiner::visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *OCTTMI) {
  // Perform the following transformations:
  // (objc_to_thick_metatype (metatype @objc_metatype)) ->
  // (metatype @thick)
  //
  // (objc_to_thick_metatype (value_metatype @objc_metatype)) ->
  // (value_metatype @thick)
  //
  // (objc_to_thick_metatype (existential_metatype @objc_metatype)) ->
  // (existential_metatype @thick)
  return visitMetatypeConversionInst(OCTTMI, MetatypeRepresentation::ObjC);
}

SILInstruction *SILCombiner::visitTupleExtractInst(TupleExtractInst *TEI) {
  // tuple_extract(apply([add|sub|...]overflow(x, 0)), 1) -> 0
  // if it can be proven that no overflow can happen.
  if (TEI->getFieldNo() != 1)
    return nullptr;

  if (auto *BI = dyn_cast<BuiltinInst>(TEI->getOperand()))
    if (!canOverflow(BI))
      return IntegerLiteralInst::create(TEI->getLoc(), TEI->getType(),
                                        APInt(1, 0), *TEI->getFunction());
  return nullptr;
}
