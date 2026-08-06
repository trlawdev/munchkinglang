#include "munx_process.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace
{

using munx::test::compile_source;
using munx::test::process_result;
using munx::test::run_source;
using munx::test::run_source_interp;
using munx::test::source_root;

std::filesystem::path program_path(const char *relative)
{
    return source_root() / "tests" / "programs" / relative;
}

std::filesystem::path sample_valid(const char *name)
{
    return source_root() / "sample" / "valid" / name;
}

std::filesystem::path sample_bench(const char *name)
{
    return source_root() / "sample" / "bench" / name;
}

void expect_success(const process_result &result, const char *label)
{
    EXPECT_EQ(result.exit_code, 0) << label << "\nstdout:\n"
                                   << result.stdout_text << "\nstderr:\n"
                                   << result.stderr_text;
}

void expect_stdout_contains(const process_result &result,
                            const std::string &needle)
{
    EXPECT_NE(result.stdout_text.find(needle), std::string::npos)
        << "expected stdout to contain: " << needle << "\nactual:\n"
        << result.stdout_text << "\nstderr:\n"
        << result.stderr_text;
}

} // namespace

TEST(CompileRun, HelloPrintsGreeting)
{
    const auto result = run_source(program_path("hello.mx"));
    expect_success(result, "hello.mx");
    expect_stdout_contains(result, "hello");
}

TEST(CompileRun, ArithmeticProducesExpectedValues)
{
    const auto result = run_source(program_path("arithmetic.mx"));
    expect_success(result, "arithmetic.mx");
    expect_stdout_contains(result, "a=7");
    expect_stdout_contains(result, "b=9");
    expect_stdout_contains(result, "c=8");
    expect_stdout_contains(result, "d=1");
}

TEST(CompileRun, LoopsAccumulateSum)
{
    const auto result = run_source(program_path("loops.mx"));
    expect_success(result, "loops.mx");
    expect_stdout_contains(result, "sum=4950");
}

TEST(CompileRun, FunctionsReturnValues)
{
    const auto result = run_source(program_path("functions.mx"));
    expect_success(result, "functions.mx");
    expect_stdout_contains(result, "add=7");
    expect_stdout_contains(result, "double=42");
}

TEST(CompileRun, IfElseSelectsBranch)
{
    const auto result = run_source(program_path("if_else.mx"));
    expect_success(result, "if_else.mx");
    expect_stdout_contains(result, "result=1");
}

TEST(CompileRun, LoopIfElseWorksUnderInterpreter)
{
    const auto result = run_source_interp(program_path("loop_if_interp.mx"));
    expect_success(result, "loop_if_interp.mx");
    expect_stdout_contains(result, "hits=5");
}

TEST(CompileRun, ArgvIsVisibleToProgram)
{
    const auto result = run_source(program_path("argv_echo.mx"), {"alpha"});
    expect_success(result, "argv_echo.mx");
    expect_stdout_contains(result, "argc=1");
    expect_stdout_contains(result, "arg0=alpha");
}

TEST(CompileRun, InterpreterMatchesJitOnLoops)
{
    const auto jit = run_source(program_path("loops.mx"));
    const auto interp = run_source_interp(program_path("loops.mx"));
    expect_success(jit, "loops.mx jit");
    expect_success(interp, "loops.mx interp");
    EXPECT_EQ(jit.stdout_text, interp.stdout_text);
}

TEST(CompileOnly, ValidSamplesCompile)
{
    // `01_package_imports.mx` references packages that are not resolvable as
    // local sources in this tree; the rest of sample/valid should compile.
    static const char *const k_samples[] = {
        "02_comments.mx",
        "03_numeric_literals.mx",
        "04_string_literals.mx",
        "05_char_literals.mx",
        "06_bool_null_regex_literals.mx",
        "07_collection_literals.mx",
        "08_arithmetic_precedence.mx",
        "09_comparison_logical.mx",
        "10_unary_operators.mx",
        "12_types.mx",
        "13_cast.mx",
        "14_functions.mx",
        "15_lambdas.mx",
        "16_if_else.mx",
        "19_enums.mx",
        "20_objects.mx",
        "21_assignments.mx",
        "29_semicolons_and_layout.mx",
        "30_edge_cases.mx",
        "33_maps.mx",
    };

    for (const char *name : k_samples)
    {
        const auto result = compile_source(sample_valid(name));
        expect_success(result, name);
        expect_stdout_contains(result, "wrote ");
    }
}

TEST(CompileRun, BranchBenchmarksProduceStableOutput)
{
    {
        const auto result = run_source(sample_bench("branch_count.mx"));
        expect_success(result, "branch_count.mx");
        expect_stdout_contains(result, "sum=499999500000");
    }
    {
        const auto result = run_source(sample_bench("branch_random.mx"));
        expect_success(result, "branch_random.mx");
        expect_stdout_contains(result, "hits=500000");
    }
}
