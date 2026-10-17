// Group 3's boundary to the shared RV64IM backend. Interface version 1.
#pragma once
#include <string>
#include <string_view>
#include "ssarv/ir.hpp"

namespace ssarv {

// Group 4 implements these services. Each lowering call has exclusive use
// of t5/t6; operand() may overwrite its requested scratch register.
// Operands must be read before an aliased destination is overwritten.
class LoweringContext {
public:
    virtual ~LoweringContext() = default;
    virtual std::string operand(const Operand& op, const char* scratch) = 0;
    virtual std::string destination(ValueId value, const char* scratch) = 0;
    virtual void storeResult(ValueId value, const std::string& reg) = 0;
    virtual std::string blockLabel(BlockId block) const = 0;
    virtual std::string epilogueLabel() const = 0;
    virtual std::string freshLabel(std::string_view purpose) = 0;
    virtual void emit(std::string_view assembly) = 0;
};

// Preconditions: supported, structurally valid scalar integer/pointer IR; phi-free;
// valid allocation; distinct scratch registers t5/t6 reserved by backend.
// No function below changes IR/allocation, creates spill slots directly,
// or changes sp. Results are committed through storeResult().
void lowerBinary(const Instr& in, LoweringContext& ctx);
void lowerCompare(const Instr& in, LoweringContext& ctx);
void lowerCast(const Instr& in, LoweringContext& ctx);
void lowerSelect(const Instr& in, LoweringContext& ctx);
void lowerMove(const Instr& in, LoweringContext& ctx);
void lowerTerminator(const Instr& in, LoweringContext& ctx);

}  // namespace ssarv
