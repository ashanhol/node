// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/code-generator.h"

#include "src/compilation-info.h"
#include "src/compiler/code-generator-impl.h"
#include "src/compiler/gap-resolver.h"
#include "src/compiler/node-matchers.h"
#include "src/compiler/osr.h"
#include "src/s390/macro-assembler-s390.h"

namespace v8 {
namespace internal {
namespace compiler {

#define __ masm()->

#define kScratchReg ip

// Adds S390-specific methods to convert InstructionOperands.
class S390OperandConverter final : public InstructionOperandConverter {
 public:
  S390OperandConverter(CodeGenerator* gen, Instruction* instr)
      : InstructionOperandConverter(gen, instr) {}

  size_t OutputCount() { return instr_->OutputCount(); }

  bool Is64BitOperand(int index) {
    return LocationOperand::cast(instr_->InputAt(index))->representation() ==
           MachineRepresentation::kWord64;
  }

  bool Is32BitOperand(int index) {
    return LocationOperand::cast(instr_->InputAt(index))->representation() ==
           MachineRepresentation::kWord32;
  }

  bool CompareLogical() const {
    switch (instr_->flags_condition()) {
      case kUnsignedLessThan:
      case kUnsignedGreaterThanOrEqual:
      case kUnsignedLessThanOrEqual:
      case kUnsignedGreaterThan:
        return true;
      default:
        return false;
    }
    UNREACHABLE();
    return false;
  }

  Operand InputImmediate(size_t index) {
    Constant constant = ToConstant(instr_->InputAt(index));
    switch (constant.type()) {
      case Constant::kInt32:
        return Operand(constant.ToInt32());
      case Constant::kFloat32:
        return Operand(
            isolate()->factory()->NewNumber(constant.ToFloat32(), TENURED));
      case Constant::kFloat64:
        return Operand(
            isolate()->factory()->NewNumber(constant.ToFloat64(), TENURED));
      case Constant::kInt64:
#if V8_TARGET_ARCH_S390X
        return Operand(constant.ToInt64());
#endif
      case Constant::kExternalReference:
      case Constant::kHeapObject:
      case Constant::kRpoNumber:
        break;
    }
    UNREACHABLE();
    return Operand::Zero();
  }

  MemOperand MemoryOperand(AddressingMode* mode, size_t* first_index) {
    const size_t index = *first_index;
    if (mode) *mode = AddressingModeField::decode(instr_->opcode());
    switch (AddressingModeField::decode(instr_->opcode())) {
      case kMode_None:
        break;
      case kMode_MR:
        *first_index += 1;
        return MemOperand(InputRegister(index + 0), 0);
      case kMode_MRI:
        *first_index += 2;
        return MemOperand(InputRegister(index + 0), InputInt32(index + 1));
      case kMode_MRR:
        *first_index += 2;
        return MemOperand(InputRegister(index + 0), InputRegister(index + 1));
      case kMode_MRRI:
        *first_index += 3;
        return MemOperand(InputRegister(index + 0), InputRegister(index + 1),
                          InputInt32(index + 2));
    }
    UNREACHABLE();
    return MemOperand(r0);
  }

  MemOperand MemoryOperand(AddressingMode* mode = NULL,
                           size_t first_index = 0) {
    return MemoryOperand(mode, &first_index);
  }

  MemOperand ToMemOperand(InstructionOperand* op) const {
    DCHECK_NOT_NULL(op);
    DCHECK(op->IsStackSlot() || op->IsFPStackSlot());
    return SlotToMemOperand(AllocatedOperand::cast(op)->index());
  }

  MemOperand SlotToMemOperand(int slot) const {
    FrameOffset offset = frame_access_state()->GetFrameOffset(slot);
    return MemOperand(offset.from_stack_pointer() ? sp : fp, offset.offset());
  }

  MemOperand InputStackSlot(size_t index) {
    InstructionOperand* op = instr_->InputAt(index);
    return SlotToMemOperand(AllocatedOperand::cast(op)->index());
  }

  MemOperand InputStackSlot32(size_t index) {
#if V8_TARGET_ARCH_S390X && !V8_TARGET_LITTLE_ENDIAN
    // We want to read the 32-bits directly from memory
    MemOperand mem = InputStackSlot(index);
    return MemOperand(mem.rb(), mem.rx(), mem.offset() + 4);
#else
    return InputStackSlot(index);
#endif
  }
};

static inline bool HasRegisterOutput(Instruction* instr, int index = 0) {
  return instr->OutputCount() > 0 && instr->OutputAt(index)->IsRegister();
}

static inline bool HasFPRegisterInput(Instruction* instr, int index) {
  return instr->InputAt(index)->IsFPRegister();
}

static inline bool HasRegisterInput(Instruction* instr, int index) {
  return instr->InputAt(index)->IsRegister() ||
         HasFPRegisterInput(instr, index);
}

static inline bool HasImmediateInput(Instruction* instr, size_t index) {
  return instr->InputAt(index)->IsImmediate();
}

static inline bool HasFPStackSlotInput(Instruction* instr, size_t index) {
  return instr->InputAt(index)->IsFPStackSlot();
}

static inline bool HasStackSlotInput(Instruction* instr, size_t index) {
  return instr->InputAt(index)->IsStackSlot() ||
         HasFPStackSlotInput(instr, index);
}

namespace {

class OutOfLineLoadNAN32 final : public OutOfLineCode {
 public:
  OutOfLineLoadNAN32(CodeGenerator* gen, DoubleRegister result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() final {
    __ LoadDoubleLiteral(result_, std::numeric_limits<float>::quiet_NaN(),
                         kScratchReg);
  }

 private:
  DoubleRegister const result_;
};

class OutOfLineLoadNAN64 final : public OutOfLineCode {
 public:
  OutOfLineLoadNAN64(CodeGenerator* gen, DoubleRegister result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() final {
    __ LoadDoubleLiteral(result_, std::numeric_limits<double>::quiet_NaN(),
                         kScratchReg);
  }

 private:
  DoubleRegister const result_;
};

class OutOfLineLoadZero final : public OutOfLineCode {
 public:
  OutOfLineLoadZero(CodeGenerator* gen, Register result)
      : OutOfLineCode(gen), result_(result) {}

  void Generate() final { __ LoadImmP(result_, Operand::Zero()); }

 private:
  Register const result_;
};

class OutOfLineRecordWrite final : public OutOfLineCode {
 public:
  OutOfLineRecordWrite(CodeGenerator* gen, Register object, Register offset,
                       Register value, Register scratch0, Register scratch1,
                       RecordWriteMode mode)
      : OutOfLineCode(gen),
        object_(object),
        offset_(offset),
        offset_immediate_(0),
        value_(value),
        scratch0_(scratch0),
        scratch1_(scratch1),
        mode_(mode),
        must_save_lr_(!gen->frame_access_state()->has_frame()) {}

  OutOfLineRecordWrite(CodeGenerator* gen, Register object, int32_t offset,
                       Register value, Register scratch0, Register scratch1,
                       RecordWriteMode mode)
      : OutOfLineCode(gen),
        object_(object),
        offset_(no_reg),
        offset_immediate_(offset),
        value_(value),
        scratch0_(scratch0),
        scratch1_(scratch1),
        mode_(mode),
        must_save_lr_(!gen->frame_access_state()->has_frame()) {}

  void Generate() final {
    if (mode_ > RecordWriteMode::kValueIsPointer) {
      __ JumpIfSmi(value_, exit());
    }
    __ CheckPageFlag(value_, scratch0_,
                     MemoryChunk::kPointersToHereAreInterestingMask, eq,
                     exit());
    RememberedSetAction const remembered_set_action =
        mode_ > RecordWriteMode::kValueIsMap ? EMIT_REMEMBERED_SET
                                             : OMIT_REMEMBERED_SET;
    SaveFPRegsMode const save_fp_mode =
        frame()->DidAllocateDoubleRegisters() ? kSaveFPRegs : kDontSaveFPRegs;
    if (must_save_lr_) {
      // We need to save and restore r14 if the frame was elided.
      __ Push(r14);
    }
    RecordWriteStub stub(isolate(), object_, scratch0_, scratch1_,
                         remembered_set_action, save_fp_mode);
    if (offset_.is(no_reg)) {
      __ AddP(scratch1_, object_, Operand(offset_immediate_));
    } else {
      DCHECK_EQ(0, offset_immediate_);
      __ AddP(scratch1_, object_, offset_);
    }
    __ CallStub(&stub);
    if (must_save_lr_) {
      // We need to save and restore r14 if the frame was elided.
      __ Pop(r14);
    }
  }

