#include <assert.h>
#include <cstdio>
#include <cstring>

#include <set>

#ifndef HAVE_LLVM
#error "This code needs LLVM enabled"
#endif

#include "../git-version.h"

#include <llvm/Analysis/Verifier.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Bitcode/ReaderWriter.h>

#include <iostream>
#include <fstream>
#include "llvm/LLVMDependenceGraph.h"
#include "llvm/PointsTo.h"
#include "llvm/ReachingDefs.h"
#include "llvm/DefUse.h"
#include "llvm/Slicer.h"
#include "Utils.h"

using namespace dg;
using llvm::errs;

static bool slice(llvm::Module *M, const char *slicing_criterion)
{
    debug::TimeMeasure tm;
    LLVMDependenceGraph d;
    std::set<LLVMNode *> callsites;

    // build the graph
    d.build(&*M);

    // verify if the graph is built correctly
    // FIXME - do it optionally (command line argument)
    if (!d.verify())
        errs() << "ERR: verifying failed\n";

    analysis::LLVMPointsToAnalysis PTA(&d);

    tm.start();
    PTA.run();
    tm.stop();
    tm.report("INFO: Points-to analysis took");

    // we might added some new functions
    // so verify once again
    if (!d.verify())
        errs() << "ERR: verifying failed\n";

    // FIXME add command line switch -svcomp and
    // do this only with -svcomp switch
    const char *sc[] = {
        slicing_criterion,
        "klee_assume",
        NULL
    };

    // check for slicing criterion here, because
    // we might have built new subgraphs that contain
    // it during points-to analysis
    tm.start();
    bool ret = d.getCallSites(sc, &callsites);
    tm.stop();

    if (!ret) {
        if (strcmp(slicing_criterion, "ret") == 0) {
            callsites.insert(d.getExit());
            tm.report("INFO: Found slicing criterion in");
        } else {
            errs() << "Did not find slicing criterion: "
                   << slicing_criterion << "\n";
            return false;
        }
    } else
        tm.report("INFO: Found slicing criterion in");


    analysis::LLVMReachingDefsAnalysis RDA(&d);
    tm.start();
    RDA.run();  // compute reaching definitions
    tm.stop();
    tm.report("INFO: Reaching defs analysis took");

    analysis::LLVMDefUseAnalysis DUA(&d);
    tm.start();
    DUA.run(); // add def-use edges according that
    tm.stop();
    tm.report("INFO: Adding Def-Use edges took");

    tm.start();
    // add post-dominator frontiers
    d.computePostDominators(true);
    tm.stop();
    tm.report("INFO: Computing post-dominator frontiers took");

    LLVMSlicer slicer;
    uint32_t slid = 0;

    // do not slice klee_assume at all
    // FIXME: do this optional
    slicer.keepFunctionUntouched("klee_assume");

    tm.start();
    for (LLVMNode *start : callsites)
        slid = slicer.mark(start, slid);

    slicer.slice(&d, nullptr, slid);

    tm.stop();
    tm.report("INFO: Slicing took");

    auto st = slicer.getStatistics();
    errs() << "INFO: Sliced away " << st.second
           << " from " << st.first << " nodes\n";

    return true;
}

static bool array_match(llvm::StringRef name, const char *names[])
{
    unsigned idx = 0;
    while(names[idx]) {
        if (name.equals(names[idx]))
            return true;
        ++idx;
    }

    return false;
}

static void remove_unused_from_module(llvm::Module *M)
{
    using namespace llvm;
    // do not slice away these functions no matter what
    // FIXME do it a vector and fill it dynamically according
    // to what is the setup (like for sv-comp or general..)
    const char *keep[] = {"main", "klee_assume", NULL};

    // when erasing while iterating the slicer crashes
    // so set the to be erased values into container
    // and then erase them
    std::set<Function *> funs;
    std::set<GlobalVariable *> globals;

    for (auto I = M->begin(), E = M->end(); I != E; ++I) {
        Function *func = &*I;
        if (func->hasNUses(0) && !array_match(func->getName(), keep))
            funs.insert(func);
    }

    for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I) {
        GlobalVariable *gv = &*I;
        if (gv->hasNUses(0))
            globals.insert(gv);
    }

    for (Function *f : funs)
        f->eraseFromParent();
    for (GlobalVariable *gv : globals)
        gv->eraseFromParent();
}

static bool verify_module(llvm::Module *M)
{
    // the verifyModule function returns false if there
    // are no errors
    return !llvm::verifyModule(*M, llvm::PrintMessageAction);
}

static bool write_module(llvm::Module *M, const char *module_name)
{
    // compose name
    std::string fl(module_name);
    fl.replace(fl.end() - 3, fl.end(), ".sliced");

    // open stream to write to
    std::ofstream ofs(fl);
    llvm::raw_os_ostream output(ofs);

    // write the module
    errs() << "INFO: saving sliced module to: " << fl.c_str() << "\n";
    llvm::WriteBitcodeToFile(M, output);

    return true;
}

int main(int argc, char *argv[])
{
    llvm::LLVMContext context;
    llvm::SMDiagnostic SMD;
    llvm::Module *M;

    const char *slicing_criterion = NULL;
    const char *module = NULL;

    // parse options
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0
            || strcmp(argv[i], "-version") == 0) {
            errs() << GIT_VERSION << "\n";
            exit(0);
        } else if (strcmp(argv[i], "-c") == 0
            || strcmp(argv[i], "-crit") == 0
            || strcmp(argv[i], "-slice") == 0){
            slicing_criterion = argv[++i];
        } else {
            module = argv[i];
        }
    }

    if (!slicing_criterion || !module) {
        errs() << "Usage: % [-c|-crit|-slice] func_call module\n";
        return 1;
    }

    M = llvm::ParseIRFile(module, SMD, context);
    if (!M) {
        SMD.print(argv[0], errs());
        return 1;
    }

    if (!slice(M, slicing_criterion)) {
        errs() << "ERR: Slicing failed\n";
        return 1;
    }

    remove_unused_from_module(M);

    if (!verify_module(M)) {
        errs() << "ERR: Verifying module failed, the IR is not valid\n";
        errs() << "INFO: Saving anyway so that you can check it\n";
    }

    if (!write_module(M, module)) {
        errs() << "Saving sliced module failed\n";
        return 1;
    }

    return 0;
}
