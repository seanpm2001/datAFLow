//===-- dataflow-sa.cpp - Static def/use chain analysis -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Perform a static def/use chain analysis.
///
//===----------------------------------------------------------------------===//

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/WithColor.h>

#include "SVF-FE/LLVMModule.h"
#include "SVF-FE/SVFIRBuilder.h"
#include "Util/config.h"
#include "WPA/Andersen.h"

#include "Config.h"
#include "fuzzalloc/Analysis/VariableRecovery.h"
#include "fuzzalloc/Metadata.h"

using namespace llvm;
using namespace SVF;

namespace dataflow {
//
// Classes
//

/// A variable definition
struct Def {
  Def(const VFGNode *Node, const DIVariable *Var)
      : Node(Node), Val(Node->getValue()), Var(Var) {}

  bool operator==(const Def &Other) const { return Node == Other.Node; }

  const VFGNode *Node;
  const Value *Val;
  const DIVariable *Var;
};

/// A variable use
struct Use {
  Use(const VFGNode *Node, const DIVariable *Var)
      : Node(Node), Val(Node->getValue()), Var(Var),
        Loc(cast<Instruction>(Val)->getDebugLoc()) {}

  bool operator==(const Use &Other) const { return Node == Other.Node; }

  const VFGNode *Node;
  const Value *Val;
  const DIVariable *Var;
  const DILocation *Loc;
};

static json::Value toJSON(const dataflow::Def &Def) {
  const auto &Name = [&]() -> std::string {
    if (const auto *Var = Def.Var) {
      return Var->getName().str();
    }
    std::string VarName;
    raw_string_ostream SS{VarName};

    Def.Val->print(SS, /*IsForDebug=*/true);
    SS.flush();
    return VarName;
  }();

  const auto &Filename = [&]() -> Optional<StringRef> {
    if (const auto *Var = Def.Var) {
      return Var->getFile()->getName();
    }
    return None;
  }();

  const auto &Func = [&]() -> Optional<StringRef> {
    if (const auto *Local = dyn_cast_or_null<DILocalVariable>(Def.Var)) {
      return getDISubprogram(Local->getScope())->getName();
    } else if (const auto *Inst = dyn_cast<Instruction>(Def.Val)) {
      return Inst->getParent()->getName();
    }
    return None;
  }();

  const auto &Line = [&]() -> Optional<unsigned int> {
    if (const auto *Var = Def.Var) {
      return Var->getLine();
    }
    return None;
  }();

  return {Name, {Filename, Func, Line}};
}

static json::Value toJSON(const dataflow::Use &Use) {
  const auto &Name = [&]() -> std::string {
    if (const auto *Var = Use.Var) {
      return Var->getName().str();
    }
    std::string VarName;
    raw_string_ostream SS{VarName};

    Use.Val->print(SS, /*IsForDebug=*/true);
    SS.flush();
    return VarName;
  }();

  const auto &Filename = [&]() -> Optional<StringRef> {
    if (const auto *Loc = Use.Loc) {
      return Loc->getFile()->getName();
    }
    return None;
  }();

  const auto &Func = [&]() -> Optional<StringRef> {
    if (const auto *Loc = Use.Loc) {
      return getDISubprogram(Loc->getScope())->getName();
    } else if (const auto *Inst = dyn_cast<Instruction>(Use.Val)) {
      return Inst->getParent()->getName();
    }
    return None;
  }();

  const auto &Line = [&]() -> Optional<unsigned int> {
    if (const auto *Loc = Use.Loc) {
      return Loc->getLine();
    }
    return None;
  }();

  return {Name, {nullptr, Func, Line}};
}
} // namespace dataflow

namespace {
//
// Constants
//

static constexpr const char *kTaggedMalloc = "__tagged_malloc";
static constexpr const char *kTaggedCalloc = "__tagged_calloc";
static constexpr const char *kTaggedRealloc = "__tagged_realloc";

static const auto &error_stream = []() -> raw_ostream & {
  return WithColor{errs(), HighlightColor::Error} << "[!] ";
};
static const auto &status_stream = []() -> raw_ostream & {
  return WithColor{outs(), HighlightColor::Remark} << "[*] ";
};
static const auto &success_stream = []() -> raw_ostream & {
  return WithColor{outs(), HighlightColor::String} << "[+] ";
};

//
// Command-line options
//

static cl::OptionCategory Cat("Static def/use chain analysis");
static cl::opt<std::string> BCFilename(cl::Positional, cl::desc("<BC file>"),
                                       cl::value_desc("path"), cl::Required,
                                       cl::cat(Cat));
static cl::opt<std::string> OutJSON("out", cl::desc("Output JSON"),
                                    cl::value_desc("path"), cl::cat(Cat));

//
// Helper functions
//

static bool isTaggedAlloc(const Value *V) {
  if (!V) {
    return false;
  }
  if (const auto *I = dyn_cast<Instruction>(V)) {
    return I->hasMetadata() && I->getMetadata(kFuzzallocTaggedAllocMD);
  }
  return false;
}

static bool isInstrumentedDeref(const Value *V) {
  if (!V) {
    return false;
  }
  if (const auto *I = dyn_cast<Instruction>(V)) {
    return I->hasMetadata() && I->getMetadata(kFuzzallocInstrumentedDerefMD);
  }
  return false;
}
} // anonymous namespace