 private:
  Register const object_;
  Register const offset_;
  int32_t const offset_immediate_;  // Valid if offset_.is(no_reg).
  Register const value_;
  Register const scratch0_;
  Register const scratch1_;
  RecordWriteMode const mode_;
  bool must_save_lr_;
};

Condition FlagsConditionToCondition(FlagsCondition condition, ArchOpcode op) {
  switch (condition) {
    case kEqual:
      return eq;
    case kNotEqual:
      return ne;
    case kUnsignedLessThan:
      // unsigned number never less than 0
      if (op == kS390_LoadAndTestWord32 || op == kS390_LoadAndTestWord64)
        return CC_NOP;
    // fall through
    case kSignedLessThan:
      return lt;
    case kUnsignedGreaterThanOrEqual:
      // unsigned number always greater than or equal 0
      if (op == kS390_LoadAndTestWord32 || op == kS390_LoadAndTestWord64)
        return CC_ALWAYS;
    // fall through
    case kSignedGreaterThanOrEqual:
      return ge;
    case kUnsignedLessThanOrEqual:
      // unsigned number never less than 0
      if (op == kS390_LoadAndTestWord32 || op == kS390_LoadAndTestWord64)
        return CC_EQ;
    // fall through
    case kSignedLessThanOrEqual:
      return le;
    case kUnsignedGreaterThan:
      // unsigned number always greater than or equal 0
      if (op == kS390_LoadAndTestWord32 || op == kS390_LoadAndTestWord64)
        return ne;
    // fall through
    case kSignedGreaterThan:
      return gt;
    case kOverflow:
      // Overflow checked for AddP/SubP only.
      switch (op) {
        case kS390_Add32:
        case kS390_Add64:
        case kS390_Sub32:
        case kS390_Sub64:
        case kS390_Abs64:
        case kS390_Abs32:
        case kS390_Mul32:
          return overflow;
        default:
          break;
      }
      break;
    case kNotOverflow:
      switch (op) {
        case kS390_Add32:
        case kS390_Add64:
        case kS390_Sub32:
        case kS390_Sub64:
        case kS390_Abs64:
        case kS390_Abs32:
        case kS390_Mul32:
          return nooverflow;
        default:
          break;
      }
      break;
    default:
      break;
  }
  UNREACHABLE();
  return kNoCondition;
}

#define GET_MEMOPERAND32(ret, fi)                                       \
  ([&](int& ret) {                                                      \
    AddressingMode mode = AddressingModeField::decode(instr->opcode()); \
    MemOperand mem(r0);                                                 \
    if (mode != kMode_None) {                                           \
      size_t first_index = (fi);                                        \
      mem = i.MemoryOperand(&mode, &first_index);                       \
      ret = first_index;                                                \
    } else {                                                            \
      mem = i.InputStackSlot32(fi);                                     \
    }                                                                   \
    return mem;                                                         \
  })(ret)

#define GET_MEMOPERAND(ret, fi)                                         \
  ([&](int& ret) {                                                      \
    AddressingMode mode = AddressingModeField::decode(instr->opcode()); \
    MemOperand mem(r0);                                                 \
    if (mode != kMode_None) {                                           \
      size_t first_index = (fi);                                        \
      mem = i.MemoryOperand(&mode, &first_index);                       \
      ret = first_index;                                                \
    } else {                                                            \
      mem = i.InputStackSlot(fi);                                       \
    }                                                                   \
    return mem;                                                         \
  })(ret)

#define RRInstr(instr)                                 \
  [&]() {                                              \
    DCHECK(i.OutputRegister().is(i.InputRegister(0))); \
    __ instr(i.OutputRegister(), i.InputRegister(1));  \
    return 2;                                          \
  }
#define RIInstr(instr)                                 \
  [&]() {                                              \
    DCHECK(i.OutputRegister().is(i.InputRegister(0))); \
    __ instr(i.OutputRegister(), i.InputImmediate(1)); \
    return 2;                                          \
  }
#define RMInstr(instr, GETMEM)                         \
  [&]() {                                              \
    DCHECK(i.OutputRegister().is(i.InputRegister(0))); \
    int ret = 2;                                       \
    __ instr(i.OutputRegister(), GETMEM(ret, 1));      \
    return ret;                                        \
  }
#define RM32Instr(instr) RMInstr(instr, GET_MEMOPERAND32)
#define RM64Instr(instr) RMInstr(instr, GET_MEMOPERAND)

#define RRRInstr(instr)                                                   \
  [&]() {                                                                 \
    __ instr(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1)); \
    return 2;                                                             \
  }
#define RRIInstr(instr)                                                    \
  [&]() {                                                                  \
    __ instr(i.OutputRegister(), i.InputRegister(0), i.InputImmediate(1)); \
    return 2;                                                              \
  }
#define RRMInstr(instr, GETMEM)                                       \
  [&]() {                                                             \
    int ret = 2;                                                      \
    __ instr(i.OutputRegister(), i.InputRegister(0), GETMEM(ret, 1)); \
    return ret;                                                       \
  }
#define RRM32Instr(instr) RRMInstr(instr, GET_MEMOPERAND32)
#define RRM64Instr(instr) RRMInstr(instr, GET_MEMOPERAND)

#define DDInstr(instr)                                             \
  [&]() {                                                          \
    DCHECK(i.OutputDoubleRegister().is(i.InputDoubleRegister(0))); \
    __ instr(i.OutputDoubleRegister(), i.InputDoubleRegister(1));  \
    return 2;                                                      \
  }

#define DMInstr(instr)                                             \
  [&]() {                                                          \
    DCHECK(i.OutputDoubleRegister().is(i.InputDoubleRegister(0))); \
    int ret = 2;                                                   \
    __ instr(i.OutputDoubleRegister(), GET_MEMOPERAND(ret, 1));    \
    return ret;                                                    \
  }

#define DMTInstr(instr)                                            \
  [&]() {                                                          \
    DCHECK(i.OutputDoubleRegister().is(i.InputDoubleRegister(0))); \
    int ret = 2;                                                   \
    __ instr(i.OutputDoubleRegister(), GET_MEMOPERAND(ret, 1),     \
             kScratchDoubleReg);                                   \
    return ret;                                                    \
  }

#define R_MInstr(instr)                                   \
  [&]() {                                                 \
    int ret = 2;                                          \
    __ instr(i.OutputRegister(), GET_MEMOPERAND(ret, 0)); \
    return ret;                                           \
  }

#define R_DInstr(instr)                                     \
  [&]() {                                                   \
    __ instr(i.OutputRegister(), i.InputDoubleRegister(0)); \
    return 2;                                               \
  }

#define D_DInstr(instr)                                           \
  [&]() {                                                         \
    __ instr(i.OutputDoubleRegister(), i.InputDoubleRegister(0)); \
    return 2;                                                     \
  }

#define D_MInstr(instr)                                         \
  [&]() {                                                       \
    int ret = 2;                                                \
    __ instr(i.OutputDoubleRegister(), GET_MEMOPERAND(ret, 0)); \
    return ret;                                                 \
  }

#define D_MTInstr(instr)                                       \
  [&]() {                                                      \
    int ret = 2;                                               \
    __ instr(i.OutputDoubleRegister(), GET_MEMOPERAND(ret, 0), \
             kScratchDoubleReg);                               \
    return ret;                                                \
  }

static int nullInstr() {
  UNREACHABLE();
  return -1;
}

template <int numOfOperand, class RType, class MType, class IType>
static inline int AssembleOp(Instruction* instr, RType r, MType m, IType i) {
  AddressingMode mode = AddressingModeField::decode(instr->opcode());
  if (mode != kMode_None || HasStackSlotInput(instr, numOfOperand - 1)) {
    return m();
  } else if (HasRegisterInput(instr, numOfOperand - 1)) {
    return r();
  } else if (HasImmediateInput(instr, numOfOperand - 1)) {
    return i();
  } else {
    UNREACHABLE();
    return -1;
  }
}

template <class _RR, class _RM, class _RI>
static inline int AssembleBinOp(Instruction* instr, _RR _rr, _RM _rm, _RI _ri) {
  return AssembleOp<2>(instr, _rr, _rm, _ri);
}

template <class _R, class _M, class _I>
static inline int AssembleUnaryOp(Instruction* instr, _R _r, _M _m, _I _i) {
  return AssembleOp<1>(instr, _r, _m, _i);
}

#define ASSEMBLE_BIN_OP(_rr, _rm, _ri) AssembleBinOp(instr, _rr, _rm, _ri)
#define ASSEMBLE_UNARY_OP(_r, _m, _i) AssembleUnaryOp(instr, _r, _m, _i)

#ifdef V8_TARGET_ARCH_S390X
#define CHECK_AND_ZERO_EXT_OUTPUT(num)                                \
  ([&](int index) {                                                   \
    DCHECK(HasImmediateInput(instr, (index)));                        \
    int doZeroExt = i.InputInt32(index);                              \
    if (doZeroExt) __ LoadlW(i.OutputRegister(), i.OutputRegister()); \
  })(num)

#define ASSEMBLE_BIN32_OP(_rr, _rm, _ri) \
  { CHECK_AND_ZERO_EXT_OUTPUT(AssembleBinOp(instr, _rr, _rm, _ri)); }
#else
#define ASSEMBLE_BIN32_OP ASSEMBLE_BIN_OP
#define CHECK_AND_ZERO_EXT_OUTPUT(num)
#endif

}  // namespace

#define ASSEMBLE_FLOAT_UNOP(asm_instr)                                \
  do {                                                                \
    __ asm_instr(i.OutputDoubleRegister(), i.InputDoubleRegister(0)); \
  } while (0)

#define ASSEMBLE_FLOAT_BINOP(asm_instr)                              \
  do {                                                               \
    __ asm_instr(i.OutputDoubleRegister(), i.InputDoubleRegister(0), \
                 i.InputDoubleRegister(1));                          \
  } while (0)

#define ASSEMBLE_COMPARE(cmp_instr, cmpl_instr)                         \
  do {                                                                  \
    AddressingMode mode = AddressingModeField::decode(instr->opcode()); \
    if (mode != kMode_None) {                                           \
      size_t first_index = 1;                                           \
      MemOperand operand = i.MemoryOperand(&mode, &first_index);        \
      if (i.CompareLogical()) {                                         \
        __ cmpl_instr(i.InputRegister(0), operand);                     \
      } else {                                                          \
        __ cmp_instr(i.InputRegister(0), operand);                      \
      }                                                                 \
    } else if (HasRegisterInput(instr, 1)) {                            \
      if (i.CompareLogical()) {                                         \
        __ cmpl_instr(i.InputRegister(0), i.InputRegister(1));          \
      } else {                                                          \
        __ cmp_instr(i.InputRegister(0), i.InputRegister(1));           \
      }                                                                 \
    } else if (HasImmediateInput(instr, 1)) {                           \
      if (i.CompareLogical()) {                                         \
        __ cmpl_instr(i.InputRegister(0), i.InputImmediate(1));         \
      } else {                                                          \
        __ cmp_instr(i.InputRegister(0), i.InputImmediate(1));          \
      }                                                                 \
    } else {                                                            \
      DCHECK(HasStackSlotInput(instr, 1));                              \
      if (i.CompareLogical()) {                                         \
        __ cmpl_instr(i.InputRegister(0), i.InputStackSlot(1));         \
      } else {                                                          \
        __ cmp_instr(i.InputRegister(0), i.InputStackSlot(1));          \
      }                                                                 \
    }                                                                   \
  } while (0)

#define ASSEMBLE_COMPARE32(cmp_instr, cmpl_instr)                       \
  do {                                                                  \
    AddressingMode mode = AddressingModeField::decode(instr->opcode()); \
    if (mode != kMode_None) {                                           \
      size_t first_index = 1;                                           \
      MemOperand operand = i.MemoryOperand(&mode, &first_index);        \
      if (i.CompareLogical()) {                                         \
        __ cmpl_instr(i.InputRegister(0), operand);                     \
      } else {                                                          \
        __ cmp_instr(i.InputRegister(0), operand);                      \
      }                                                                 \
    } else if (HasRegisterInput(instr, 1)) {                            \
      if (i.CompareLogical()) {                                         \
        __ cmpl_instr(i.InputRegister(0), i.InputRegister(1));          \
      } else {                                                          \
        __ cmp_instr(i.InputRegister(0), i.InputRegister(1));           \
      }                                                                 \
    } else if (HasImmediateInput(instr, 1)) {                           \
      if (i.CompareLogical()) {                                         \
        __ cmpl_instr(i.InputRegister(0), i.InputImmediate(1));         \
      } else {                                                          \
        __ cmp_instr(i.InputRegister(0), i.InputImmediate(1));          \
      }                                                                 \
    } else {                                                            \
      DCHECK(HasStackSlotInput(instr, 1));                              \
      if (i.CompareLogical()) {                                         \
        __ cmpl_instr(i.InputRegister(0), i.InputStackSlot32(1));       \
      } else {                                                          \
        __ cmp_instr(i.InputRegister(0), i.InputStackSlot32(1));        \
      }                                                                 \
    }                                                                   \
  } while (0)

#define ASSEMBLE_FLOAT_COMPARE(cmp_rr_instr, cmp_rm_instr, load_instr)     \
  do {                                                                     \
    AddressingMode mode = AddressingModeField::decode(instr->opcode());    \
    if (mode != kMode_None) {                                              \
      size_t first_index = 1;                                              \
      MemOperand operand = i.MemoryOperand(&mode, &first_index);           \
      __ cmp_rm_instr(i.InputDoubleRegister(0), operand);                  \
    } else if (HasFPRegisterInput(instr, 1)) {                             \
      __ cmp_rr_instr(i.InputDoubleRegister(0), i.InputDoubleRegister(1)); \
    } else {                                                               \
      USE(HasFPStackSlotInput);                                            \
      DCHECK(HasFPStackSlotInput(instr, 1));                               \
      MemOperand operand = i.InputStackSlot(1);                            \
      if (operand.offset() >= 0) {                                         \
        __ cmp_rm_instr(i.InputDoubleRegister(0), operand);                \
      } else {                                                             \
        __ load_instr(kScratchDoubleReg, operand);                         \
        __ cmp_rr_instr(i.InputDoubleRegister(0), kScratchDoubleReg);      \
      }                                                                    \
    }                                                                      \
  } while (0)

// Divide instruction dr will implicity use register pair
// r0 & r1 below.
// R0:R1 = R1 / divisor - R0 remainder
// Copy remainder to output reg
#define ASSEMBLE_MODULO(div_instr, shift_instr) \
  do {                                          \
    __ LoadRR(r0, i.InputRegister(0));          \
    __ shift_instr(r0, Operand(32));            \
    __ div_instr(r0, i.InputRegister(1));       \
    __ LoadlW(i.OutputRegister(), r0);          \
  } while (0)

#define ASSEMBLE_FLOAT_MODULO()                                               \
  do {                                                                        \
    FrameScope scope(masm(), StackFrame::MANUAL);                             \
    __ PrepareCallCFunction(0, 2, kScratchReg);                               \
    __ MovToFloatParameters(i.InputDoubleRegister(0),                         \
                            i.InputDoubleRegister(1));                        \
    __ CallCFunction(ExternalReference::mod_two_doubles_operation(isolate()), \
                     0, 2);                                                   \
    __ MovFromFloatResult(i.OutputDoubleRegister());                          \
  } while (0)

#define ASSEMBLE_IEEE754_UNOP(name)                                            \
  do {                                                                         \
    /* TODO(bmeurer): We should really get rid of this special instruction, */ \
    /* and generate a CallAddress instruction instead. */                      \
    FrameScope scope(masm(), StackFrame::MANUAL);                              \
    __ PrepareCallCFunction(0, 1, kScratchReg);                                \
    __ MovToFloatParameter(i.InputDoubleRegister(0));                          \
    __ CallCFunction(ExternalReference::ieee754_##name##_function(isolate()),  \
                     0, 1);                                                    \
    /* Move the result in the double result register. */                       \
    __ MovFromFloatResult(i.OutputDoubleRegister());                           \
  } while (0)

#define ASSEMBLE_IEEE754_BINOP(name)                                           \
  do {                                                                         \
    /* TODO(bmeurer): We should really get rid of this special instruction, */ \
    /* and generate a CallAddress instruction instead. */                      \
    FrameScope scope(masm(), StackFrame::MANUAL);                              \
    __ PrepareCallCFunction(0, 2, kScratchReg);                                \
    __ MovToFloatParameters(i.InputDoubleRegister(0),                          \
                            i.InputDoubleRegister(1));                         \
    __ CallCFunction(ExternalReference::ieee754_##name##_function(isolate()),  \
                     0, 2);                                                    \
    /* Move the result in the double result register. */                       \
    __ MovFromFloatResult(i.OutputDoubleRegister());                           \
  } while (0)

#define ASSEMBLE_DOUBLE_MAX()                                          \
  do {                                                                 \
    DoubleRegister left_reg = i.InputDoubleRegister(0);                \
    DoubleRegister right_reg = i.InputDoubleRegister(1);               \
    DoubleRegister result_reg = i.OutputDoubleRegister();              \
    Label check_nan_left, check_zero, return_left, return_right, done; \
    __ cdbr(left_reg, right_reg);                                      \
    __ bunordered(&check_nan_left, Label::kNear);                      \
    __ beq(&check_zero);                                               \
    __ bge(&return_left, Label::kNear);                                \
    __ b(&return_right, Label::kNear);                                 \
                                                                       \
    __ bind(&check_zero);                                              \
    __ lzdr(kDoubleRegZero);                                           \
    __ cdbr(left_reg, kDoubleRegZero);                                 \
    /* left == right != 0. */                                          \
    __ bne(&return_left, Label::kNear);                                \
    /* At this point, both left and right are either 0 or -0. */       \
    /* N.B. The following works because +0 + -0 == +0 */               \
    /* For max we want logical-and of sign bit: (L + R) */             \
    __ ldr(result_reg, left_reg);                                      \
    __ adbr(result_reg, right_reg);                                    \
    __ b(&done, Label::kNear);                                         \
                                                                       \
    __ bind(&check_nan_left);                                          \
    __ cdbr(left_reg, left_reg);                                       \
    /* left == NaN. */                                                 \
    __ bunordered(&return_left, Label::kNear);                         \
                                                                       \
    __ bind(&return_right);                                            \
    if (!right_reg.is(result_reg)) {                                   \
      __ ldr(result_reg, right_reg);                                   \
    }                                                                  \
    __ b(&done, Label::kNear);                                         \
                                                                       \
    __ bind(&return_left);                                             \
    if (!left_reg.is(result_reg)) {                                    \
      __ ldr(result_reg, left_reg);                                    \
    }                                                                  \
    __ bind(&done);                                                    \
  } while (0)

#define ASSEMBLE_DOUBLE_MIN()                                          \
  do {                                                                 \
    DoubleRegister left_reg = i.InputDoubleRegister(0);                \
    DoubleRegister right_reg = i.InputDoubleRegister(1);               \
    DoubleRegister result_reg = i.OutputDoubleRegister();              \
    Label check_nan_left, check_zero, return_left, return_right, done; \
    __ cdbr(left_reg, right_reg);                                      \
    __ bunordered(&check_nan_left, Label::kNear);                      \
    __ beq(&check_zero);                                               \
    __ ble(&return_left, Label::kNear);                                \
    __ b(&return_right, Label::kNear);                                 \
                                                                       \
    __ bind(&check_zero);                                              \
    __ lzdr(kDoubleRegZero);                                           \
    __ cdbr(left_reg, kDoubleRegZero);                                 \
    /* left == right != 0. */                                          \
    __ bne(&return_left, Label::kNear);                                \
    /* At this point, both left and right are either 0 or -0. */       \
    /* N.B. The following works because +0 + -0 == +0 */               \
    /* For min we want logical-or of sign bit: -(-L + -R) */           \
    __ lcdbr(left_reg, left_reg);                                      \
    __ ldr(result_reg, left_reg);                                      \
    if (left_reg.is(right_reg)) {                                      \
      __ adbr(result_reg, right_reg);                                  \
    } else {                                                           \
      __ sdbr(result_reg, right_reg);                                  \
    }                                                                  \
    __ lcdbr(result_reg, result_reg);                                  \
    __ b(&done, Label::kNear);                                         \
                                                                       \
    __ bind(&check_nan_left);                                          \
    __ cdbr(left_reg, left_reg);                                       \
    /* left == NaN. */                                                 \
    __ bunordered(&return_left, Label::kNear);                         \
                                                                       \
    __ bind(&return_right);                                            \
    if (!right_reg.is(result_reg)) {                                   \
      __ ldr(result_reg, right_reg);                                   \
    }                                                                  \
    __ b(&done, Label::kNear);                                         \
                                                                       \
    __ bind(&return_left);                                             \
    if (!left_reg.is(result_reg)) {                                    \
      __ ldr(result_reg, left_reg);                                    \
    }                                                                  \
    __ bind(&done);                                                    \
  } while (0)

#define ASSEMBLE_FLOAT_MAX()                                           \
  do {                                                                 \
    DoubleRegister left_reg = i.InputDoubleRegister(0);                \
    DoubleRegister right_reg = i.InputDoubleRegister(1);               \
    DoubleRegister result_reg = i.OutputDoubleRegister();              \
    Label check_nan_left, check_zero, return_left, return_right, done; \
    __ cebr(left_reg, right_reg);                                      \
    __ bunordered(&check_nan_left, Label::kNear);                      \
    __ beq(&check_zero);                                               \
    __ bge(&return_left, Label::kNear);                                \
    __ b(&return_right, Label::kNear);                                 \
                                                                       \
    __ bind(&check_zero);                                              \
    __ lzdr(kDoubleRegZero);                                           \
    __ cebr(left_reg, kDoubleRegZero);                                 \
    /* left == right != 0. */                                          \
    __ bne(&return_left, Label::kNear);                                \
    /* At this point, both left and right are either 0 or -0. */       \
    /* N.B. The following works because +0 + -0 == +0 */               \
    /* For max we want logical-and of sign bit: (L + R) */             \
    __ ldr(result_reg, left_reg);                                      \
    __ aebr(result_reg, right_reg);                                    \
    __ b(&done, Label::kNear);                                         \
                                                                       \
    __ bind(&check_nan_left);                                          \
    __ cebr(left_reg, left_reg);                                       \
    /* left == NaN. */                                                 \
    __ bunordered(&return_left, Label::kNear);                         \
                                                                       \
    __ bind(&return_right);                                            \
    if (!right_reg.is(result_reg)) {                                   \
      __ ldr(result_reg, right_reg);                                   \
    }                                                                  \
    __ b(&done, Label::kNear);                                         \
                                                                       \
    __ bind(&return_left);                                             \
    if (!left_reg.is(result_reg)) {                                    \
      __ ldr(result_reg, left_reg);                                    \
    }                                                                  \
    __ bind(&done);                                                    \
  } while (0)

#define ASSEMBLE_FLOAT_MIN()                                           \
  do {                                                                 \
    DoubleRegister left_reg = i.InputDoubleRegister(0);                \
    DoubleRegister right_reg = i.InputDoubleRegister(1);               \
    DoubleRegister result_reg = i.OutputDoubleRegister();              \
    Label check_nan_left, check_zero, return_left, return_right, done; \
    __ cebr(left_reg, right_reg);                                      \
    __ bunordered(&check_nan_left, Label::kNear);                      \
    __ beq(&check_zero);                                               \
    __ ble(&return_left, Label::kNear);                                \
    __ b(&return_right, Label::kNear);                                 \
                                                                       \
    __ bind(&check_zero);                                              \
    __ lzdr(kDoubleRegZero);                                           \
    __ cebr(left_reg, kDoubleRegZero);                                 \
    /* left == right != 0. */                                          \
    __ bne(&return_left, Label::kNear);                                \
    /* At this point, both left and right are either 0 or -0. */       \
    /* N.B. The following works because +0 + -0 == +0 */               \
    /* For min we want logical-or of sign bit: -(-L + -R) */           \
    __ lcebr(left_reg, left_reg);                                      \
    __ ldr(result_reg, left_reg);                                      \
    if (left_reg.is(right_reg)) {                                      \
      __ aebr(result_reg, right_reg);                                  \
    } else {                                                           \
      __ sebr(result_reg, right_reg);                                  \
    }                                                                  \
    __ lcebr(result_reg, result_reg);                                  \
    __ b(&done, Label::kNear);                                         \
                                                                       \
    __ bind(&check_nan_left);                                          \
    __ cebr(left_reg, left_reg);                                       \
    /* left == NaN. */                                                 \
    __ bunordered(&return_left, Label::kNear);                         \
                                                                       \
    __ bind(&return_right);                                            \
    if (!right_reg.is(result_reg)) {                                   \
      __ ldr(result_reg, right_reg);                                   \
    }                                                                  \
    __ b(&done, Label::kNear);                                         \
                                                                       \
    __ bind(&return_left);                                             \
    if (!left_reg.is(result_reg)) {                                    \
      __ ldr(result_reg, left_reg);                                    \
    }                                                                  \
    __ bind(&done);                                                    \
  } while (0)
//
// Only MRI mode for these instructions available
#define ASSEMBLE_LOAD_FLOAT(asm_instr)                \
  do {                                                \
    DoubleRegister result = i.OutputDoubleRegister(); \
    AddressingMode mode = kMode_None;                 \
    MemOperand operand = i.MemoryOperand(&mode);      \
    __ asm_instr(result, operand);                    \
  } while (0)

#define ASSEMBLE_LOAD_INTEGER(asm_instr)         \
  do {                                           \
    Register result = i.OutputRegister();        \
    AddressingMode mode = kMode_None;            \
    MemOperand operand = i.MemoryOperand(&mode); \
    __ asm_instr(result, operand);               \
  } while (0)

#define ASSEMBLE_LOADANDTEST64(asm_instr_rr, asm_instr_rm)              \
  {                                                                     \
    AddressingMode mode = AddressingModeField::decode(instr->opcode()); \
    Register dst = HasRegisterOutput(instr) ? i.OutputRegister() : r0;  \
    if (mode != kMode_None) {                                           \
      size_t first_index = 0;                                           \
      MemOperand operand = i.MemoryOperand(&mode, &first_index);        \
      __ asm_instr_rm(dst, operand);                                    \
    } else if (HasRegisterInput(instr, 0)) {                            \
      __ asm_instr_rr(dst, i.InputRegister(0));                         \
    } else {                                                            \
      DCHECK(HasStackSlotInput(instr, 0));                              \
      __ asm_instr_rm(dst, i.InputStackSlot(0));                        \
    }                                                                   \
  }

#define ASSEMBLE_LOADANDTEST32(asm_instr_rr, asm_instr_rm)              \
  {                                                                     \
    AddressingMode mode = AddressingModeField::decode(instr->opcode()); \
    Register dst = HasRegisterOutput(instr) ? i.OutputRegister() : r0;  \
    if (mode != kMode_None) {                                           \
      size_t first_index = 0;                                           \
      MemOperand operand = i.MemoryOperand(&mode, &first_index);        \
      __ asm_instr_rm(dst, operand);                                    \
    } else if (HasRegisterInput(instr, 0)) {                            \
      __ asm_instr_rr(dst, i.InputRegister(0));                         \
    } else {                                                            \
      DCHECK(HasStackSlotInput(instr, 0));                              \
      __ asm_instr_rm(dst, i.InputStackSlot32(0));                      \
    }                                                                   \
  }

#define ASSEMBLE_STORE_FLOAT32()                         \
  do {                                                   \
    size_t index = 0;                                    \
    AddressingMode mode = kMode_None;                    \
    MemOperand operand = i.MemoryOperand(&mode, &index); \
    DoubleRegister value = i.InputDoubleRegister(index); \
    __ StoreFloat32(value, operand);                     \
  } while (0)

#define ASSEMBLE_STORE_DOUBLE()                          \
  do {                                                   \
    size_t index = 0;                                    \
    AddressingMode mode = kMode_None;                    \
    MemOperand operand = i.MemoryOperand(&mode, &index); \
    DoubleRegister value = i.InputDoubleRegister(index); \
    __ StoreDouble(value, operand);                      \
  } while (0)

#define ASSEMBLE_STORE_INTEGER(asm_instr)                \
  do {                                                   \
    size_t index = 0;                                    \
    AddressingMode mode = kMode_None;                    \
    MemOperand operand = i.MemoryOperand(&mode, &index); \
    Register value = i.InputRegister(index);             \
    __ asm_instr(value, operand);                        \
  } while (0)

#define ASSEMBLE_CHECKED_LOAD_FLOAT(asm_instr, width)              \
  do {                                                             \
    DoubleRegister result = i.OutputDoubleRegister();              \
    size_t index = 0;                                              \
    AddressingMode mode = kMode_None;                              \
    MemOperand operand = i.MemoryOperand(&mode, index);            \
    Register offset = operand.rb();                                \
    if (HasRegisterInput(instr, 2)) {                              \
      __ CmpLogical32(offset, i.InputRegister(2));                 \
    } else {                                                       \
      __ CmpLogical32(offset, i.InputImmediate(2));                \
    }                                                              \
    auto ool = new (zone()) OutOfLineLoadNAN##width(this, result); \
    __ bge(ool->entry());                                          \
    __ CleanUInt32(offset);                                        \
    __ asm_instr(result, operand);                                 \
    __ bind(ool->exit());                                          \
  } while (0)

#define ASSEMBLE_CHECKED_LOAD_INTEGER(asm_instr)             \
  do {                                                       \
    Register result = i.OutputRegister();                    \
    size_t index = 0;                                        \
    AddressingMode mode = kMode_None;                        \
    MemOperand operand = i.MemoryOperand(&mode, index);      \
    Register offset = operand.rb();                          \
    if (HasRegisterInput(instr, 2)) {                        \
      __ CmpLogical32(offset, i.InputRegister(2));           \
    } else {                                                 \
      __ CmpLogical32(offset, i.InputImmediate(2));          \
    }                                                        \
    auto ool = new (zone()) OutOfLineLoadZero(this, result); \
    __ bge(ool->entry());                                    \
    __ CleanUInt32(offset);                                  \
    __ asm_instr(result, operand);                           \
    __ bind(ool->exit());                                    \
  } while (0)

#define ASSEMBLE_CHECKED_STORE_FLOAT32()                \
  do {                                                  \
    Label done;                                         \
    size_t index = 0;                                   \
    AddressingMode mode = kMode_None;                   \
    MemOperand operand = i.MemoryOperand(&mode, index); \
    Register offset = operand.rb();                     \
    if (HasRegisterInput(instr, 2)) {                   \
      __ CmpLogical32(offset, i.InputRegister(2));      \
    } else {                                            \
      __ CmpLogical32(offset, i.InputImmediate(2));     \
    }                                                   \
    __ bge(&done);                                      \
    DoubleRegister value = i.InputDoubleRegister(3);    \
    __ CleanUInt32(offset);                             \
    __ StoreFloat32(value, operand);                    \
    __ bind(&done);                                     \
  } while (0)

#define ASSEMBLE_CHECKED_STORE_DOUBLE()                 \
  do {                                                  \
    Label done;                                         \
    size_t index = 0;                                   \
    AddressingMode mode = kMode_None;                   \
    MemOperand operand = i.MemoryOperand(&mode, index); \
    DCHECK_EQ(kMode_MRR, mode);                         \
    Register offset = operand.rb();                     \
    if (HasRegisterInput(instr, 2)) {                   \
      __ CmpLogical32(offset, i.InputRegister(2));      \
    } else {                                            \
      __ CmpLogical32(offset, i.InputImmediate(2));     \
    }                                                   \
    __ bge(&done);                                      \
    DoubleRegister value = i.InputDoubleRegister(3);    \
    __ CleanUInt32(offset);                             \
    __ StoreDouble(value, operand);                     \
    __ bind(&done);                                     \
  } while (0)

#define ASSEMBLE_CHECKED_STORE_INTEGER(asm_instr)       \
  do {                                                  \
    Label done;                                         \
    size_t index = 0;                                   \
    AddressingMode mode = kMode_None;                   \
    MemOperand operand = i.MemoryOperand(&mode, index); \
    Register offset = operand.rb();                     \
    if (HasRegisterInput(instr, 2)) {                   \
      __ CmpLogical32(offset, i.InputRegister(2));      \
    } else {                                            \
      __ CmpLogical32(offset, i.InputImmediate(2));     \
    }                                                   \
    __ bge(&done);                                      \
    Register value = i.InputRegister(3);                \
    __ CleanUInt32(offset);                             \
    __ asm_instr(value, operand);                       \
    __ bind(&done);                                     \
  } while (0)

void CodeGenerator::AssembleDeconstructFrame() {
  __ LeaveFrame(StackFrame::MANUAL);
}

void CodeGenerator::AssemblePrepareTailCall() {
  if (frame_access_state()->has_frame()) {
    __ RestoreFrameStateForTailCall();
  }
  frame_access_state()->SetFrameAccessToSP();
}

void CodeGenerator::AssemblePopArgumentsAdaptorFrame(Register args_reg,
                                                     Register scratch1,
                                                     Register scratch2,
                                                     Register scratch3) {
  DCHECK(!AreAliased(args_reg, scratch1, scratch2, scratch3));
  Label done;

  // Check if current frame is an arguments adaptor frame.
  __ LoadP(scratch1, MemOperand(fp, StandardFrameConstants::kContextOffset));
  __ CmpP(scratch1,
          Operand(StackFrame::TypeToMarker(StackFrame::ARGUMENTS_ADAPTOR)));
  __ bne(&done);

  // Load arguments count from current arguments adaptor frame (note, it
  // does not include receiver).
  Register caller_args_count_reg = scratch1;
  __ LoadP(caller_args_count_reg,
           MemOperand(fp, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ SmiUntag(caller_args_count_reg);

  ParameterCount callee_args_count(args_reg);
  __ PrepareForTailCall(callee_args_count, caller_args_count_reg, scratch2,
                        scratch3);
  __ bind(&done);
}

namespace {

void FlushPendingPushRegisters(MacroAssembler* masm,
                               FrameAccessState* frame_access_state,
                               ZoneVector<Register>* pending_pushes) {
  switch (pending_pushes->size()) {
    case 0:
      break;
    case 1:
      masm->Push((*pending_pushes)[0]);
      break;
    case 2:
      masm->Push((*pending_pushes)[0], (*pending_pushes)[1]);
      break;
    case 3:
      masm->Push((*pending_pushes)[0], (*pending_pushes)[1],
                 (*pending_pushes)[2]);
      break;
    default:
      UNREACHABLE();
      break;
  }
  frame_access_state->IncreaseSPDelta(pending_pushes->size());
  pending_pushes->resize(0);
}

void AddPendingPushRegister(MacroAssembler* masm,
                            FrameAccessState* frame_access_state,
                            ZoneVector<Register>* pending_pushes,
                            Register reg) {
  pending_pushes->push_back(reg);
  if (pending_pushes->size() == 3 || reg.is(ip)) {
    FlushPendingPushRegisters(masm, frame_access_state, pending_pushes);
  }
}
void AdjustStackPointerForTailCall(
    MacroAssembler* masm, FrameAccessState* state, int new_slot_above_sp,
    ZoneVector<Register>* pending_pushes = nullptr,
    bool allow_shrinkage = true) {
  int current_sp_offset = state->GetSPToFPSlotCount() +
                          StandardFrameConstants::kFixedSlotCountAboveFp;
  int stack_slot_delta = new_slot_above_sp - current_sp_offset;
  if (stack_slot_delta > 0) {
    if (pending_pushes != nullptr) {
      FlushPendingPushRegisters(masm, state, pending_pushes);
    }
    masm->AddP(sp, sp, Operand(-stack_slot_delta * kPointerSize));
    state->IncreaseSPDelta(stack_slot_delta);
  } else if (allow_shrinkage && stack_slot_delta < 0) {
    if (pending_pushes != nullptr) {
      FlushPendingPushRegisters(masm, state, pending_pushes);
    }
    masm->AddP(sp, sp, Operand(-stack_slot_delta * kPointerSize));
    state->IncreaseSPDelta(stack_slot_delta);
  }
}

}  // namespace

void CodeGenerator::AssembleTailCallBeforeGap(Instruction* instr,
                                              int first_unused_stack_slot) {
  CodeGenerator::PushTypeFlags flags(kImmediatePush | kScalarPush);
  ZoneVector<MoveOperands*> pushes(zone());
  GetPushCompatibleMoves(instr, flags, &pushes);

  if (!pushes.empty() &&
      (LocationOperand::cast(pushes.back()->destination()).index() + 1 ==
       first_unused_stack_slot)) {
    S390OperandConverter g(this, instr);
    ZoneVector<Register> pending_pushes(zone());
    for (auto move : pushes) {
      LocationOperand destination_location(
          LocationOperand::cast(move->destination()));
      InstructionOperand source(move->source());
      AdjustStackPointerForTailCall(
          masm(), frame_access_state(),
          destination_location.index() - pending_pushes.size(),
          &pending_pushes);
      if (source.IsStackSlot()) {
        LocationOperand source_location(LocationOperand::cast(source));
        __ LoadP(ip, g.SlotToMemOperand(source_location.index()));
        AddPendingPushRegister(masm(), frame_access_state(), &pending_pushes,
                               ip);
      } else if (source.IsRegister()) {
        LocationOperand source_location(LocationOperand::cast(source));
        AddPendingPushRegister(masm(), frame_access_state(), &pending_pushes,
                               source_location.GetRegister());
      } else if (source.IsImmediate()) {
        AddPendingPushRegister(masm(), frame_access_state(), &pending_pushes,
                               ip);
      } else {
        // Pushes of non-scalar data types is not supported.
        UNIMPLEMENTED();
      }
      move->Eliminate();
    }
    FlushPendingPushRegisters(masm(), frame_access_state(), &pending_pushes);
  }
  AdjustStackPointerForTailCall(masm(), frame_access_state(),
                                first_unused_stack_slot, nullptr, false);
}

void CodeGenerator::AssembleTailCallAfterGap(Instruction* instr,
                                             int first_unused_stack_slot) {
  AdjustStackPointerForTailCall(masm(), frame_access_state(),
                                first_unused_stack_slot);
}

// Assembles an instruction after register allocation, producing machine code.
CodeGenerator::CodeGenResult CodeGenerator::AssembleArchInstruction(
    Instruction* instr) {
  S390OperandConverter i(this, instr);
  ArchOpcode opcode = ArchOpcodeField::decode(instr->opcode());

  switch (opcode) {
    case kArchComment: {
      Address comment_string = i.InputExternalReference(0).address();
      __ RecordComment(reinterpret_cast<const char*>(comment_string));
      break;
    }
    case kArchCallCodeObject: {
      EnsureSpaceForLazyDeopt();
      if (HasRegisterInput(instr, 0)) {
        __ AddP(ip, i.InputRegister(0),
                Operand(Code::kHeaderSize - kHeapObjectTag));
        __ Call(ip);
      } else {
        __ Call(Handle<Code>::cast(i.InputHeapObject(0)),
                RelocInfo::CODE_TARGET);
      }
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchTailCallCodeObjectFromJSFunction:
    case kArchTailCallCodeObject: {
      if (opcode == kArchTailCallCodeObjectFromJSFunction) {
        AssemblePopArgumentsAdaptorFrame(kJavaScriptCallArgCountRegister,
                                         i.TempRegister(0), i.TempRegister(1),
                                         i.TempRegister(2));
      }
      if (HasRegisterInput(instr, 0)) {
        __ AddP(ip, i.InputRegister(0),
                Operand(Code::kHeaderSize - kHeapObjectTag));
        __ Jump(ip);
      } else {
        // We cannot use the constant pool to load the target since
        // we've already restored the caller's frame.
        ConstantPoolUnavailableScope constant_pool_unavailable(masm());
        __ Jump(Handle<Code>::cast(i.InputHeapObject(0)),
                RelocInfo::CODE_TARGET);
      }
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
    case kArchTailCallAddress: {
      CHECK(!instr->InputAt(0)->IsImmediate());
      __ Jump(i.InputRegister(0));
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
    case kArchCallJSFunction: {
      EnsureSpaceForLazyDeopt();
      Register func = i.InputRegister(0);
      if (FLAG_debug_code) {
        // Check the function's context matches the context argument.
        __ LoadP(kScratchReg,
                 FieldMemOperand(func, JSFunction::kContextOffset));
        __ CmpP(cp, kScratchReg);
        __ Assert(eq, kWrongFunctionContext);
      }
      __ LoadP(ip, FieldMemOperand(func, JSFunction::kCodeEntryOffset));
      __ Call(ip);
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchTailCallJSFunctionFromJSFunction: {
      Register func = i.InputRegister(0);
      if (FLAG_debug_code) {
        // Check the function's context matches the context argument.
        __ LoadP(kScratchReg,
                 FieldMemOperand(func, JSFunction::kContextOffset));
        __ CmpP(cp, kScratchReg);
        __ Assert(eq, kWrongFunctionContext);
      }
      AssemblePopArgumentsAdaptorFrame(kJavaScriptCallArgCountRegister,
                                       i.TempRegister(0), i.TempRegister(1),
                                       i.TempRegister(2));
      __ LoadP(ip, FieldMemOperand(func, JSFunction::kCodeEntryOffset));
      __ Jump(ip);
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    }
    case kArchPrepareCallCFunction: {
      int const num_parameters = MiscField::decode(instr->opcode());
      __ PrepareCallCFunction(num_parameters, kScratchReg);
      // Frame alignment requires using FP-relative frame addressing.
      frame_access_state()->SetFrameAccessToFP();
      break;
    }
    case kArchPrepareTailCall:
      AssemblePrepareTailCall();
      break;
    case kArchCallCFunction: {
      int const num_parameters = MiscField::decode(instr->opcode());
      if (instr->InputAt(0)->IsImmediate()) {
        ExternalReference ref = i.InputExternalReference(0);
        __ CallCFunction(ref, num_parameters);
      } else {
        Register func = i.InputRegister(0);
        __ CallCFunction(func, num_parameters);
      }
      frame_access_state()->SetFrameAccessToDefault();
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchJmp:
      AssembleArchJump(i.InputRpo(0));
      break;
    case kArchLookupSwitch:
      AssembleArchLookupSwitch(instr);
      break;
    case kArchTableSwitch:
      AssembleArchTableSwitch(instr);
      break;
    case kArchDebugBreak:
      __ stop("kArchDebugBreak");
      break;
    case kArchNop:
    case kArchThrowTerminator:
      // don't emit code for nops.
      break;
    case kArchDeoptimize: {
      int deopt_state_id =
          BuildTranslation(instr, -1, 0, OutputFrameStateCombine::Ignore());
      CodeGenResult result =
          AssembleDeoptimizerCall(deopt_state_id, current_source_position_);
      if (result != kSuccess) return result;
      break;
    }
    case kArchRet:
      AssembleReturn(instr->InputAt(0));
      break;
    case kArchStackPointer:
      __ LoadRR(i.OutputRegister(), sp);
      break;
    case kArchFramePointer:
      __ LoadRR(i.OutputRegister(), fp);
      break;
    case kArchParentFramePointer:
      if (frame_access_state()->has_frame()) {
        __ LoadP(i.OutputRegister(), MemOperand(fp, 0));
      } else {
        __ LoadRR(i.OutputRegister(), fp);
      }
      break;
    case kArchTruncateDoubleToI:
      // TODO(mbrandy): move slow call to stub out of line.
      __ TruncateDoubleToI(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kArchStoreWithWriteBarrier: {
      RecordWriteMode mode =
          static_cast<RecordWriteMode>(MiscField::decode(instr->opcode()));
      Register object = i.InputRegister(0);
      Register value = i.InputRegister(2);
      Register scratch0 = i.TempRegister(0);
      Register scratch1 = i.TempRegister(1);
      OutOfLineRecordWrite* ool;

      AddressingMode addressing_mode =
          AddressingModeField::decode(instr->opcode());
      if (addressing_mode == kMode_MRI) {
        int32_t offset = i.InputInt32(1);
        ool = new (zone()) OutOfLineRecordWrite(this, object, offset, value,
                                                scratch0, scratch1, mode);
        __ StoreP(value, MemOperand(object, offset));
      } else {
        DCHECK_EQ(kMode_MRR, addressing_mode);
        Register offset(i.InputRegister(1));
        ool = new (zone()) OutOfLineRecordWrite(this, object, offset, value,
                                                scratch0, scratch1, mode);
        __ StoreP(value, MemOperand(object, offset));
      }
      __ CheckPageFlag(object, scratch0,
                       MemoryChunk::kPointersFromHereAreInterestingMask, ne,
                       ool->entry());
      __ bind(ool->exit());
      break;
    }
    case kArchStackSlot: {
      FrameOffset offset =
          frame_access_state()->GetFrameOffset(i.InputInt32(0));
      __ AddP(i.OutputRegister(), offset.from_stack_pointer() ? sp : fp,
              Operand(offset.offset()));
      break;
    }
    case kS390_Abs32:
      // TODO(john.yan): zero-ext
      __ lpr(i.OutputRegister(0), i.InputRegister(0));
      break;
    case kS390_Abs64:
      __ lpgr(i.OutputRegister(0), i.InputRegister(0));
      break;
    case kS390_And32:
      // zero-ext
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN32_OP(RRRInstr(nrk), RM32Instr(And), RIInstr(nilf));
      } else {
        ASSEMBLE_BIN32_OP(RRInstr(nr), RM32Instr(And), RIInstr(nilf));
      }
      break;
    case kS390_And64:
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN_OP(RRRInstr(ngrk), RM64Instr(ng), nullInstr);
      } else {
        ASSEMBLE_BIN_OP(RRInstr(ngr), RM64Instr(ng), nullInstr);
      }
      break;
    case kS390_Or32:
      // zero-ext
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN32_OP(RRRInstr(ork), RM32Instr(Or), RIInstr(oilf));
      } else {
        ASSEMBLE_BIN32_OP(RRInstr(or_z), RM32Instr(Or), RIInstr(oilf));
      }
      break;
    case kS390_Or64:
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN_OP(RRRInstr(ogrk), RM64Instr(og), nullInstr);
      } else {
        ASSEMBLE_BIN_OP(RRInstr(ogr), RM64Instr(og), nullInstr);
      }
      break;
    case kS390_Xor32:
      // zero-ext
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN32_OP(RRRInstr(xrk), RM32Instr(Xor), RIInstr(xilf));
      } else {
        ASSEMBLE_BIN32_OP(RRInstr(xr), RM32Instr(Xor), RIInstr(xilf));
      }
      break;
    case kS390_Xor64:
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN_OP(RRRInstr(xgrk), RM64Instr(xg), nullInstr);
      } else {
        ASSEMBLE_BIN_OP(RRInstr(xgr), RM64Instr(xg), nullInstr);
      }
      break;
    case kS390_ShiftLeft32:
      // zero-ext
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN32_OP(RRRInstr(ShiftLeft), nullInstr, RRIInstr(ShiftLeft));
      } else {
        ASSEMBLE_BIN32_OP(RRInstr(sll), nullInstr, RIInstr(sll));
      }
      break;
    case kS390_ShiftLeft64:
      ASSEMBLE_BIN_OP(RRRInstr(sllg), nullInstr, RRIInstr(sllg));
      break;
    case kS390_ShiftRight32:
      // zero-ext
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN32_OP(RRRInstr(srlk), nullInstr, RRIInstr(srlk));
      } else {
        ASSEMBLE_BIN32_OP(RRInstr(srl), nullInstr, RIInstr(srl));
      }
      break;
    case kS390_ShiftRight64:
      ASSEMBLE_BIN_OP(RRRInstr(srlg), nullInstr, RRIInstr(srlg));
      break;
    case kS390_ShiftRightArith32:
      // zero-ext
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN32_OP(RRRInstr(srak), nullInstr, RRIInstr(srak));
      } else {
        ASSEMBLE_BIN32_OP(RRInstr(sra), nullInstr, RIInstr(sra));
      }
      break;
    case kS390_ShiftRightArith64:
      ASSEMBLE_BIN_OP(RRRInstr(srag), nullInstr, RRIInstr(srag));
      break;
#if !V8_TARGET_ARCH_S390X
    case kS390_AddPair:
      // i.InputRegister(0) ... left low word.
      // i.InputRegister(1) ... left high word.
      // i.InputRegister(2) ... right low word.
      // i.InputRegister(3) ... right high word.
      __ AddLogical32(i.OutputRegister(0), i.InputRegister(0),
                      i.InputRegister(2));
      __ AddLogicalWithCarry32(i.OutputRegister(1), i.InputRegister(1),
                               i.InputRegister(3));
      break;
    case kS390_SubPair:
      // i.InputRegister(0) ... left low word.
      // i.InputRegister(1) ... left high word.
      // i.InputRegister(2) ... right low word.
      // i.InputRegister(3) ... right high word.
      __ SubLogical32(i.OutputRegister(0), i.InputRegister(0),
                      i.InputRegister(2));
      __ SubLogicalWithBorrow32(i.OutputRegister(1), i.InputRegister(1),
                                i.InputRegister(3));
      break;
    case kS390_MulPair:
      // i.InputRegister(0) ... left low word.
      // i.InputRegister(1) ... left high word.
      // i.InputRegister(2) ... right low word.
      // i.InputRegister(3) ... right high word.
      __ sllg(r0, i.InputRegister(1), Operand(32));
      __ sllg(r1, i.InputRegister(3), Operand(32));
      __ lr(r0, i.InputRegister(0));
      __ lr(r1, i.InputRegister(2));
      __ msgr(r1, r0);
      __ lr(i.OutputRegister(0), r1);
      __ srag(i.OutputRegister(1), r1, Operand(32));
      break;
    case kS390_ShiftLeftPair: {
      Register second_output =
          instr->OutputCount() >= 2 ? i.OutputRegister(1) : i.TempRegister(0);
      if (instr->InputAt(2)->IsImmediate()) {
        __ ShiftLeftPair(i.OutputRegister(0), second_output, i.InputRegister(0),
                         i.InputRegister(1), i.InputInt32(2));
      } else {
        __ ShiftLeftPair(i.OutputRegister(0), second_output, i.InputRegister(0),
                         i.InputRegister(1), kScratchReg, i.InputRegister(2));
      }
      break;
    }
    case kS390_ShiftRightPair: {
      Register second_output =
          instr->OutputCount() >= 2 ? i.OutputRegister(1) : i.TempRegister(0);
      if (instr->InputAt(2)->IsImmediate()) {
        __ ShiftRightPair(i.OutputRegister(0), second_output,
                          i.InputRegister(0), i.InputRegister(1),
                          i.InputInt32(2));
      } else {
        __ ShiftRightPair(i.OutputRegister(0), second_output,
                          i.InputRegister(0), i.InputRegister(1), kScratchReg,
                          i.InputRegister(2));
      }
      break;
    }
    case kS390_ShiftRightArithPair: {
      Register second_output =
          instr->OutputCount() >= 2 ? i.OutputRegister(1) : i.TempRegister(0);
      if (instr->InputAt(2)->IsImmediate()) {
        __ ShiftRightArithPair(i.OutputRegister(0), second_output,
                               i.InputRegister(0), i.InputRegister(1),
                               i.InputInt32(2));
      } else {
        __ ShiftRightArithPair(i.OutputRegister(0), second_output,
                               i.InputRegister(0), i.InputRegister(1),
                               kScratchReg, i.InputRegister(2));
      }
      break;
    }
#endif
    case kS390_RotRight32: {
      // zero-ext
      if (HasRegisterInput(instr, 1)) {
        __ LoadComplementRR(kScratchReg, i.InputRegister(1));
        __ rll(i.OutputRegister(), i.InputRegister(0), kScratchReg);
      } else {
        __ rll(i.OutputRegister(), i.InputRegister(0),
               Operand(32 - i.InputInt32(1)));
      }
      CHECK_AND_ZERO_EXT_OUTPUT(2);
      break;
    }
    case kS390_RotRight64:
      if (HasRegisterInput(instr, 1)) {
        __ lcgr(kScratchReg, i.InputRegister(1));
        __ rllg(i.OutputRegister(), i.InputRegister(0), kScratchReg);
      } else {
        DCHECK(HasImmediateInput(instr, 1));
        __ rllg(i.OutputRegister(), i.InputRegister(0),
                Operand(64 - i.InputInt32(1)));
      }
      break;
    // TODO(john.yan): clean up kS390_RotLeftAnd...
    case kS390_RotLeftAndClear64:
      if (CpuFeatures::IsSupported(GENERAL_INSTR_EXT)) {
        int shiftAmount = i.InputInt32(1);
        int endBit = 63 - shiftAmount;
        int startBit = 63 - i.InputInt32(2);
        __ risbg(i.OutputRegister(), i.InputRegister(0), Operand(startBit),
                 Operand(endBit), Operand(shiftAmount), true);
      } else {
        int shiftAmount = i.InputInt32(1);
        int clearBit = 63 - i.InputInt32(2);
        __ rllg(i.OutputRegister(), i.InputRegister(0), Operand(shiftAmount));
        __ sllg(i.OutputRegister(), i.OutputRegister(), Operand(clearBit));
        __ srlg(i.OutputRegister(), i.OutputRegister(),
                Operand(clearBit + shiftAmount));
        __ sllg(i.OutputRegister(), i.OutputRegister(), Operand(shiftAmount));
      }
      break;
    case kS390_RotLeftAndClearLeft64:
      if (CpuFeatures::IsSupported(GENERAL_INSTR_EXT)) {
        int shiftAmount = i.InputInt32(1);
        int endBit = 63;
        int startBit = 63 - i.InputInt32(2);
        __ risbg(i.OutputRegister(), i.InputRegister(0), Operand(startBit),
                 Operand(endBit), Operand(shiftAmount), true);
      } else {
        int shiftAmount = i.InputInt32(1);
        int clearBit = 63 - i.InputInt32(2);
        __ rllg(i.OutputRegister(), i.InputRegister(0), Operand(shiftAmount));
        __ sllg(i.OutputRegister(), i.OutputRegister(), Operand(clearBit));
        __ srlg(i.OutputRegister(), i.OutputRegister(), Operand(clearBit));
      }
      break;
    case kS390_RotLeftAndClearRight64:
      if (CpuFeatures::IsSupported(GENERAL_INSTR_EXT)) {
        int shiftAmount = i.InputInt32(1);
        int endBit = 63 - i.InputInt32(2);
        int startBit = 0;
        __ risbg(i.OutputRegister(), i.InputRegister(0), Operand(startBit),
                 Operand(endBit), Operand(shiftAmount), true);
      } else {
        int shiftAmount = i.InputInt32(1);
        int clearBit = i.InputInt32(2);
        __ rllg(i.OutputRegister(), i.InputRegister(0), Operand(shiftAmount));
        __ srlg(i.OutputRegister(), i.OutputRegister(), Operand(clearBit));
        __ sllg(i.OutputRegister(), i.OutputRegister(), Operand(clearBit));
      }
      break;
    case kS390_Add32: {
      // zero-ext
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN32_OP(RRRInstr(ark), RM32Instr(Add32), RRIInstr(Add32));
      } else {
        ASSEMBLE_BIN32_OP(RRInstr(ar), RM32Instr(Add32), RIInstr(Add32));
      }
      break;
    }
    case kS390_Add64:
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN_OP(RRRInstr(agrk), RM64Instr(ag), RRIInstr(AddP));
      } else {
        ASSEMBLE_BIN_OP(RRInstr(agr), RM64Instr(ag), RIInstr(agfi));
      }
      break;
    case kS390_AddFloat:
      ASSEMBLE_BIN_OP(DDInstr(aebr), DMTInstr(AddFloat32), nullInstr);
      break;
    case kS390_AddDouble:
      ASSEMBLE_BIN_OP(DDInstr(adbr), DMTInstr(AddFloat64), nullInstr);
      break;
    case kS390_Sub32:
      // zero-ext
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN32_OP(RRRInstr(srk), RM32Instr(Sub32), RRIInstr(Sub32));
      } else {
        ASSEMBLE_BIN32_OP(RRInstr(sr), RM32Instr(Sub32), RIInstr(Sub32));
      }
      break;
    case kS390_Sub64:
      if (CpuFeatures::IsSupported(DISTINCT_OPS)) {
        ASSEMBLE_BIN_OP(RRRInstr(sgrk), RM64Instr(sg), RRIInstr(SubP));
      } else {
        ASSEMBLE_BIN_OP(RRInstr(sgr), RM64Instr(sg), RIInstr(SubP));
      }
      break;
    case kS390_SubFloat:
      ASSEMBLE_BIN_OP(DDInstr(sebr), DMTInstr(SubFloat32), nullInstr);
      break;
    case kS390_SubDouble:
      ASSEMBLE_BIN_OP(DDInstr(sdbr), DMTInstr(SubFloat64), nullInstr);
      break;
    case kS390_Mul32:
      // zero-ext
      if (CpuFeatures::IsSupported(MISC_INSTR_EXT2)) {
        ASSEMBLE_BIN32_OP(RRRInstr(msrkc), RM32Instr(msc), RIInstr(Mul32));
      } else {
        ASSEMBLE_BIN32_OP(RRInstr(Mul32), RM32Instr(Mul32), RIInstr(Mul32));
      }
      break;
    case kS390_Mul32WithOverflow:
      // zero-ext
      ASSEMBLE_BIN32_OP(RRRInstr(Mul32WithOverflowIfCCUnequal),
                        RRM32Instr(Mul32WithOverflowIfCCUnequal),
                        RRIInstr(Mul32WithOverflowIfCCUnequal));
      break;
    case kS390_Mul64:
      ASSEMBLE_BIN_OP(RRInstr(Mul64), RM64Instr(Mul64), RIInstr(Mul64));
      break;
    case kS390_MulHigh32:
      // zero-ext
      ASSEMBLE_BIN_OP(RRRInstr(MulHigh32), RRM32Instr(MulHigh32),
                      RRIInstr(MulHigh32));
      break;
    case kS390_MulHighU32:
      // zero-ext
      ASSEMBLE_BIN_OP(RRRInstr(MulHighU32), RRM32Instr(MulHighU32),
                      RRIInstr(MulHighU32));
      break;
    case kS390_MulFloat:
      ASSEMBLE_BIN_OP(DDInstr(meebr), DMTInstr(MulFloat32), nullInstr);
      break;
    case kS390_MulDouble:
      ASSEMBLE_BIN_OP(DDInstr(mdbr), DMTInstr(MulFloat64), nullInstr);
      break;
    case kS390_Div64:
      ASSEMBLE_BIN_OP(RRRInstr(Div64), RRM64Instr(Div64), nullInstr);
      break;
    case kS390_Div32: {
      // zero-ext
      ASSEMBLE_BIN_OP(RRRInstr(Div32), RRM32Instr(Div32), nullInstr);
      break;
    }
    case kS390_DivU64:
      ASSEMBLE_BIN_OP(RRRInstr(DivU64), RRM64Instr(DivU64), nullInstr);
      break;
    case kS390_DivU32: {
      // zero-ext
      ASSEMBLE_BIN_OP(RRRInstr(DivU32), RRM32Instr(DivU32), nullInstr);
      break;
    }
    case kS390_DivFloat:
      ASSEMBLE_BIN_OP(DDInstr(debr), DMTInstr(DivFloat32), nullInstr);
      break;
    case kS390_DivDouble:
      ASSEMBLE_BIN_OP(DDInstr(ddbr), DMTInstr(DivFloat64), nullInstr);
      break;
    case kS390_Mod32:
      // zero-ext
      ASSEMBLE_BIN_OP(RRRInstr(Mod32), RRM32Instr(Mod32), nullInstr);
      break;
    case kS390_ModU32:
      // zero-ext
      ASSEMBLE_BIN_OP(RRRInstr(ModU32), RRM32Instr(ModU32), nullInstr);
      break;
    case kS390_Mod64:
      ASSEMBLE_BIN_OP(RRRInstr(Mod64), RRM64Instr(Mod64), nullInstr);
      break;
    case kS390_ModU64:
      ASSEMBLE_BIN_OP(RRRInstr(ModU64), RRM64Instr(ModU64), nullInstr);
      break;
    case kS390_AbsFloat:
      __ lpebr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_SqrtFloat:
      ASSEMBLE_UNARY_OP(D_DInstr(sqebr), nullInstr, nullInstr);
      break;
    case kS390_SqrtDouble:
      ASSEMBLE_UNARY_OP(D_DInstr(sqdbr), nullInstr, nullInstr);
      break;
    case kS390_FloorFloat:
      __ fiebra(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                v8::internal::Assembler::FIDBRA_ROUND_TOWARD_NEG_INF);
      break;
    case kS390_CeilFloat:
      __ fiebra(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                v8::internal::Assembler::FIDBRA_ROUND_TOWARD_POS_INF);
      break;
    case kS390_TruncateFloat:
      __ fiebra(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                v8::internal::Assembler::FIDBRA_ROUND_TOWARD_0);
      break;
    //  Double operations
    case kS390_ModDouble:
      ASSEMBLE_FLOAT_MODULO();
      break;
    case kIeee754Float64Acos:
      ASSEMBLE_IEEE754_UNOP(acos);
      break;
    case kIeee754Float64Acosh:
      ASSEMBLE_IEEE754_UNOP(acosh);
      break;
    case kIeee754Float64Asin:
      ASSEMBLE_IEEE754_UNOP(asin);
      break;
    case kIeee754Float64Asinh:
      ASSEMBLE_IEEE754_UNOP(asinh);
      break;
    case kIeee754Float64Atanh:
      ASSEMBLE_IEEE754_UNOP(atanh);
      break;
    case kIeee754Float64Atan:
      ASSEMBLE_IEEE754_UNOP(atan);
      break;
    case kIeee754Float64Atan2:
      ASSEMBLE_IEEE754_BINOP(atan2);
      break;
    case kIeee754Float64Tan:
      ASSEMBLE_IEEE754_UNOP(tan);
      break;
    case kIeee754Float64Tanh:
      ASSEMBLE_IEEE754_UNOP(tanh);
      break;
    case kIeee754Float64Cbrt:
      ASSEMBLE_IEEE754_UNOP(cbrt);
      break;
    case kIeee754Float64Sin:
      ASSEMBLE_IEEE754_UNOP(sin);
      break;
    case kIeee754Float64Sinh:
      ASSEMBLE_IEEE754_UNOP(sinh);
      break;
    case kIeee754Float64Cos:
      ASSEMBLE_IEEE754_UNOP(cos);
      break;
    case kIeee754Float64Cosh:
      ASSEMBLE_IEEE754_UNOP(cosh);
      break;
    case kIeee754Float64Exp:
      ASSEMBLE_IEEE754_UNOP(exp);
      break;
    case kIeee754Float64Expm1:
      ASSEMBLE_IEEE754_UNOP(expm1);
      break;
    case kIeee754Float64Log:
      ASSEMBLE_IEEE754_UNOP(log);
      break;
    case kIeee754Float64Log1p:
      ASSEMBLE_IEEE754_UNOP(log1p);
      break;
    case kIeee754Float64Log2:
      ASSEMBLE_IEEE754_UNOP(log2);
      break;
    case kIeee754Float64Log10:
      ASSEMBLE_IEEE754_UNOP(log10);
      break;
    case kIeee754Float64Pow: {
      MathPowStub stub(isolate(), MathPowStub::DOUBLE);
      __ CallStub(&stub);
      __ Move(d1, d3);
      break;
    }
    case kS390_Neg32:
      __ lcr(i.OutputRegister(), i.InputRegister(0));
      CHECK_AND_ZERO_EXT_OUTPUT(1);
      break;
    case kS390_Neg64:
      __ lcgr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_MaxFloat:
      ASSEMBLE_FLOAT_MAX();
      break;
    case kS390_MaxDouble:
      ASSEMBLE_DOUBLE_MAX();
      break;
    case kS390_MinFloat:
      ASSEMBLE_FLOAT_MIN();
      break;
    case kS390_MinDouble:
      ASSEMBLE_DOUBLE_MIN();
      break;
    case kS390_AbsDouble:
      __ lpdbr(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_FloorDouble:
      __ fidbra(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                v8::internal::Assembler::FIDBRA_ROUND_TOWARD_NEG_INF);
      break;
    case kS390_CeilDouble:
      __ fidbra(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                v8::internal::Assembler::FIDBRA_ROUND_TOWARD_POS_INF);
      break;
    case kS390_TruncateDouble:
      __ fidbra(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                v8::internal::Assembler::FIDBRA_ROUND_TOWARD_0);
      break;
    case kS390_RoundDouble:
      __ fidbra(i.OutputDoubleRegister(), i.InputDoubleRegister(0),
                v8::internal::Assembler::FIDBRA_ROUND_TO_NEAREST_AWAY_FROM_0);
      break;
    case kS390_NegFloat:
      ASSEMBLE_UNARY_OP(D_DInstr(lcebr), nullInstr, nullInstr);
      break;
    case kS390_NegDouble:
      ASSEMBLE_UNARY_OP(D_DInstr(lcdbr), nullInstr, nullInstr);
      break;
    case kS390_Cntlz32: {
      __ llgfr(i.OutputRegister(), i.InputRegister(0));
      __ flogr(r0, i.OutputRegister());
      __ Add32(i.OutputRegister(), r0, Operand(-32));
      // No need to zero-ext b/c llgfr is done already
      break;
    }
#if V8_TARGET_ARCH_S390X
    case kS390_Cntlz64: {
      __ flogr(r0, i.InputRegister(0));
      __ LoadRR(i.OutputRegister(), r0);
      break;
    }
#endif
    case kS390_Popcnt32:
      __ Popcnt32(i.OutputRegister(), i.InputRegister(0));
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Popcnt64:
      __ Popcnt64(i.OutputRegister(), i.InputRegister(0));
      break;
#endif
    case kS390_Cmp32:
      ASSEMBLE_COMPARE32(Cmp32, CmpLogical32);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_Cmp64:
      ASSEMBLE_COMPARE(CmpP, CmpLogicalP);
      break;
#endif
    case kS390_CmpFloat:
      ASSEMBLE_FLOAT_COMPARE(cebr, ceb, ley);
      // __ cebr(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      break;
    case kS390_CmpDouble:
      ASSEMBLE_FLOAT_COMPARE(cdbr, cdb, ldy);
      // __ cdbr(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      break;
    case kS390_Tst32:
      if (HasRegisterInput(instr, 1)) {
        __ And(r0, i.InputRegister(0), i.InputRegister(1));
      } else {
        // detect tmlh/tmhl/tmhh case
        Operand opnd = i.InputImmediate(1);
        if (is_uint16(opnd.immediate())) {
          __ tmll(i.InputRegister(0), opnd);
        } else {
          __ lr(r0, i.InputRegister(0));
          __ nilf(r0, opnd);
        }
      }
      break;
    case kS390_Tst64:
      if (HasRegisterInput(instr, 1)) {
        __ AndP(r0, i.InputRegister(0), i.InputRegister(1));
      } else {
        Operand opnd = i.InputImmediate(1);
        if (is_uint16(opnd.immediate())) {
          __ tmll(i.InputRegister(0), opnd);
        } else {
          __ AndP(r0, i.InputRegister(0), opnd);
        }
      }
      break;
    case kS390_Float64SilenceNaN: {
      DoubleRegister value = i.InputDoubleRegister(0);
      DoubleRegister result = i.OutputDoubleRegister();
      __ CanonicalizeNaN(result, value);
      break;
    }
    case kS390_Push:
      if (instr->InputAt(0)->IsFPRegister()) {
        __ lay(sp, MemOperand(sp, -kDoubleSize));
        __ StoreDouble(i.InputDoubleRegister(0), MemOperand(sp));
        frame_access_state()->IncreaseSPDelta(kDoubleSize / kPointerSize);
      } else {
        __ Push(i.InputRegister(0));
        frame_access_state()->IncreaseSPDelta(1);
      }
      break;
    case kS390_PushFrame: {
      int num_slots = i.InputInt32(1);
      __ lay(sp, MemOperand(sp, -num_slots * kPointerSize));
      if (instr->InputAt(0)->IsFPRegister()) {
        LocationOperand* op = LocationOperand::cast(instr->InputAt(0));
        if (op->representation() == MachineRepresentation::kFloat64) {
          __ StoreDouble(i.InputDoubleRegister(0), MemOperand(sp));
        } else {
          DCHECK(op->representation() == MachineRepresentation::kFloat32);
          __ StoreFloat32(i.InputDoubleRegister(0), MemOperand(sp));
        }
      } else {
        __ StoreP(i.InputRegister(0),
                  MemOperand(sp));
      }
      break;
    }
    case kS390_StoreToStackSlot: {
      int slot = i.InputInt32(1);
      if (instr->InputAt(0)->IsFPRegister()) {
        LocationOperand* op = LocationOperand::cast(instr->InputAt(0));
        if (op->representation() == MachineRepresentation::kFloat64) {
          __ StoreDouble(i.InputDoubleRegister(0),
                         MemOperand(sp, slot * kPointerSize));
        } else {
          DCHECK(op->representation() == MachineRepresentation::kFloat32);
          __ StoreFloat32(i.InputDoubleRegister(0),
                          MemOperand(sp, slot * kPointerSize));
        }
      } else {
        __ StoreP(i.InputRegister(0), MemOperand(sp, slot * kPointerSize));
      }
      break;
    }
    case kS390_ExtendSignWord8:
      __ lbr(i.OutputRegister(), i.InputRegister(0));
      CHECK_AND_ZERO_EXT_OUTPUT(1);
      break;
    case kS390_ExtendSignWord16:
      __ lhr(i.OutputRegister(), i.InputRegister(0));
      CHECK_AND_ZERO_EXT_OUTPUT(1);
      break;
    case kS390_ExtendSignWord32:
      __ lgfr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_Uint32ToUint64:
      // Zero extend
      __ llgfr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_Int64ToInt32:
      // sign extend
      __ lgfr(i.OutputRegister(), i.InputRegister(0));
      break;
    // Convert Fixed to Floating Point
    case kS390_Int64ToFloat32:
      __ ConvertInt64ToFloat(i.OutputDoubleRegister(), i.InputRegister(0));
      break;
    case kS390_Int64ToDouble:
      __ ConvertInt64ToDouble(i.OutputDoubleRegister(), i.InputRegister(0));
      break;
    case kS390_Uint64ToFloat32:
      __ ConvertUnsignedInt64ToFloat(i.OutputDoubleRegister(),
                                     i.InputRegister(0));
      break;
    case kS390_Uint64ToDouble:
      __ ConvertUnsignedInt64ToDouble(i.OutputDoubleRegister(),
                                      i.InputRegister(0));
      break;
    case kS390_Int32ToFloat32:
      __ ConvertIntToFloat(i.OutputDoubleRegister(), i.InputRegister(0));
      break;
    case kS390_Int32ToDouble:
      __ ConvertIntToDouble(i.OutputDoubleRegister(), i.InputRegister(0));
      break;
    case kS390_Uint32ToFloat32:
      __ ConvertUnsignedIntToFloat(i.OutputDoubleRegister(),
                                   i.InputRegister(0));
      break;
    case kS390_Uint32ToDouble:
      __ ConvertUnsignedIntToDouble(i.OutputDoubleRegister(),
                                    i.InputRegister(0));
      break;
    case kS390_DoubleToInt32: {
      Label done;
      __ ConvertDoubleToInt32(i.OutputRegister(0), i.InputDoubleRegister(0),
                              kRoundToNearest);
      __ b(Condition(0xe), &done, Label::kNear);  // normal case
      __ lghi(i.OutputRegister(0), Operand::Zero());
      __ bind(&done);
      break;
    }
    case kS390_DoubleToUint32: {
      Label done;
      __ ConvertDoubleToUnsignedInt32(i.OutputRegister(0),
                                      i.InputDoubleRegister(0));
      __ b(Condition(0xe), &done, Label::kNear);  // normal case
      __ lghi(i.OutputRegister(0), Operand::Zero());
      __ bind(&done);
      break;
    }
    case kS390_DoubleToInt64: {
      Label done;
      if (i.OutputCount() > 1) {
        __ lghi(i.OutputRegister(1), Operand(1));
      }
      __ ConvertDoubleToInt64(i.OutputRegister(0), i.InputDoubleRegister(0));
      __ b(Condition(0xe), &done, Label::kNear);  // normal case
      if (i.OutputCount() > 1) {
        __ lghi(i.OutputRegister(1), Operand::Zero());
      } else {
        __ lghi(i.OutputRegister(0), Operand::Zero());
      }
      __ bind(&done);
      break;
    }
    case kS390_DoubleToUint64: {
      Label done;
      if (i.OutputCount() > 1) {
        __ lghi(i.OutputRegister(1), Operand(1));
      }
      __ ConvertDoubleToUnsignedInt64(i.OutputRegister(0),
                                      i.InputDoubleRegister(0));
      __ b(Condition(0xe), &done, Label::kNear);  // normal case
      if (i.OutputCount() > 1) {
        __ lghi(i.OutputRegister(1), Operand::Zero());
      } else {
        __ lghi(i.OutputRegister(0), Operand::Zero());
      }
      __ bind(&done);
      break;
    }
    case kS390_Float32ToInt32: {
      Label done;
      __ ConvertFloat32ToInt32(i.OutputRegister(0), i.InputDoubleRegister(0),
                               kRoundToZero);
      __ b(Condition(0xe), &done, Label::kNear);  // normal case
      __ lghi(i.OutputRegister(0), Operand::Zero());
      __ bind(&done);
      break;
    }
    case kS390_Float32ToUint32: {
      Label done;
      __ ConvertFloat32ToUnsignedInt32(i.OutputRegister(0),
                                       i.InputDoubleRegister(0));
      __ b(Condition(0xe), &done, Label::kNear);  // normal case
      __ lghi(i.OutputRegister(0), Operand::Zero());
      __ bind(&done);
      break;
    }
    case kS390_Float32ToUint64: {
      Label done;
      if (i.OutputCount() > 1) {
        __ lghi(i.OutputRegister(1), Operand(1));
      }
      __ ConvertFloat32ToUnsignedInt64(i.OutputRegister(0),
                                       i.InputDoubleRegister(0));
      __ b(Condition(0xe), &done, Label::kNear);  // normal case
      if (i.OutputCount() > 1) {
        __ lghi(i.OutputRegister(1), Operand::Zero());
      } else {
        __ lghi(i.OutputRegister(0), Operand::Zero());
      }
      __ bind(&done);
      break;
    }
    case kS390_Float32ToInt64: {
      Label done;
      if (i.OutputCount() > 1) {
        __ lghi(i.OutputRegister(1), Operand(1));
      }
      __ ConvertFloat32ToInt64(i.OutputRegister(0), i.InputDoubleRegister(0));
      __ b(Condition(0xe), &done, Label::kNear);  // normal case
      if (i.OutputCount() > 1) {
        __ lghi(i.OutputRegister(1), Operand::Zero());
      } else {
        __ lghi(i.OutputRegister(0), Operand::Zero());
      }
      __ bind(&done);
      break;
    }
    case kS390_DoubleToFloat32:
      ASSEMBLE_UNARY_OP(D_DInstr(ledbr), nullInstr, nullInstr);
      break;
    case kS390_Float32ToDouble:
      ASSEMBLE_UNARY_OP(D_DInstr(ldebr), D_MTInstr(LoadFloat32ToDouble),
                        nullInstr);
      break;
    case kS390_DoubleExtractLowWord32:
      __ lgdr(i.OutputRegister(), i.InputDoubleRegister(0));
      __ llgfr(i.OutputRegister(), i.OutputRegister());
      break;
    case kS390_DoubleExtractHighWord32:
      __ lgdr(i.OutputRegister(), i.InputDoubleRegister(0));
      __ srlg(i.OutputRegister(), i.OutputRegister(), Operand(32));
      break;
    case kS390_DoubleInsertLowWord32:
      __ lgdr(kScratchReg, i.InputDoubleRegister(0));
      __ lr(kScratchReg, i.InputRegister(1));
      __ ldgr(i.OutputDoubleRegister(), kScratchReg);
      break;
    case kS390_DoubleInsertHighWord32:
      __ sllg(kScratchReg, i.InputRegister(1), Operand(32));
      __ lgdr(r0, i.InputDoubleRegister(0));
      __ lr(kScratchReg, r0);
      __ ldgr(i.OutputDoubleRegister(), kScratchReg);
      break;
    case kS390_DoubleConstruct:
      __ sllg(kScratchReg, i.InputRegister(0), Operand(32));
      __ lr(kScratchReg, i.InputRegister(1));

      // Bitwise convert from GPR to FPR
      __ ldgr(i.OutputDoubleRegister(), kScratchReg);
      break;
    case kS390_LoadWordS8:
      ASSEMBLE_LOAD_INTEGER(LoadB);
      break;
    case kS390_BitcastFloat32ToInt32:
      ASSEMBLE_UNARY_OP(R_DInstr(MovFloatToInt), R_MInstr(LoadlW), nullInstr);
      break;
    case kS390_BitcastInt32ToFloat32:
      __ MovIntToFloat(i.OutputDoubleRegister(), i.InputRegister(0));
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_BitcastDoubleToInt64:
      __ MovDoubleToInt64(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kS390_BitcastInt64ToDouble:
      __ MovInt64ToDouble(i.OutputDoubleRegister(), i.InputRegister(0));
      break;
#endif
    case kS390_LoadWordU8:
      ASSEMBLE_LOAD_INTEGER(LoadlB);
      break;
    case kS390_LoadWordU16:
      ASSEMBLE_LOAD_INTEGER(LoadLogicalHalfWordP);
      break;
    case kS390_LoadWordS16:
      ASSEMBLE_LOAD_INTEGER(LoadHalfWordP);
      break;
    case kS390_LoadWordU32:
      ASSEMBLE_LOAD_INTEGER(LoadlW);
      break;
    case kS390_LoadWordS32:
      ASSEMBLE_LOAD_INTEGER(LoadW);
      break;
    case kS390_LoadReverse16:
      ASSEMBLE_LOAD_INTEGER(lrvh);
      break;
    case kS390_LoadReverse32:
      ASSEMBLE_LOAD_INTEGER(lrv);
      break;
    case kS390_LoadReverse64:
      ASSEMBLE_LOAD_INTEGER(lrvg);
      break;
    case kS390_LoadReverse16RR:
      __ lrvr(i.OutputRegister(), i.InputRegister(0));
      __ rll(i.OutputRegister(), i.OutputRegister(), Operand(16));
      break;
    case kS390_LoadReverse32RR:
      __ lrvr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_LoadReverse64RR:
      __ lrvgr(i.OutputRegister(), i.InputRegister(0));
      break;
    case kS390_LoadWord64:
      ASSEMBLE_LOAD_INTEGER(lg);
      break;
    case kS390_LoadAndTestWord32: {
      ASSEMBLE_LOADANDTEST32(ltr, lt_z);
      break;
    }
    case kS390_LoadAndTestWord64: {
      ASSEMBLE_LOADANDTEST64(ltgr, ltg);
      break;
    }
    case kS390_LoadFloat32:
      ASSEMBLE_LOAD_FLOAT(LoadFloat32);
      break;
    case kS390_LoadDouble:
      ASSEMBLE_LOAD_FLOAT(LoadDouble);
      break;
    case kS390_StoreWord8:
      ASSEMBLE_STORE_INTEGER(StoreByte);
      break;
    case kS390_StoreWord16:
      ASSEMBLE_STORE_INTEGER(StoreHalfWord);
      break;
    case kS390_StoreWord32:
      ASSEMBLE_STORE_INTEGER(StoreW);
      break;
#if V8_TARGET_ARCH_S390X
    case kS390_StoreWord64:
      ASSEMBLE_STORE_INTEGER(StoreP);
      break;
#endif
    case kS390_StoreReverse16:
      ASSEMBLE_STORE_INTEGER(strvh);
      break;
    case kS390_StoreReverse32:
      ASSEMBLE_STORE_INTEGER(strv);
      break;
    case kS390_StoreReverse64:
      ASSEMBLE_STORE_INTEGER(strvg);
      break;
    case kS390_StoreFloat32:
      ASSEMBLE_STORE_FLOAT32();
      break;
    case kS390_StoreDouble:
      ASSEMBLE_STORE_DOUBLE();
      break;
    case kS390_Lay:
      __ lay(i.OutputRegister(), i.MemoryOperand());
      break;
    case kCheckedLoadInt8:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadB);
      break;
    case kCheckedLoadUint8:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadlB);
      break;
    case kCheckedLoadInt16:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadHalfWordP);
      break;
    case kCheckedLoadUint16:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadLogicalHalfWordP);
      break;
    case kCheckedLoadWord32:
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadlW);
      break;
    case kCheckedLoadWord64:
