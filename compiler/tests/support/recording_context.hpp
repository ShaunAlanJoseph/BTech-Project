#pragma once
#include "ssarv/lowering.hpp"
#include <map>
#include <utility>
#include <vector>

namespace fixtures {
class RecordingContext final : public ssarv::LoweringContext {
public:
    std::map<ssarv::ValueId, std::string> registers;
    std::vector<std::string> lines;
    std::vector<std::pair<ssarv::ValueId, std::string>> commits;
    std::vector<std::string> requestedScratch;
    unsigned counter = 0;
    std::string operand(const ssarv::Operand& op, const char* scratch) override {
        requestedScratch.emplace_back(scratch);
        if (op.isReg() && registers.contains(op.value)) return registers.at(op.value);
        emit(op.isReg() ? "ld " + std::string(scratch) + ", 0(sp)"
                        : "li " + std::string(scratch) + ", " + op.text);
        return scratch;
    }
    std::string destination(ssarv::ValueId v, const char* scratch) override {
        return registers.contains(v) ? registers.at(v) : std::string(scratch);
    }
    void storeResult(ssarv::ValueId v, const std::string& reg) override { commits.emplace_back(v, reg); }
    std::string blockLabel(ssarv::BlockId b) const override { return ".Lblock" + std::to_string(b); }
    std::string epilogueLabel() const override { return ".Lepilogue"; }
    std::string freshLabel(std::string_view purpose) override {
        return ".L" + std::string(purpose) + std::to_string(counter++);
    }
    void emit(std::string_view assembly) override { lines.emplace_back(assembly); }
    std::string text() const {
        std::string s; for (const auto& line : lines) s += line; return s;
    }
};
}
