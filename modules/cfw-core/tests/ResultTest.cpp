// Result<T> and Error: success and failure paths, moves, context, and that
// valueOr drops the error rather than touching a missing value.

#include "cfw/core/Result.h"

#include <memory>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

Result<int> halve(int value) {
    if (value % 2 != 0) {
        return Error(ErrorCode::InvalidArgument, "odd").with("value", std::to_string(value));
    }
    return value / 2;
}

Result<void> requireEven(int value) {
    if (value % 2 != 0) {
        return Error(ErrorCode::InvalidArgument, "odd");
    }
    return success();
}

void successCarriesTheValue() {
    const Result<int> result = halve(10);
    check(result.ok(), "halve(10) succeeds");
    check(static_cast<bool>(result), "a successful Result is truthy");
    checkEqual(result.value(), 5, "halve(10) is 5");
}

void failureCarriesCodeMessageAndContext() {
    const Result<int> result = halve(3);
    check(!result.ok(), "halve(3) fails");
    checkEqual(result.error().code(), ErrorCode::InvalidArgument, "error code is kept");
    checkEqual(result.error().message(), String("odd"), "error message is kept");
    checkEqual(result.error().context().size(), std::size_t(1), "one context pair");
    checkEqual(result.error().describe(), String("InvalidArgument: odd (value=3)"), "describe() formats everything");
}

void valueOrFallsBackOnlyOnFailure() {
    checkEqual(halve(8).valueOr(-1), 4, "valueOr returns the value on success");
    checkEqual(halve(7).valueOr(-1), -1, "valueOr returns the fallback on failure");
}

void moveOnlyValuesMoveOut() {
    auto make = []() -> Result<std::unique_ptr<int>> { return std::make_unique<int>(42); };
    std::unique_ptr<int> owned = make().value();
    check(owned && *owned == 42, "a move-only value moves out of an rvalue Result");
}

void implicitConversionFromConvertibleValues() {
    auto make = []() -> Result<String> { return "text"; };
    checkEqual(make().value(), String("text"), "Result<String> accepts a string literal");
}

void voidResults() {
    check(requireEven(2).ok(), "Result<void> success");
    const Result<void> failed = requireEven(1);
    check(!failed.ok(), "Result<void> failure");
    checkEqual(failed.error().code(), ErrorCode::InvalidArgument, "Result<void> keeps the error");
}

void errorCodeNamesAreStable() {
    checkEqual(String(errorCodeName(ErrorCode::ParseError)), String("ParseError"), "ParseError name");
    checkEqual(static_cast<int>(ErrorCode::NetworkError), 15, "NetworkError value never changes");
}

} // namespace

int main() {
    successCarriesTheValue();
    failureCarriesCodeMessageAndContext();
    valueOrFallsBackOnlyOnFailure();
    moveOnlyValuesMoveOut();
    implicitConversionFromConvertibleValues();
    voidResults();
    errorCodeNamesAreStable();
    return cfw::test::finish("ResultTest");
}