#if V8_TARGET_ARCH_S390X
      ASSEMBLE_CHECKED_LOAD_INTEGER(LoadP);
#else
      UNREACHABLE();
#endif
      break;
    case kCheckedLoadFloat32:
      ASSEMBLE_CHECKED_LOAD_FLOAT(LoadFloat32, 32);
      break;
    case kCheckedLoadFloat64:
      ASSEMBLE_CHECKED_LOAD_FLOAT(LoadDouble, 64);
      break;
    case kCheckedStoreWord8:
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreByte);
      break;
    case kCheckedStoreWord16:
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreHalfWord);
      break;
    case kCheckedStoreWord32:
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreW);
      break;
    case kCheckedStoreWord64:
#if V8_TARGET_ARCH_S390X
      ASSEMBLE_CHECKED_STORE_INTEGER(StoreP);
#else
      UNREACHABLE();
#endif
      break;
    case kCheckedStoreFloat32:
      ASSEMBLE_CHECKED_STORE_FLOAT32();
      break;
    case kCheckedStoreFloat64:
      ASSEMBLE_CHECKED_STORE_DOUBLE();
      break;
    case kAtomicLoadInt8:
      __ LoadB(i.OutputRegister(), i.MemoryOperand());
      break;
    case kAtomicLoadUint8:
      __ LoadlB(i.OutputRegister(), i.MemoryOperand());
      break;
    case kAtomicLoadInt16:
      __ LoadHalfWordP(i.OutputRegister(), i.MemoryOperand());
      break;
    case kAtomicLoadUint16:
      __ LoadLogicalHalfWordP(i.OutputRegister(), i.MemoryOperand());
      break;
    case kAtomicLoadWord32:
      __ LoadlW(i.OutputRegister(), i.MemoryOperand());
      break;
    case kAtomicStoreWord8:
      __ StoreByte(i.InputRegister(0), i.MemoryOperand(NULL, 1));
      break;
    case kAtomicStoreWord16:
      __ StoreHalfWord(i.InputRegister(0), i.MemoryOperand(NULL, 1));
      break;
    case kAtomicStoreWord32:
      __ StoreW(i.InputRegister(0), i.MemoryOperand(NULL, 1));
      break;
