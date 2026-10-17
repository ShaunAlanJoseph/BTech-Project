#include "ssarv/lowering.hpp"

#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ssarv {

namespace {

constexpr const char* kScratch0 = "t5";
constexpr const char* kScratch1 = "t6";

[[noreturn]] void unsupported(std::string_view category, std::string_view opcode) {
    throw std::invalid_argument("unsupported " + std::string(category) + ": " +
                                std::string(opcode));
}

unsigned integerWidth(std::string_view type) {
    // The compiler targets RV64, so pointers use the same physical width as
    // i64 values. Keep accepting `ptr` anywhere a value's storage width is
    // needed (comparisons, moves, selects, casts, and arithmetic lowering).
    if (type == "ptr") return 64;
    if (type.size() < 2 || type.front() != 'i') {
        throw std::invalid_argument("expected integer type, got '" + std::string(type) + "'");
    }
    unsigned width = 0;
    const auto [end, error] = std::from_chars(type.data() + 1,
                                               type.data() + type.size(), width);
    if (error != std::errc{} || end != type.data() + type.size() ||
        width == 0 || width > 64) {
        throw std::invalid_argument("unsupported integer width '" + std::string(type) + "'");
    }
    return width;
}

unsigned targetWidth(const Instr& in) {
    const auto marker = in.text.rfind(" to ");
    if (marker == std::string::npos) {
        throw std::invalid_argument("cast is missing its target type");
    }
    std::string_view rest(in.text);
    rest.remove_prefix(marker + 4);
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) {
        rest.remove_prefix(1);
    }
    std::size_t end = 0;
    while (end < rest.size() && !std::isspace(static_cast<unsigned char>(rest[end])) &&
           rest[end] != ',') {
        ++end;
    }
    return integerWidth(rest.substr(0, end));
}

Operand operandAt(const Instr& in, std::size_t index, std::string_view operation) {
    if (index < in.operands.size()) return in.operands[index];
    // Synthetic register copies intentionally retain their source only in
    // `uses`. Ordinary parser output retains operands, including duplicates.
    if (index < in.uses.size()) return {in.uses[index], {}};
    throw std::invalid_argument(std::string(operation) + " has too few operands");
}

void requireDestination(const Instr& in) {
    if (in.dst == kNoValue) {
        throw std::invalid_argument(in.opcode + " has no destination");
    }
}

void emitLine(LoweringContext& ctx, std::string assembly);

void emitMove(LoweringContext& ctx, const std::string& destination,
              const std::string& source) {
    if (destination != source) emitLine(ctx, "mv " + destination + ", " + source);
}

// LoweringContext::emit appends bytes to the final assembly stream. Include
// the line ending here so adjacent emitted instructions remain separate.
void emitLine(LoweringContext& ctx, std::string assembly) {
    assembly.push_back('\n');
    ctx.emit(assembly);
}

// Values narrower than XLEN live as sign-extended integers, except i1 values,
// which are represented as 0 or 1. This is the final canonicalisation step
// after an operation which computes only the low-width bit pattern.
void normalise(LoweringContext& ctx, const std::string& destination,
               const std::string& source, unsigned width) {
    if (width == 64) {
        emitMove(ctx, destination, source);
    } else if (width == 1) {
        emitLine(ctx, "andi " + destination + ", " + source + ", 1");
    } else {
        const auto shift = std::to_string(64U - width);
        emitLine(ctx, "slli " + destination + ", " + source + ", " + shift);
        emitLine(ctx, "srai " + destination + ", " + destination + ", " + shift);
    }
}

// Extract an unsigned low-width value without modifying an allocated source.
void zeroExtend(LoweringContext& ctx, const std::string& destination,
                const std::string& source, unsigned width) {
    if (width == 64) {
        emitMove(ctx, destination, source);
    } else if (width == 1) {
        emitLine(ctx, "andi " + destination + ", " + source + ", 1");
    } else {
        const auto shift = std::to_string(64U - width);
        emitLine(ctx, "slli " + destination + ", " + source + ", " + shift);
        emitLine(ctx, "srli " + destination + ", " + destination + ", " + shift);
    }
}

// Interpret low width bits as a signed LLVM integer. This differs from
// normalise for i1: signed i1 true is -1, while normal program values use 1.
void signedBits(LoweringContext& ctx, const std::string& destination,
                const std::string& source, unsigned width) {
    if (width == 1) {
        emitLine(ctx, "andi " + destination + ", " + source + ", 1");
        emitLine(ctx, "neg " + destination + ", " + destination);
        return;
    }
    normalise(ctx, destination, source, width);
}

