#include <gtest/gtest.h>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include "support/recording_context.hpp"
using namespace ssarv;
TEST(Lowering, BinaryCommitsThroughContextAndUsesReservedScratch) {
    fixtures::RecordingContext ctx; ctx.registers = {{0,"a0"},{1,"a1"},{2,"a0"}};
    Instr in; in.opcode = "add"; in.type = "i32"; in.dst = 2; in.uses = {0,1};
    in.operands = {{0,"%x"},{1,"%y"}}; lowerBinary(in, ctx);
    ASSERT_EQ(ctx.commits.size(), 1u); EXPECT_EQ(ctx.commits[0].first, 2u);
    EXPECT_EQ(ctx.commits[0].second, "a0"); EXPECT_FALSE(ctx.lines.empty());
    for (const auto& scratch : ctx.requestedScratch) EXPECT_TRUE(scratch == "t5" || scratch == "t6");
}
TEST(Lowering, RegisterAndLiteralMovesCommitSpilledDestination) {
    for (bool literal : {false,true}) {
        fixtures::RecordingContext ctx; ctx.registers = {{0,"a0"}};
        Instr in; in.opcode = "mov"; in.type = "i32"; in.dst = 1;
        if (literal) in.operands = {{kNoValue,"17"}}; else in.uses = {0};
        lowerMove(in, ctx); ASSERT_EQ(ctx.commits.size(), 1u); EXPECT_EQ(ctx.commits[0].first, 1u);
    }
}
TEST(Lowering, CompareCastAndSelectUseContextResults) {
    for (const char* opcode : {"icmp", "trunc", "select"}) {
        fixtures::RecordingContext ctx; ctx.registers = {{0,"a0"},{1,"a1"},{2,"a2"}};
        Instr in; in.opcode = opcode; in.type = "i32"; in.dst = 2;
        in.operands = {{0,"%x"},{1,"%y"}};
        if (in.opcode == "icmp") { in.subop = "ult"; lowerCompare(in,ctx); }
        else if (in.opcode == "trunc") { in.operands.resize(1); in.text = "trunc i32 %x to i1"; lowerCast(in,ctx); }
        else { in.operands = {{0,"%c"},{1,"%x"},{kNoValue,"3"}}; lowerSelect(in,ctx); }
        ASSERT_EQ(ctx.commits.size(), 1u); EXPECT_EQ(ctx.commits[0].first, 2u);
        EXPECT_FALSE(ctx.lines.empty());
    }
}
TEST(Lowering, BranchAndReturnUseContextLabels) {
    fixtures::RecordingContext ctx; ctx.registers = {{0,"a0"}};
    Instr br; br.opcode = "br"; br.succs = {3}; lowerTerminator(br,ctx);
    EXPECT_NE(ctx.text().find(".Lblock3"), std::string::npos);
    Instr ret; ret.opcode = "ret"; ret.type = "i32"; ret.operands = {{0,"%x"}};
    lowerTerminator(ret,ctx); EXPECT_NE(ctx.text().find(".Lepilogue"), std::string::npos);
    EXPECT_TRUE(ctx.commits.empty());
}
TEST(Lowering, IntegerArithmeticCoversBitwiseShiftsAndDivision) {
    for (const char* opcode : {"add", "sub", "mul", "and", "or", "xor",
                               "shl", "lshr", "ashr", "sdiv", "udiv", "srem", "urem"}) {
        fixtures::RecordingContext ctx;
        // Destination deliberately aliases the second source. Multi-step
        // unsigned/signed lowering must preserve that source first.
        ctx.registers = {{0, "a0"}, {1, "a1"}, {2, "a1"}};
        Instr in; in.opcode = opcode; in.type = "i33"; in.dst = 2;
        in.uses = {0, 1}; in.operands = {{0, "%x"}, {1, "%y"}};
        lowerBinary(in, ctx);
        ASSERT_EQ(ctx.commits, (std::vector<std::pair<ValueId, std::string>>{{2, "a1"}}));
        EXPECT_FALSE(ctx.lines.empty()) << opcode;
        EXPECT_NE(ctx.text().find("slli a1, a1, 31"), std::string::npos) << opcode;
    }
}
TEST(Lowering, AllIntegerComparisonPredicatesYieldCommittedI1) {
    for (const char* predicate : {"eq", "ne", "slt", "sle", "sgt", "sge",
                                  "ult", "ule", "ugt", "uge"}) {
        fixtures::RecordingContext ctx;
        ctx.registers = {{0, "a0"}, {1, "a1"}, {2, "a1"}};
        Instr in; in.opcode = "icmp"; in.subop = predicate; in.type = "i8"; in.dst = 2;
        in.operands = {{0, "%x"}, {1, "%y"}};
        lowerCompare(in, ctx);
        ASSERT_EQ(ctx.commits.size(), 1u);
        EXPECT_EQ(ctx.commits[0].second, "a1");
        EXPECT_NE(ctx.text().find("andi a1, a1, 1"), std::string::npos);
    }
}
TEST(Lowering, EmitsNewlineTerminatedTextSupportsPointersAndNormalisesNarrowEquality) {
    fixtures::RecordingContext narrowContext;
    narrowContext.registers = {{0, "a0"}, {1, "a1"}, {2, "a0"}};
    Instr narrow; narrow.opcode = "icmp"; narrow.subop = "eq";
    narrow.type = "i7"; narrow.dst = 2;
    narrow.operands = {{0, "%x"}, {1, "%y"}};
    lowerCompare(narrow, narrowContext);

    const std::string narrowAssembly = narrowContext.text();
    EXPECT_NE(narrowAssembly.find("slli t5, a0, 57\nsrai t5, t5, 57\n"),
              std::string::npos);
    EXPECT_NE(narrowAssembly.find("slli t6, a1, 57\nsrai t6, t6, 57\n"),
              std::string::npos);
    for (const auto& fragment : narrowContext.lines) {
        ASSERT_FALSE(fragment.empty());
        EXPECT_EQ(fragment.back(), '\n');
    }

    fixtures::RecordingContext pointerContext;
    pointerContext.registers = {{0, "a0"}, {1, "a1"}, {2, "a2"}};
    Instr pointer; pointer.opcode = "icmp"; pointer.subop = "eq";
    pointer.type = "ptr"; pointer.dst = 2;
    pointer.operands = {{0, "%left"}, {1, "%right"}};
    EXPECT_NO_THROW(lowerCompare(pointer, pointerContext));

    Instr move; move.opcode = "mov"; move.type = "ptr"; move.dst = 3;
    move.operands = {{0, "%pointer"}};
    EXPECT_NO_THROW(lowerMove(move, pointerContext));
    EXPECT_EQ(pointerContext.commits.size(), 2u);
}
TEST(Lowering, CastsHandleSignZeroTruncationAndNoopBitcast) {
    for (const auto& [opcode, source, target] :
         std::initializer_list<std::tuple<const char*, const char*, const char*>>{
             {"trunc", "i64", "i1"}, {"zext", "i1", "i33"},
             {"sext", "i1", "i64"}, {"bitcast", "i32", "i32"}}) {
        fixtures::RecordingContext ctx; ctx.registers = {{0, "a0"}, {1, "a1"}};
        Instr in; in.opcode = opcode; in.type = source; in.dst = 1;
        in.operands = {{0, "%x"}};
        in.text = std::string(opcode) + " " + source + " %x to " + target;
        lowerCast(in, ctx);
        EXPECT_EQ(ctx.commits.size(), 1u) << opcode;
        EXPECT_FALSE(ctx.lines.empty()) << opcode;
    }
}
TEST(Lowering, ConditionalSwitchAndUnreachableTerminatorsEmitValidControlFlow) {
    fixtures::RecordingContext ctx; ctx.registers = {{0, "a0"}};
    Instr branch; branch.opcode = "br"; branch.succs = {1, 2};
    branch.operands = {{0, "%condition"}};
    lowerTerminator(branch, ctx);
    EXPECT_NE(ctx.text().find("bnez a0, .Lblock1"), std::string::npos);
    EXPECT_NE(ctx.text().find("j .Lblock2"), std::string::npos);

    Instr sw; sw.opcode = "switch"; sw.type = "i8"; sw.succs = {3, 4};
    sw.operands = {{0, "%value"}}; sw.cases = {{"255", 4, "%case"}};
    lowerTerminator(sw, ctx);
    EXPECT_NE(ctx.text().find("beq a0, t6, .Lblock4"), std::string::npos);
    EXPECT_NE(ctx.text().find("j .Lblock3"), std::string::npos);

    Instr unreachable; unreachable.opcode = "unreachable";
    lowerTerminator(unreachable, ctx);
    EXPECT_NE(ctx.text().find("ebreak"), std::string::npos);
}
TEST(Lowering, SelectUsesControlFlowForSpilledInputsAndUnsupportedOpsDiagnose) {
    fixtures::RecordingContext ctx;
    Instr select; select.opcode = "select"; select.type = "i8"; select.dst = 3;
    select.operands = {{kNoValue, "1"}, {kNoValue, "255"}, {kNoValue, "2"}};
    lowerSelect(select, ctx);
    EXPECT_NE(ctx.text().find("select.false"), std::string::npos);
    EXPECT_NE(ctx.text().find("select.end"), std::string::npos);
    ASSERT_EQ(ctx.commits.size(), 1u);
    EXPECT_EQ(ctx.commits[0].second, "t5");

    Instr bad; bad.opcode = "fadd"; bad.type = "i32"; bad.dst = 0;
    bad.operands = {{kNoValue, "1"}, {kNoValue, "2"}};
    EXPECT_THROW(lowerBinary(bad, ctx), std::invalid_argument);
}