//         0x aa bb cc dd
// index =    3..2..1..0
#define ATOMIC_EXCHANGE(start, end, shift_amount, offset)                    \
  {                                                                          \
    Label do_cs;                                                             \
    __ LoadlW(output, MemOperand(r1));                                       \
    __ bind(&do_cs);                                                         \
    __ llgfr(r0, output);                                                    \
    __ risbg(r0, value, Operand(start), Operand(end), Operand(shift_amount), \
             false);                                                         \
    __ csy(output, r0, MemOperand(r1, offset));                              \
    __ bne(&do_cs, Label::kNear);                                            \
    __ srl(output, Operand(shift_amount));                                   \
  }
#ifdef V8_TARGET_BIG_ENDIAN
#define ATOMIC_EXCHANGE_BYTE(i)                                  \
  {                                                              \
    constexpr int idx = (i);                                     \
    static_assert(idx <= 3 && idx >= 0, "idx is out of range!"); \
    constexpr int start = 32 + 8 * idx;                          \
    constexpr int end = start + 7;                               \
    constexpr int shift_amount = (3 - idx) * 8;                  \
    ATOMIC_EXCHANGE(start, end, shift_amount, -idx);             \
  }
#define ATOMIC_EXCHANGE_HALFWORD(i)                              \
  {                                                              \
    constexpr int idx = (i);                                     \
    static_assert(idx <= 1 && idx >= 0, "idx is out of range!"); \
    constexpr int start = 32 + 16 * idx;                         \
    constexpr int end = start + 15;                              \
    constexpr int shift_amount = (1 - idx) * 16;                 \
    ATOMIC_EXCHANGE(start, end, shift_amount, -idx * 2);         \
  }