void normaliseLiteralScratch(LoweringContext& ctx, const Operand& operand,
                             const std::string& reg, const char* scratch,
                             unsigned width) {
    if (!operand.isReg() && reg == scratch) normalise(ctx, reg, reg, width);
}

void commit(LoweringContext& ctx, const Instr& in, const std::string& result) {
    ctx.storeResult(in.dst, result);
}

}  // namespace

void lowerBinary(const Instr& in, LoweringContext& ctx) {
    requireDestination(in);
    const bool supported = in.opcode == "add" || in.opcode == "sub" ||
                           in.opcode == "mul" || in.opcode == "and" ||
                           in.opcode == "or" || in.opcode == "xor" ||
                           in.opcode == "shl" || in.opcode == "lshr" ||
                           in.opcode == "ashr" || in.opcode == "udiv" ||
                           in.opcode == "urem" || in.opcode == "sdiv" ||
                           in.opcode == "srem";
    if (!supported) unsupported("binary opcode", in.opcode);
    const unsigned width = integerWidth(in.type);
    const Operand leftOperand = operandAt(in, 0, in.opcode);
    const Operand rightOperand = operandAt(in, 1, in.opcode);
    const std::string left = ctx.operand(leftOperand, kScratch0);
    const std::string right = ctx.operand(rightOperand, kScratch1);
    normaliseLiteralScratch(ctx, leftOperand, left, kScratch0, width);
    normaliseLiteralScratch(ctx, rightOperand, right, kScratch1, width);
    const std::string destination = ctx.destination(in.dst, kScratch0);

    if (in.opcode == "add") {
        emitLine(ctx, "add " + destination + ", " + left + ", " + right);
    } else if (in.opcode == "sub") {
        emitLine(ctx, "sub " + destination + ", " + left + ", " + right);
    } else if (in.opcode == "mul") {
        emitLine(ctx, "mul " + destination + ", " + left + ", " + right);
    } else if (in.opcode == "and") {
        emitLine(ctx, "and " + destination + ", " + left + ", " + right);
    } else if (in.opcode == "or") {
        emitLine(ctx, "or " + destination + ", " + left + ", " + right);
    } else if (in.opcode == "xor") {
        emitLine(ctx, "xor " + destination + ", " + left + ", " + right);
    } else if (in.opcode == "shl") {
        emitLine(ctx, "sll " + destination + ", " + left + ", " + right);
    } else if (in.opcode == "lshr") {
        emitMove(ctx, kScratch1, right);
        zeroExtend(ctx, destination, left, width);
        emitLine(ctx, "srl " + destination + ", " + destination + ", " + kScratch1);
    } else if (in.opcode == "ashr") {
        emitMove(ctx, kScratch1, right);
        signedBits(ctx, destination, left, width);
        emitLine(ctx, "sra " + destination + ", " + destination + ", " + kScratch1);
    } else if (in.opcode == "udiv" || in.opcode == "urem") {
        emitMove(ctx, kScratch1, right);
        zeroExtend(ctx, destination, left, width);
        zeroExtend(ctx, kScratch1, kScratch1, width);
        emitLine(ctx, std::string(in.opcode == "udiv" ? "divu " : "remu ") +
                 destination + ", " + destination + ", " + kScratch1);
    } else if (in.opcode == "sdiv" || in.opcode == "srem") {
        emitMove(ctx, kScratch1, right);
        signedBits(ctx, destination, left, width);
        signedBits(ctx, kScratch1, kScratch1, width);
        emitLine(ctx, std::string(in.opcode == "sdiv" ? "div " : "rem ") +
                 destination + ", " + destination + ", " + kScratch1);
    }

    normalise(ctx, destination, destination, width);
    commit(ctx, in, destination);
}