namespace std {
template <> struct hash<dataflow::Def> {
  size_t operator()(const dataflow::Def &Def) const {
    return hash<const VFGNode *>()(Def.Node);
  }
};

template <> struct hash<dataflow::Use> {
  size_t operator()(const dataflow::Use &Use) const {
    return hash<const VFGNode *>()(Use.Node);
  }
};
} // namespace std

int main(int argc, char *argv[]) {
  cl::HideUnrelatedOptions(Cat);
  cl::ParseCommandLineOptions(argc, argv, "Static def/use chain analysis");

  // Parse bitcode
  status_stream() << "Parsing " << BCFilename << "...\n";
  LLVMContext Ctx;
  SMDiagnostic Err;
  auto Mod = parseIRFile(BCFilename, Err, Ctx);
  if (!Mod) {
    error_stream() << "Failed to parse `" << BCFilename
                   << "`: " << Err.getMessage() << '\n';
    ::exit(1);
  }

  // Recover source-level variables
  status_stream() << "Running variable recovery pass...\n";

  legacy::PassManager PM;
  auto *RecoverVars = new VariableRecovery;
  PM.add(RecoverVars);
  PM.run(*Mod);
  const auto &SrcVars = RecoverVars->getVariables();

  status_stream() << "Doing pointer analysis...\n";

  // Initialize external API
  auto *Externals = ExtAPI::getExtAPI(kExtAPIPath);

  auto *SVFMod = LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(*Mod);
  SVFMod->buildSymbolTableInfo();

  // Build SVF IR
  auto *IR = [&SVFMod]() {
    SVFIRBuilder Builder;
    return Builder.build(SVFMod);
  }();

  // Build pointer analysis
  auto *Ander = AndersenWaveDiff::createAndersenWaveDiff(IR);
  auto *VFG = [&Ander]() {
    SVFGBuilder Builder(/*WithIndCall=*/true);
    return Builder.buildFullSVFG(Ander);
  }();

  // Get definitions
  SmallVector<dataflow::Def, 0> Defs;

  status_stream() << "Collecting definitions...\n";
  for (const auto *SVFCallSite : IR->getCallSiteSet()) {
    const auto *CS = SVFCallSite->getCallSite();
    const auto *F = SVFUtil::getCallee(CS);
    if (!F) {
      continue;
    }
    if (F->getName() == kTaggedMalloc || F->getName() == kTaggedCalloc ||
        F->getName() == kTaggedRealloc) {
      (void)isTaggedAlloc;
      assert(isTaggedAlloc(CS) && "Tagged alloc must have fuzzalloc metadata");
      assert((Externals->is_alloc(F) || Externals->is_realloc(F)) &&
             "Tagged function must (re)allocate");
      const auto *PAGNode = IR->getGNode(IR->getValueNode(CS));
      const auto *VNode = VFG->getDefSVFGNode(PAGNode);
      Defs.emplace_back(VNode, SrcVars.lookup(VNode->getValue()));
    }
  }
  if (Defs.empty()) {
    error_stream() << "Failed to collect any def sites\n";
    ::exit(1);
  }
  success_stream() << "Collected " << Defs.size() << " def sites\n";

  // Collect uses
  FIFOWorkList<const VFGNode *> Worklist;
  Set<const VFGNode *> Visited;
  std::unordered_map<dataflow::Def, std::unordered_set<dataflow::Use>>
      DefUseChains;
  unsigned NumUses = 0;

  status_stream() << "Collecting uses...\n";
  for (const auto &Def : Defs) {
    Worklist.clear();
    Visited.clear();

    Worklist.push(Def.Node);
    while (!Worklist.empty()) {
      const auto *Node = Worklist.pop();
      for (const auto *Edge : Node->getOutEdges()) {
        const auto *Succ = Edge->getDstNode();
        if (Visited.insert(Succ).second) {
          Worklist.push(Succ);
        }
      }
    }

    for (const auto *Use : Visited) {
      const auto *UseV = Use->getValue();
      if (!isInstrumentedDeref(UseV)) {
        continue;
      }

      // A use must be a load or store
      const auto *V = [&]() -> const Value * {
        if (const auto *Load = dyn_cast<LoadInst>(UseV)) {
          return Load->getPointerOperand();
        } else if (const auto *Store = dyn_cast<StoreInst>(UseV)) {
          return Store->getPointerOperand();
        }
        llvm_unreachable("use must be a load or store");
      }();

      DefUseChains[Def].emplace(Use, SrcVars.lookup(V));
      NumUses++;
    }
  }
  success_stream() << "Collected " << NumUses << " use sites\n";

  // Save Output JSON
  if (!OutJSON.empty()) {
    std::vector<json::Value> J, JUses;
    J.reserve(DefUseChains.size());

    status_stream() << "Serializing def/use chains to JSON...\n";
    for (const auto &[Def, Uses] : DefUseChains) {
      JUses.clear();
      JUses.reserve(Uses.size());

      for (const auto &Use : Uses) {
        JUses.push_back(Use);
      }

      J.push_back({Def, JUses});
    }

    std::error_code EC;
    raw_fd_ostream OS{OutJSON, EC, sys::fs::OF_Text};
    if (EC) {
      error_stream() << "Unable to open " << OutJSON << '\n';
      ::exit(1);
    }

    status_stream() << "Writing to " << OutJSON << "...\n";
    OS << json::Value{J};
    OS.flush();
    OS.close();
  }

  // Cleanup
  AndersenWaveDiff::releaseAndersenWaveDiff();
  SVFIR::releaseSVFIR();
  LLVMModuleSet::releaseLLVMModuleSet();
  llvm_shutdown();

  return 0;
}