#else
#define ATOMIC_EXCHANGE_BYTE(i)                                  \
  {                                                              \
    constexpr int idx = (i);                                     \
    static_assert(idx <= 3 && idx >= 0, "idx is out of range!"); \
    constexpr int start = 32 + 8 * (3 - idx);                    \
    constexpr int end = start + 7;                               \
    constexpr int shift_amount = idx * 8;                        \
    ATOMIC_EXCHANGE(start, end, shift_amount, -idx);             \
  }
#define ATOMIC_EXCHANGE_HALFWORD(i)                              \
  {                                                              \
    constexpr int idx = (i);                                     \
    static_assert(idx <= 1 && idx >= 0, "idx is out of range!"); \
    constexpr int start = 32 + 16 * (1 - idx);                   \
    constexpr int end = start + 15;                              \
    constexpr int shift_amount = idx * 16;                       \
    ATOMIC_EXCHANGE(start, end, shift_amount, -idx * 2);         \
  }
#endif
    case kAtomicExchangeInt8:
    case kAtomicExchangeUint8: {
      Register base = i.InputRegister(0);
      Register index = i.InputRegister(1);
      Register value = i.InputRegister(2);
      Register output = i.OutputRegister();
      Label three, two, one, done;
      __ la(r1, MemOperand(base, index));
      __ tmll(r1, Operand(3));
      __ b(Condition(1), &three);
      __ b(Condition(2), &two);
      __ b(Condition(4), &one);

      // end with 0b00
      ATOMIC_EXCHANGE_BYTE(0);
      __ b(&done);

      // ending with 0b01
      __ bind(&one);
      ATOMIC_EXCHANGE_BYTE(1);
      __ b(&done);

      // ending with 0b10
      __ bind(&two);
      ATOMIC_EXCHANGE_BYTE(2);
      __ b(&done);

      // ending with 0b11
      __ bind(&three);
      ATOMIC_EXCHANGE_BYTE(3);

      __ bind(&done);
      if (opcode == kAtomicExchangeInt8) {
        __ lbr(output, output);
      } else {
        __ llcr(output, output);
      }
      break;
    }
    case kAtomicExchangeInt16:
    case kAtomicExchangeUint16: {
      Register base = i.InputRegister(0);
      Register index = i.InputRegister(1);
      Register value = i.InputRegister(2);
      Register output = i.OutputRegister();
      Label two, unaligned, done;
      __ la(r1, MemOperand(base, index));
      __ tmll(r1, Operand(3));
      __ b(Condition(2), &two);

      // end with 0b00
      ATOMIC_EXCHANGE_HALFWORD(0);
      __ b(&done);

      // ending with 0b10
      __ bind(&two);
      ATOMIC_EXCHANGE_HALFWORD(1);

      __ bind(&done);
      if (opcode == kAtomicExchangeInt8) {
        __ lhr(output, output);
      } else {
        __ llhr(output, output);
      }
      break;
    }
    case kAtomicExchangeWord32: {
      Register base = i.InputRegister(0);
      Register index = i.InputRegister(1);
      Register value = i.InputRegister(2);
      Register output = i.OutputRegister();
      Label do_cs;
      __ lay(r1, MemOperand(base, index));
      __ LoadlW(output, MemOperand(r1));
      __ bind(&do_cs);
      __ cs(output, value, MemOperand(r1));
      __ bne(&do_cs, Label::kNear);
      break;
    }
    default:
      UNREACHABLE();
      break;
  }
  return kSuccess;
}  // NOLINT(readability/fn_size)