void lowerCompare(const Instr& in, LoweringContext& ctx) {
    requireDestination(in);
    if (in.opcode != "icmp") unsupported("comparison opcode", in.opcode);
    const bool supportedPredicate = in.subop == "eq" || in.subop == "ne" ||
                                    in.subop == "ult" || in.subop == "ugt" ||
                                    in.subop == "ule" || in.subop == "uge" ||
                                    in.subop == "slt" || in.subop == "sgt" ||
                                    in.subop == "sle" || in.subop == "sge";
    if (!supportedPredicate) unsupported("icmp predicate", in.subop);
    const unsigned width = integerWidth(in.type);
    const Operand leftOperand = operandAt(in, 0, "icmp");
    const Operand rightOperand = operandAt(in, 1, "icmp");
    const std::string left = ctx.operand(leftOperand, kScratch0);
    const std::string right = ctx.operand(rightOperand, kScratch1);
    normaliseLiteralScratch(ctx, leftOperand, left, kScratch0, width);
    normaliseLiteralScratch(ctx, rightOperand, right, kScratch1, width);
    const std::string destination = ctx.destination(in.dst, kScratch0);

    if (in.subop == "eq" || in.subop == "ne") {
        // The physical register representation may contain sign-extension
        // bits above the LLVM value width. Mask/canonicalise both operands
        // before equality, including register operands (not just literals).
        normalise(ctx, kScratch0, left, width);
        normalise(ctx, kScratch1, right, width);
        emitLine(ctx, "xor " + destination + ", " + kScratch0 + ", " + kScratch1);
        emitLine(ctx, std::string(in.subop == "eq" ? "seqz " : "snez ") +
                 destination + ", " + destination);
    } else if (in.subop == "ult" || in.subop == "ugt" ||
               in.subop == "ule" || in.subop == "uge") {
        emitMove(ctx, kScratch1, right);
        zeroExtend(ctx, destination, left, width);
        zeroExtend(ctx, kScratch1, kScratch1, width);
        if (in.subop == "ult") {
            emitLine(ctx, "sltu " + destination + ", " + destination + ", " + kScratch1);
        } else if (in.subop == "ugt") {
            emitLine(ctx, "sltu " + destination + ", " + kScratch1 + ", " + destination);
        } else if (in.subop == "ule") {
            emitLine(ctx, "sltu " + destination + ", " + kScratch1 + ", " + destination);
            emitLine(ctx, "xori " + destination + ", " + destination + ", 1");
        } else {
            emitLine(ctx, "sltu " + destination + ", " + destination + ", " + kScratch1);
            emitLine(ctx, "xori " + destination + ", " + destination + ", 1");
        }
    } else if (in.subop == "slt" || in.subop == "sgt" ||
               in.subop == "sle" || in.subop == "sge") {
        emitMove(ctx, kScratch1, right);
        signedBits(ctx, destination, left, width);
        signedBits(ctx, kScratch1, kScratch1, width);
        if (in.subop == "slt") {
            emitLine(ctx, "slt " + destination + ", " + destination + ", " + kScratch1);
        } else if (in.subop == "sgt") {
            emitLine(ctx, "slt " + destination + ", " + kScratch1 + ", " + destination);
        } else if (in.subop == "sle") {
            emitLine(ctx, "slt " + destination + ", " + kScratch1 + ", " + destination);
            emitLine(ctx, "xori " + destination + ", " + destination + ", 1");
        } else {
            emitLine(ctx, "slt " + destination + ", " + destination + ", " + kScratch1);
            emitLine(ctx, "xori " + destination + ", " + destination + ", 1");
        }
    }

    // Every icmp result is i1, irrespective of the compared operand width.
    normalise(ctx, destination, destination, 1);
    commit(ctx, in, destination);
}

void lowerCast(const Instr& in, LoweringContext& ctx) {
    requireDestination(in);
    if (in.opcode != "trunc" && in.opcode != "zext" &&
        in.opcode != "sext" && in.opcode != "bitcast") {
        unsupported("cast opcode", in.opcode);
    }
    const unsigned sourceWidth = integerWidth(in.type);
    const unsigned destinationWidth = targetWidth(in);
    if ((in.opcode == "trunc" && destinationWidth >= sourceWidth) ||
        ((in.opcode == "zext" || in.opcode == "sext") &&
         destinationWidth <= sourceWidth) ||
        (in.opcode == "bitcast" && destinationWidth != sourceWidth)) {
        throw std::invalid_argument("invalid source/target widths for " + in.opcode);
    }
    const Operand sourceOperand = operandAt(in, 0, in.opcode);
    const std::string source = ctx.operand(sourceOperand, kScratch0);
    const std::string destination = ctx.destination(in.dst, kScratch0);

    if (in.opcode == "trunc") {
        normalise(ctx, destination, source, destinationWidth);
    } else if (in.opcode == "zext") {
        zeroExtend(ctx, destination, source, sourceWidth);
        normalise(ctx, destination, destination, destinationWidth);
    } else if (in.opcode == "sext") {
        signedBits(ctx, destination, source, sourceWidth);
        normalise(ctx, destination, destination, destinationWidth);
    } else if (in.opcode == "bitcast") {
        normalise(ctx, destination, source, destinationWidth);
    }
    commit(ctx, in, destination);
}

