#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"

#ifndef NDEBUG
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#endif

#include <vector>
#include <random>

using namespace llvm;

namespace {
class ObfuscateZero : public BasicBlockPass {
  std::vector<Value *> IntegerVect;
  std::default_random_engine Generator;

public:
  static char ID;

  ObfuscateZero() : BasicBlockPass(ID) {}

  virtual bool runOnBasicBlock(BasicBlock &BB) override {
    IntegerVect.clear();
    bool modified = false;

    // Not iterating from the beginning to avoid obfuscation of Phi instructions
    // parameters
    for (typename BasicBlock::iterator I = BB.getFirstInsertionPt(),
                                       end = BB.end();
         I != end; ++I) {
      Instruction &Inst = *I;
      if (isValidCandidateInstruction(Inst)) {
        for (size_t i{0}; i < Inst.getNumOperands(); ++i) {
          if (Constant *C = isValidCandidateOperand(Inst.getOperand(i))) {
            if (Value *New_val = replaceZero(Inst, C)) {
              Inst.setOperand(i, New_val);
              modified = true;
            } else {
              //dbgs() << "ObfuscateZero: could not rand pick a variable for replacement\n";
            }
          }
        }
      }
      registerInteger(Inst);
    }

#ifndef NDEBUG
    verifyFunction(*BB.getParent());
#endif
    return modified;
  }

private:
  bool isValidCandidateInstruction(Instruction &Inst) {
    if (isa<GetElementPtrInst>(&Inst)) {
      // dbgs() << "Ignoring GEP\n";
      return false;
    } else if (isa<SwitchInst>(&Inst)) {
      // dbgs() << "Ignoring Switch\n";
      return false;
    } else if (isa<CallInst>(&Inst)) {
      // dbgs() << "Ignoring Calls\n";
      return false;
    } else {
      return true;
    }
  }

  Constant *isValidCandidateOperand(Value *V) {
    if (Constant *C = dyn_cast<Constant>(V)) {
      // Checking constant eligibility
      if (isa<PointerType>(C->getType())) {
        // dbgs() << "Ignoring NULL pointers\n";
        return nullptr;
      } else if (C->getType()->isFloatingPointTy()) {
        // dbgs() << "Ignoring Floats 0\n";
        return nullptr;
      } else if (C->isNullValue()) {
        return C;
      } else {
        return nullptr;
      }
    } else {
      return nullptr;
    }
  }

  void registerInteger(Value &V) {
    if (V.getType()->isIntegerTy())
      IntegerVect.emplace_back(&V);
  }

  Value *replaceZero(Instruction &Inst, Value *VReplace) {
    // Replacing 0 by:
    // prime1 * ((x | any1)**2) != prime2 * ((y | any2)**2)
    // with prime1 != prime2 and any1 != 0 and any2 != 0
    using prime_type = uint32_t;
    // FIXME : replace by dynamic generation
    const prime_type p1 = 431, p2 = 277;

    Type *ReplacedType = VReplace->getType(),
         *IntermediaryType = IntegerType::get(Inst.getParent()->getContext(),
                                              sizeof(prime_type) * 8);

    if (IntegerVect.size() < 1) {
      return nullptr;
    }

    std::uniform_int_distribution<size_t> Rand(0, IntegerVect.size() - 1);
    std::uniform_int_distribution<size_t> RandAny(1, 10);

    size_t Index1 = Rand(Generator), Index2 = Rand(Generator);

    // Masking Any1 and Any2 to avoid overflow in the obsfuscation
    Constant *any1 = ConstantInt::get(IntermediaryType, 1 + RandAny(Generator)),
             *any2 = ConstantInt::get(IntermediaryType, 1 + RandAny(Generator)),
             *prime1 = ConstantInt::get(IntermediaryType, p1),
             *prime2 = ConstantInt::get(IntermediaryType, p2),
             // Bitmasks to prevent overflow
        *OverflowMaskLHS = ConstantInt::get(IntermediaryType, 0x00000007),
             *OverflowMaskRHS = ConstantInt::get(IntermediaryType, 0x00000007);

    IRBuilder<> Builder(&Inst);

    // lhs
    // To avoid overflow
    Value *LhsCast =
        Builder.CreateZExtOrTrunc(IntegerVect.at(Index1), IntermediaryType);
    registerInteger(*LhsCast);
    Value *LhsAnd = Builder.CreateAnd(LhsCast, OverflowMaskLHS);
    registerInteger(*LhsAnd);
    Value *LhsOr = Builder.CreateOr(LhsAnd, any1);
    registerInteger(*LhsOr);
    Value *LhsSquare = Builder.CreateMul(LhsOr, LhsOr);
    registerInteger(*LhsSquare);
    Value *LhsTot = Builder.CreateMul(LhsSquare, prime1);
    registerInteger(*LhsTot);

    // rhs
    Value *RhsCast =
        Builder.CreateZExtOrTrunc(IntegerVect.at(Index2), IntermediaryType);
    registerInteger(*RhsCast);
    Value *RhsAnd = Builder.CreateAnd(RhsCast, OverflowMaskRHS);
    registerInteger(*RhsAnd);
    Value *RhsOr = Builder.CreateOr(RhsAnd, any2);
    registerInteger(*RhsOr);
    Value *RhsSquare = Builder.CreateMul(RhsOr, RhsOr);
    registerInteger(*RhsSquare);
    Value *RhsTot = Builder.CreateMul(RhsSquare, prime2);
    registerInteger(*RhsTot);

    // comp
    Value *comp =
        Builder.CreateICmp(CmpInst::Predicate::ICMP_EQ, LhsTot, RhsTot);
    registerInteger(*comp);
    Value *castComp = Builder.CreateZExt(comp, ReplacedType);
    registerInteger(*castComp);

    return castComp;
  }
};
}

char ObfuscateZero::ID = 0;
static RegisterPass<ObfuscateZero> X("ObfuscateZero", "Obfuscates zeroes",
                                     false, false);

// register pass for clang use
static void registerObfuscateZeroPass(const PassManagerBuilder &,
                                      PassManagerBase &PM) {
  PM.add(new ObfuscateZero());
}
static RegisterStandardPasses
    RegisterMBAPass(PassManagerBuilder::EP_EarlyAsPossible,
                    registerObfuscateZeroPass);