// Assembles branches after an instruction.
void CodeGenerator::AssembleArchBranch(Instruction* instr, BranchInfo* branch) {
  S390OperandConverter i(this, instr);
  Label* tlabel = branch->true_label;
  Label* flabel = branch->false_label;
  ArchOpcode op = instr->arch_opcode();
  FlagsCondition condition = branch->condition;

  Condition cond = FlagsConditionToCondition(condition, op);
  if (op == kS390_CmpDouble) {
    // check for unordered if necessary
    // Branching to flabel/tlabel according to what's expected by tests
    if (cond == le || cond == eq || cond == lt) {
      __ bunordered(flabel);
    } else if (cond == gt || cond == ne || cond == ge) {
      __ bunordered(tlabel);
    }
  }
  __ b(cond, tlabel);
  if (!branch->fallthru) __ b(flabel);  // no fallthru to flabel.
}

void CodeGenerator::AssembleArchJump(RpoNumber target) {
  if (!IsNextInAssemblyOrder(target)) __ b(GetLabel(target));
}

void CodeGenerator::AssembleArchTrap(Instruction* instr,
                                     FlagsCondition condition) {
  class OutOfLineTrap final : public OutOfLineCode {
   public:
    OutOfLineTrap(CodeGenerator* gen, bool frame_elided, Instruction* instr)
        : OutOfLineCode(gen),
          frame_elided_(frame_elided),
          instr_(instr),
          gen_(gen) {}

    void Generate() final {
      S390OperandConverter i(gen_, instr_);

      Builtins::Name trap_id =
          static_cast<Builtins::Name>(i.InputInt32(instr_->InputCount() - 1));
      bool old_has_frame = __ has_frame();
      if (frame_elided_) {
        __ set_has_frame(true);
        __ EnterFrame(StackFrame::WASM_COMPILED);
      }
      GenerateCallToTrap(trap_id);
      if (frame_elided_) {
        __ set_has_frame(old_has_frame);
      }
    }

   private:
    void GenerateCallToTrap(Builtins::Name trap_id) {
      if (trap_id == Builtins::builtin_count) {
        // We cannot test calls to the runtime in cctest/test-run-wasm.
        // Therefore we emit a call to C here instead of a call to the runtime.
        // We use the context register as the scratch register, because we do
        // not have a context here.
        __ PrepareCallCFunction(0, 0, cp);
        __ CallCFunction(
            ExternalReference::wasm_call_trap_callback_for_testing(isolate()),
            0);
        __ LeaveFrame(StackFrame::WASM_COMPILED);
        __ Ret();
      } else {
        gen_->AssembleSourcePosition(instr_);
        __ Call(handle(isolate()->builtins()->builtin(trap_id), isolate()),
                RelocInfo::CODE_TARGET);
        ReferenceMap* reference_map =
            new (gen_->zone()) ReferenceMap(gen_->zone());
        gen_->RecordSafepoint(reference_map, Safepoint::kSimple, 0,
                              Safepoint::kNoLazyDeopt);
        if (FLAG_debug_code) {
          __ stop(GetBailoutReason(kUnexpectedReturnFromWasmTrap));
        }
      }
    }

    bool frame_elided_;
    Instruction* instr_;
    CodeGenerator* gen_;
  };
  bool frame_elided = !frame_access_state()->has_frame();
  auto ool = new (zone()) OutOfLineTrap(this, frame_elided, instr);
  Label* tlabel = ool->entry();
  Label end;

  ArchOpcode op = instr->arch_opcode();
  Condition cond = FlagsConditionToCondition(condition, op);
  if (op == kS390_CmpDouble) {
    // check for unordered if necessary
    if (cond == le) {
      __ bunordered(&end);
      // Unnecessary for eq/lt since only FU bit will be set.
    } else if (cond == gt) {
      __ bunordered(tlabel);
      // Unnecessary for ne/ge since only FU bit will be set.
    }
  }
  __ b(cond, tlabel);
  __ bind(&end);
}