void lowerSelect(const Instr& in, LoweringContext& ctx) {
    requireDestination(in);
    if (in.opcode != "select") unsupported("select opcode", in.opcode);
    const unsigned width = integerWidth(in.type);
    const Operand condition = operandAt(in, 0, "select");
    const Operand trueValue = operandAt(in, 1, "select");
    const Operand falseValue = operandAt(in, 2, "select");
    const std::string destination = ctx.destination(in.dst, kScratch0);

    // Branching avoids keeping three possibly-spilled inputs live with only
    // the two scratch registers that the backend reserves for lowering.
    const std::string conditionReg = ctx.operand(condition, kScratch0);
    normaliseLiteralScratch(ctx, condition, conditionReg, kScratch0, 1);
    const std::string falseLabel = ctx.freshLabel("select.false");
    const std::string endLabel = ctx.freshLabel("select.end");
    emitLine(ctx, "beqz " + conditionReg + ", " + falseLabel);

    const std::string selectedTrue = ctx.operand(trueValue, kScratch0);
    emitMove(ctx, destination, selectedTrue);
    normalise(ctx, destination, destination, width);
    emitLine(ctx, "j " + endLabel);

    emitLine(ctx, falseLabel + ":");
    const std::string selectedFalse = ctx.operand(falseValue, kScratch0);
    emitMove(ctx, destination, selectedFalse);
    normalise(ctx, destination, destination, width);
    emitLine(ctx, endLabel + ":");
    commit(ctx, in, destination);
}

void lowerMove(const Instr& in, LoweringContext& ctx) {
    requireDestination(in);
    if (in.opcode != "mov") unsupported("move opcode", in.opcode);
    const unsigned width = integerWidth(in.type);
    const Operand sourceOperand = operandAt(in, 0, "mov");
    const std::string source = ctx.operand(sourceOperand, kScratch0);
    const std::string destination = ctx.destination(in.dst, kScratch0);
    emitMove(ctx, destination, source);
    normalise(ctx, destination, destination, width);
    commit(ctx, in, destination);
}

void lowerTerminator(const Instr& in, LoweringContext& ctx) {
    if (in.opcode == "br") {
        if (in.succs.size() == 1) {
            emitLine(ctx, "j " + ctx.blockLabel(in.succs[0]));
            return;
        }
        if (in.succs.size() == 2) {
            const Operand condition = operandAt(in, 0, "conditional branch");
            const std::string conditionReg = ctx.operand(condition, kScratch0);
            normaliseLiteralScratch(ctx, condition, conditionReg, kScratch0, 1);
            emitLine(ctx, "bnez " + conditionReg + ", " + ctx.blockLabel(in.succs[0]));
            emitLine(ctx, "j " + ctx.blockLabel(in.succs[1]));
            return;
        }
        throw std::invalid_argument("branch must have one or two successors");
    }

    if (in.opcode == "switch") {
        if (in.succs.empty()) throw std::invalid_argument("switch has no default successor");
        for (const auto& arm : in.cases) {
            if (arm.target == kNoBlock) throw std::invalid_argument("switch case has no target");
        }
        const unsigned width = integerWidth(in.type);
        const Operand value = operandAt(in, 0, "switch");
        const std::string valueReg = ctx.operand(value, kScratch0);
        normaliseLiteralScratch(ctx, value, valueReg, kScratch0, width);
        for (const auto& arm : in.cases) {
            Operand caseValue{kNoValue, arm.value};
            const std::string caseReg = ctx.operand(caseValue, kScratch1);
            normalise(ctx, caseReg, caseReg, width);
            emitLine(ctx, "beq " + valueReg + ", " + caseReg + ", " +
                     ctx.blockLabel(arm.target));
        }
        emitLine(ctx, "j " + ctx.blockLabel(in.succs[0]));
        return;
    }

    if (in.opcode == "ret") {
        if (!in.operands.empty() || !in.uses.empty()) {
            const Operand value = operandAt(in, 0, "return");
            const std::string valueReg = ctx.operand(value, kScratch0);
            emitMove(ctx, "a0", valueReg);
            if (in.type != "void" && !in.type.empty()) {
                normalise(ctx, "a0", "a0", integerWidth(in.type));
            }
        }
        emitLine(ctx, "j " + ctx.epilogueLabel());
        return;
    }

    if (in.opcode == "unreachable") {
        emitLine(ctx, "ebreak");
        return;
    }
    unsupported("terminator opcode", in.opcode);
}

}  // namespace ssarv
