// Bridge from LLVM-C to LLVM C++ API for SwitchInst case-value
// retrieval. `LLVMGetSwitchCaseValue` landed in upstream
// `llvm-c/Core.h` fairly late: mstorsjo/llvm-22 carries it,
// Ubuntu 24.04's llvm-19 packages do not, and earlier releases
// definitely do not. Implementing our own thin wrapper around the
// C++ `SwitchInst::CaseHandle::getCaseValue()` decouples the
// translator from that version skew at no runtime cost — the wrapper
// is one std::advance + one cast.
//
// Exposed under a CVM-prefixed name so it can't clash with the
// upstream symbol when both are present.

#include "llvm/IR/Constants.h"     /* ConstantInt definition for the upcast */
#include "llvm/IR/Instructions.h"  /* SwitchInst, CaseHandle */
#include "llvm-c/Core.h"

#include <iterator>

extern "C" {

// `i` is the LLVMGetSuccessor() index of the case: 0 is the default
// destination, 1..N map to case_begin() + (i - 1).
LLVMValueRef cvm_llvm_get_switch_case_value(LLVMValueRef SI, unsigned i) {
    llvm::SwitchInst *S =
        llvm::cast<llvm::SwitchInst>(llvm::unwrap(SI));
    auto It = S->case_begin();
    std::advance(It, static_cast<int>(i) - 1);
    return llvm::wrap(It->getCaseValue());
}

} // extern "C"