// Assembles boolean materializations after an instruction.
void CodeGenerator::AssembleArchBoolean(Instruction* instr,
                                        FlagsCondition condition) {
  S390OperandConverter i(this, instr);
  ArchOpcode op = instr->arch_opcode();
  bool check_unordered = (op == kS390_CmpDouble || op == kS390_CmpFloat);

  // Overflow checked for add/sub only.
  DCHECK((condition != kOverflow && condition != kNotOverflow) ||
         (op == kS390_Add32 || op == kS390_Add64 || op == kS390_Sub32 ||
          op == kS390_Sub64 || op == kS390_Mul32));

  // Materialize a full 32-bit 1 or 0 value. The result register is always the
  // last output of the instruction.
  DCHECK_NE(0u, instr->OutputCount());
  Register reg = i.OutputRegister(instr->OutputCount() - 1);
  Condition cond = FlagsConditionToCondition(condition, op);
  Label done;
  if (check_unordered) {
    __ LoadImmP(reg, (cond == eq || cond == le || cond == lt) ? Operand::Zero()
                                                              : Operand(1));
    __ bunordered(&done);
  }

  // TODO(john.yan): use load imm high on condition here
  __ LoadImmP(reg, Operand::Zero());
  __ LoadImmP(kScratchReg, Operand(1));
  // locr is sufficient since reg's upper 32 is guarrantee to be 0
  __ locr(cond, reg, kScratchReg);
  __ bind(&done);
}

void CodeGenerator::AssembleArchLookupSwitch(Instruction* instr) {
  S390OperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  for (size_t index = 2; index < instr->InputCount(); index += 2) {
    __ Cmp32(input, Operand(i.InputInt32(index + 0)));
    __ beq(GetLabel(i.InputRpo(index + 1)));
  }
  AssembleArchJump(i.InputRpo(1));
}

void CodeGenerator::AssembleArchTableSwitch(Instruction* instr) {
  S390OperandConverter i(this, instr);
  Register input = i.InputRegister(0);
  int32_t const case_count = static_cast<int32_t>(instr->InputCount() - 2);
  Label** cases = zone()->NewArray<Label*>(case_count);
  for (int32_t index = 0; index < case_count; ++index) {
    cases[index] = GetLabel(i.InputRpo(index + 2));
  }
  Label* const table = AddJumpTable(cases, case_count);
  __ CmpLogicalP(input, Operand(case_count));
  __ bge(GetLabel(i.InputRpo(1)));
  __ larl(kScratchReg, table);
  __ ShiftLeftP(r1, input, Operand(kPointerSizeLog2));
  __ LoadP(kScratchReg, MemOperand(kScratchReg, r1));
  __ Jump(kScratchReg);
}

CodeGenerator::CodeGenResult CodeGenerator::AssembleDeoptimizerCall(
    int deoptimization_id, SourcePosition pos) {
  DeoptimizeKind deoptimization_kind = GetDeoptimizationKind(deoptimization_id);
  DeoptimizeReason deoptimization_reason =
      GetDeoptimizationReason(deoptimization_id);
  Deoptimizer::BailoutType bailout_type =
      deoptimization_kind == DeoptimizeKind::kSoft ? Deoptimizer::SOFT
                                                   : Deoptimizer::EAGER;
  Address deopt_entry = Deoptimizer::GetDeoptimizationEntry(
      isolate(), deoptimization_id, bailout_type);
  // TODO(turbofan): We should be able to generate better code by sharing the
  // actual final call site and just bl'ing to it here, similar to what we do
  // in the lithium backend.
  if (deopt_entry == nullptr) return kTooManyDeoptimizationBailouts;
  if (isolate()->NeedsSourcePositionsForProfiling()) {
    __ RecordDeoptReason(deoptimization_reason, pos, deoptimization_id);
  }
  __ Call(deopt_entry, RelocInfo::RUNTIME_ENTRY);
  return kSuccess;
}

void CodeGenerator::FinishFrame(Frame* frame) {
  CallDescriptor* descriptor = linkage()->GetIncomingDescriptor();
  const RegList double_saves = descriptor->CalleeSavedFPRegisters();

  // Save callee-saved Double registers.
  if (double_saves != 0) {
    frame->AlignSavedCalleeRegisterSlots();
    DCHECK(kNumCalleeSavedDoubles ==
           base::bits::CountPopulation32(double_saves));
    frame->AllocateSavedCalleeRegisterSlots(kNumCalleeSavedDoubles *
                                            (kDoubleSize / kPointerSize));
  }
  // Save callee-saved registers.
  const RegList saves = descriptor->CalleeSavedRegisters();
  if (saves != 0) {
    // register save area does not include the fp or constant pool pointer.
    const int num_saves = kNumCalleeSaved - 1;
    DCHECK(num_saves == base::bits::CountPopulation32(saves));
    frame->AllocateSavedCalleeRegisterSlots(num_saves);
  }
}

void CodeGenerator::AssembleConstructFrame() {
  CallDescriptor* descriptor = linkage()->GetIncomingDescriptor();

  if (frame_access_state()->has_frame()) {
    if (descriptor->IsCFunctionCall()) {
      __ Push(r14, fp);
      __ LoadRR(fp, sp);
    } else if (descriptor->IsJSFunctionCall()) {
      __ Prologue(this->info()->GeneratePreagedPrologue(), ip);
      if (descriptor->PushArgumentCount()) {
        __ Push(kJavaScriptCallArgCountRegister);
      }
    } else {
      StackFrame::Type type = info()->GetOutputStackFrameType();
      // TODO(mbrandy): Detect cases where ip is the entrypoint (for
      // efficient intialization of the constant pool pointer register).
      __ StubPrologue(type);
    }
  }

  int shrink_slots =
      frame()->GetTotalFrameSlotCount() - descriptor->CalculateFixedFrameSize();
  if (info()->is_osr()) {
    // TurboFan OSR-compiled functions cannot be entered directly.
    __ Abort(kShouldNotDirectlyEnterOsrFunction);

    // Unoptimized code jumps directly to this entrypoint while the unoptimized
    // frame is still on the stack. Optimized code uses OSR values directly from
    // the unoptimized frame. Thus, all that needs to be done is to allocate the
    // remaining stack slots.
    if (FLAG_code_comments) __ RecordComment("-- OSR entrypoint --");
    osr_pc_offset_ = __ pc_offset();
    shrink_slots -= OsrHelper(info()).UnoptimizedFrameSlots();
  }

  const RegList double_saves = descriptor->CalleeSavedFPRegisters();
  if (shrink_slots > 0) {
    __ lay(sp, MemOperand(sp, -shrink_slots * kPointerSize));
  }

  // Save callee-saved Double registers.
  if (double_saves != 0) {
    __ MultiPushDoubles(double_saves);
    DCHECK(kNumCalleeSavedDoubles ==
           base::bits::CountPopulation32(double_saves));
  }

  // Save callee-saved registers.
  const RegList saves = descriptor->CalleeSavedRegisters();
  if (saves != 0) {
    __ MultiPush(saves);
    // register save area does not include the fp or constant pool pointer.
  }
}

void CodeGenerator::AssembleReturn(InstructionOperand* pop) {
  CallDescriptor* descriptor = linkage()->GetIncomingDescriptor();
  int pop_count = static_cast<int>(descriptor->StackParameterCount());

  // Restore registers.
  const RegList saves = descriptor->CalleeSavedRegisters();
  if (saves != 0) {
    __ MultiPop(saves);
  }

  // Restore double registers.
  const RegList double_saves = descriptor->CalleeSavedFPRegisters();
  if (double_saves != 0) {
    __ MultiPopDoubles(double_saves);
  }

  S390OperandConverter g(this, nullptr);
  if (descriptor->IsCFunctionCall()) {
    AssembleDeconstructFrame();
  } else if (frame_access_state()->has_frame()) {
    // Canonicalize JSFunction return sites for now unless they have an variable
    // number of stack slot pops
    if (pop->IsImmediate() && g.ToConstant(pop).ToInt32() == 0) {
      if (return_label_.is_bound()) {
        __ b(&return_label_);
        return;
      } else {
        __ bind(&return_label_);
        AssembleDeconstructFrame();
      }
    } else {
      AssembleDeconstructFrame();
    }
  }
  if (pop->IsImmediate()) {
    DCHECK_EQ(Constant::kInt32, g.ToConstant(pop).type());
    pop_count += g.ToConstant(pop).ToInt32();
  } else {
    __ Drop(g.ToRegister(pop));
  }
  __ Drop(pop_count);
  __ Ret();
}

void CodeGenerator::FinishCode() {}

void CodeGenerator::AssembleMove(InstructionOperand* source,
                                 InstructionOperand* destination) {
  S390OperandConverter g(this, nullptr);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      __ Move(g.ToRegister(destination), src);
    } else {
      __ StoreP(src, g.ToMemOperand(destination));
    }
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsRegister() || destination->IsStackSlot());
    MemOperand src = g.ToMemOperand(source);
    if (destination->IsRegister()) {
      __ LoadP(g.ToRegister(destination), src);
    } else {
      Register temp = kScratchReg;
      __ LoadP(temp, src, r0);
      __ StoreP(temp, g.ToMemOperand(destination));
    }
  } else if (source->IsConstant()) {
    Constant src = g.ToConstant(source);
    if (destination->IsRegister() || destination->IsStackSlot()) {
      Register dst =
          destination->IsRegister() ? g.ToRegister(destination) : kScratchReg;
      switch (src.type()) {
        case Constant::kInt32:
#if V8_TARGET_ARCH_S390X
          if (RelocInfo::IsWasmSizeReference(src.rmode())) {
#else
          if (RelocInfo::IsWasmReference(src.rmode())) {
#endif
            __ mov(dst, Operand(src.ToInt32(), src.rmode()));
          } else {
            __ Load(dst, Operand(src.ToInt32()));
          }
          break;
        case Constant::kInt64:
#if V8_TARGET_ARCH_S390X
          if (RelocInfo::IsWasmPtrReference(src.rmode())) {
            __ mov(dst, Operand(src.ToInt64(), src.rmode()));
          } else {
            DCHECK(!RelocInfo::IsWasmSizeReference(src.rmode()));
            __ Load(dst, Operand(src.ToInt64()));
          }
#else
          __ mov(dst, Operand(src.ToInt64()));
#endif  // V8_TARGET_ARCH_S390X
          break;
        case Constant::kFloat32:
          __ Move(dst,
                  isolate()->factory()->NewNumber(src.ToFloat32(), TENURED));
          break;
        case Constant::kFloat64:
          __ Move(dst,
                  isolate()->factory()->NewNumber(src.ToFloat64(), TENURED));
          break;
        case Constant::kExternalReference:
          __ mov(dst, Operand(src.ToExternalReference()));
          break;
        case Constant::kHeapObject: {
          Handle<HeapObject> src_object = src.ToHeapObject();
          Heap::RootListIndex index;
          if (IsMaterializableFromRoot(src_object, &index)) {
            __ LoadRoot(dst, index);
          } else {
            __ Move(dst, src_object);
          }
          break;
        }
        case Constant::kRpoNumber:
          UNREACHABLE();  // TODO(dcarney): loading RPO constants on S390.
          break;
      }
      if (destination->IsStackSlot()) {
        __ StoreP(dst, g.ToMemOperand(destination), r0);
      }
    } else {
      DoubleRegister dst = destination->IsFPRegister()
                               ? g.ToDoubleRegister(destination)
                               : kScratchDoubleReg;
      double value = (src.type() == Constant::kFloat32) ? src.ToFloat32()
                                                        : src.ToFloat64();
      if (src.type() == Constant::kFloat32) {
        __ LoadFloat32Literal(dst, src.ToFloat32(), kScratchReg);
      } else {
        __ LoadDoubleLiteral(dst, value, kScratchReg);
      }

      if (destination->IsFPStackSlot()) {
        __ StoreDouble(dst, g.ToMemOperand(destination));
      }
    }
  } else if (source->IsFPRegister()) {
    DoubleRegister src = g.ToDoubleRegister(source);
    if (destination->IsFPRegister()) {
      DoubleRegister dst = g.ToDoubleRegister(destination);
      __ Move(dst, src);
    } else {
      DCHECK(destination->IsFPStackSlot());
      LocationOperand* op = LocationOperand::cast(source);
      if (op->representation() == MachineRepresentation::kFloat64) {
        __ StoreDouble(src, g.ToMemOperand(destination));
      } else {
        __ StoreFloat32(src, g.ToMemOperand(destination));
      }
    }
  } else if (source->IsFPStackSlot()) {
    DCHECK(destination->IsFPRegister() || destination->IsFPStackSlot());
    MemOperand src = g.ToMemOperand(source);
    if (destination->IsFPRegister()) {
      LocationOperand* op = LocationOperand::cast(source);
      if (op->representation() == MachineRepresentation::kFloat64) {
        __ LoadDouble(g.ToDoubleRegister(destination), src);
      } else {
        __ LoadFloat32(g.ToDoubleRegister(destination), src);
      }
    } else {
      LocationOperand* op = LocationOperand::cast(source);
      DoubleRegister temp = kScratchDoubleReg;
      if (op->representation() == MachineRepresentation::kFloat64) {
        __ LoadDouble(temp, src);
        __ StoreDouble(temp, g.ToMemOperand(destination));
      } else {
        __ LoadFloat32(temp, src);
        __ StoreFloat32(temp, g.ToMemOperand(destination));
      }
    }
  } else {
    UNREACHABLE();
  }
}

void CodeGenerator::AssembleSwap(InstructionOperand* source,
                                 InstructionOperand* destination) {
  S390OperandConverter g(this, nullptr);
  // Dispatch on the source and destination operand kinds.  Not all
  // combinations are possible.
  if (source->IsRegister()) {
    // Register-register.
    Register temp = kScratchReg;
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      Register dst = g.ToRegister(destination);
      __ LoadRR(temp, src);
      __ LoadRR(src, dst);
      __ LoadRR(dst, temp);
    } else {
      DCHECK(destination->IsStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      __ LoadRR(temp, src);
      __ LoadP(src, dst);
      __ StoreP(temp, dst);
    }
#if V8_TARGET_ARCH_S390X
  } else if (source->IsStackSlot() || source->IsFPStackSlot()) {
#else
  } else if (source->IsStackSlot()) {
    DCHECK(destination->IsStackSlot());
#endif
    Register temp_0 = kScratchReg;
    Register temp_1 = r0;
    MemOperand src = g.ToMemOperand(source);
    MemOperand dst = g.ToMemOperand(destination);
    __ LoadP(temp_0, src);
    __ LoadP(temp_1, dst);
    __ StoreP(temp_0, dst);
    __ StoreP(temp_1, src);
  } else if (source->IsFPRegister()) {
    DoubleRegister temp = kScratchDoubleReg;
    DoubleRegister src = g.ToDoubleRegister(source);
    if (destination->IsFPRegister()) {
      DoubleRegister dst = g.ToDoubleRegister(destination);
      __ ldr(temp, src);
      __ ldr(src, dst);
      __ ldr(dst, temp);
    } else {
      DCHECK(destination->IsFPStackSlot());
      MemOperand dst = g.ToMemOperand(destination);
      __ ldr(temp, src);
      __ LoadDouble(src, dst);
      __ StoreDouble(temp, dst);
    }
#if !V8_TARGET_ARCH_S390X
  } else if (source->IsFPStackSlot()) {
    DCHECK(destination->IsFPStackSlot());
    DoubleRegister temp_0 = kScratchDoubleReg;
    DoubleRegister temp_1 = d0;
    MemOperand src = g.ToMemOperand(source);
    MemOperand dst = g.ToMemOperand(destination);
    // TODO(joransiu): MVC opportunity
    __ LoadDouble(temp_0, src);
    __ LoadDouble(temp_1, dst);
    __ StoreDouble(temp_0, dst);
    __ StoreDouble(temp_1, src);
#endif
  } else {
    // No other combinations are possible.
    UNREACHABLE();
  }
}

void CodeGenerator::AssembleJumpTable(Label** targets, size_t target_count) {
  for (size_t index = 0; index < target_count; ++index) {
    __ emit_label_addr(targets[index]);
  }
}

void CodeGenerator::EnsureSpaceForLazyDeopt() {
  if (!info()->ShouldEnsureSpaceForLazyDeopt()) {
    return;
  }

  int space_needed = Deoptimizer::patch_size();
  // Ensure that we have enough space after the previous lazy-bailout
  // instruction for patching the code here.
  int current_pc = masm()->pc_offset();
  if (current_pc < last_lazy_deopt_pc_ + space_needed) {
    int padding_size = last_lazy_deopt_pc_ + space_needed - current_pc;
    DCHECK_EQ(0, padding_size % 2);
    while (padding_size > 0) {
      __ nop();
      padding_size -= 2;
    }
  }
}

#undef __

}  // namespace compiler
}  // namespace internal
}  // namespace v8
