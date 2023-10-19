// This needs to be at the top. The order of includes is also important. This is fine.
#include "Export.h"
#include "ModuleSelector.h"
#include <array>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <glib.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <s2e/S2E.h>
#include <s2e/Utils.h>
#include <s2e/cpu.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <tcg/tcg-llvm.h>
#include <unistd.h>

#define WRITE_LLVM_SRC true
#define BINARY_SYMLINK_NAME "binary"
#define BINARY_PATH_ENVNAME "S2E_BINARY"

using namespace binrec;
using namespace llvm;
using namespace std;

namespace s2e::plugins {
    Export::Export(S2E *s2e) :
            Plugin(s2e),
            m_module(NULL),
            m_exportCounter(0),
            m_regenerateBlocks(true),
            m_exportInterval(0)
    {
    }

    Export::~Export()
    {
        saveLLVMModule(false);
    }

    void Export::initialize()
    {
        ti = TraceInfo::get();
    }

    static inline auto fileExists(const string &name) -> bool
    {
        struct stat buffer {};
        return stat(name.c_str(), &buffer) == 0;
    }

    void Export::initializeModule(
        const ModuleDescriptor &module,
        const ConfigFile::string_list &baseDirs)
    {
        m_moduleDesc = module;
        const char *envPath = getenv(BINARY_PATH_ENVNAME);
        string path;

        if (!envPath) {
            foreach2(it, baseDirs.begin(), baseDirs.end())
            {
                const string &baseDir = *it;

                if (fileExists(baseDir + "/" + module.Name)) {
                    path = baseDir + "/" + module.Name;
                    break;
                }
            }

            assert(!path.empty() && "binary path not found in environment or base dirs");
        } else {
            path = envPath;
        }

        // get full path to binary
        array<char, PATH_MAX> fullProgramPath{};
        assert(realpath(path.c_str(), fullProgramPath.data()));

        // create symlink to binary in s2e output dir
        symlink(fullProgramPath.data(), s2e()->getOutputFilename(BINARY_SYMLINK_NAME).c_str());
    }

    extern "C" __thread TCGContext *tcg_ctx;
    int cpu_gen_llvm(CPUArchState *env, TranslationBlock *tb)
    {
        TCGContext *s = tcg_ctx;

        tb->llvm_function = tcg_llvm_gen_code(tcg_llvm_translator, s, tb);
        g_sqi.tb.set_tb_function(tb->se_tb, tb->llvm_function);
        return 0;
    }

    auto Export::exportBB(S2EExecutionState *state, uint64_t pc) -> bool
    {
        auto it = m_bbCounts.find(pc);
        unsigned npassed = it == m_bbCounts.end() ? 0 : it->second;
        S2ETranslationBlock *se_tb = nullptr;

        // only export a block twice (the second time, check for differences and
        // use the second version)
        if (npassed == 0) {
            s2e()->getDebugStream(state) << "[ExportELF] Export block " << hexval(pc) << ".\n";
            se_tb = forceCodeGen(state);

            // regenerating BBS breaks symbex, so don't regen and assume the
            // generated block is correct (should be since it is evaluated)
            m_bbCounts[pc] = m_regenerateBlocks ? 1 : 2;
            m_bbFinalized[pc] = false;
            //} else if (npassed == 1 && m_regenerateBlocks) {
        } else if (m_regenerateBlocks && !m_bbFinalized[pc]) {
            s2e()->getDebugStream(state) << "[ExportELF] Regen block " << hexval(pc) << ".\n";
            se_tb = regenCode(state, getBB(pc));
            m_bbCounts[pc] += 1;
        }

        if (!se_tb) {
            s2e()->getDebugStream(state) << "[ExportELF] nullptr f " << hexval(pc) << ".\n";
            return false;
        }

        //
        // clone LLVM funcion from translation block
        //
        // We store a S2ETranslationBlockPtr here, which is a smart pointer with a reference
        // count (boost:intrusive_ptr<S2ETranslationBlock>). S2E does periodic garbage collection
        // which can removed unused or unreferenced disassembled functions. By using a smart
        // pointer here, we assure that S2E won't garbage collect the functions we've traced.
        //
        // This changed was due to an issue found in binrec,
        // https://github.com/trailofbits/binrec-prerelease/issues/201, and then confirmed by
        // the S2E maintainers, https://github.com/S2E/s2e-env/issues/462.
        //
        m_tbs.emplace(pc, se_tb);

        Function *f = se_tb->translationBlock;
        if (!m_module)
            m_module = f->getParent();
        else
            assert(m_module == f->getParent() && "LLVM basic blocks saved to different modules");

        if (m_exportInterval && ++m_exportCounter % m_exportInterval == 0) {
            s2e()->getDebugStream() << "[Export] triggered exportBB and saveLLVMModule\n";
            saveLLVMModule(true, state->getID());
        }

        return true;
    }

    void Export::saveLLVMModule(bool intermediate)
    {
        saveLLVMModule(intermediate, -1);
    }

    void Export::saveLLVMModule(bool intermediate, int stateNum)
    {
        s2e()->getDebugStream() << "[Export] Saving LLVM module...\n";

        if (!m_module) {
            s2e()->getWarningsStream() << "[Export] Error: module is uninitialized, cannot save.";
            return;
        }

        string dir = s2e()->getOutputFilename("/");
        string baseName = "captured";

        if (stateNum >= 0) {
            baseName += "_" + std::to_string(stateNum);
        }

        std::error_code error;

        raw_fd_ostream bitcodeOstream(
            (dir + baseName + ".bc").c_str(),
            error,
            sys::fs::CreationDisposition::CD_CreateAlways);
        WriteBitcodeToFile(*m_module, bitcodeOstream);
        bitcodeOstream.close();

#if WRITE_LLVM_SRC
        if (!intermediate) {
            raw_fd_ostream llvmOstream(
                (dir + baseName + ".ll").c_str(),
                error,
                sys::fs::CreationDisposition::CD_CreateAlways);
            llvmOstream << *m_module;
            llvmOstream.close();
        }
#endif
    }

    auto Export::getFirstStoredPc(Function *f) -> uint64_t
    {

        assert(!f->empty() && "Function is empty (getFirstStoredPc)");

        // NOTE (hbrodin): the model for generated functions seems to have changed since
        // this code was written. Rely on function name instead. Format from:
        // std::string TCGLLVMTranslator::generateName()
        assert(f->hasName() && "Function have no name (getFirstStoredPc)");
        auto fname = f->getName();
        assert(
            fname.startswith("tcg-llvm-") &&
            "Function does not start with tcg-llvm- (getFirstStoredPc");
        auto [first, pcstr] = fname.rsplit('-');
        uint64_t pc;
        auto fail = pcstr.getAsInteger(16, pc);
        assert(!fail && "Failed to convert pc to integer (getFirstStoredPc)");
        return pc;
    }

    auto Export::forceCodeGen(S2EExecutionState *state) -> S2ETranslationBlock *
    {
        TranslationBlock *tb = state->getTb();
        if (tb->llvm_function == NULL) {
            cpu_gen_llvm(env, (struct TranslationBlock *)tb);
            // tcg_llvm_gen_code(tcg_llvm_ctx, &tcg_ctx, (struct TranslationBlock*)tb);
            assert(tb->llvm_function && "no llvm translation block found");

            // check to make sure that the generated code is that emulates the
            // correct PC
            uint64_t pcExpected = state->regs()->getPc();
            uint64_t pcFound = getFirstStoredPc(static_cast<llvm::Function *>(tb->llvm_function));

            if (pcFound != pcExpected) {
                s2e()->getWarningsStream() << "LLVM block for pc " << hexval(pcExpected)
                                           << " stores pc " << hexval(pcFound) << ":\n";

                static_cast<Function *>(tb->llvm_function)->print(s2e()->getWarningsStream());
                assert(false);
            }

            // FIXME: enable this?
            // clean up again to avoid a crash in S2E's cleanup
            // clearLLVMFunction(tb);
        }

        S2ETranslationBlock *se_tb = static_cast<S2ETranslationBlock *>(tb->se_tb);
        assert(
            se_tb && se_tb->translationBlock == tb->llvm_function &&
            "Inconsistent state: S2E translation block does not reference disassembled function");

        return se_tb;
    }

    namespace {
        auto skipAllocas(BasicBlock::iterator it) -> BasicBlock::iterator
        {
            while (isa<AllocaInst>(it))
                it++;
            return it;
        }
    } // namespace

    auto Export::isFuncCallAndValid(Instruction *i) -> bool
    {
        if (auto *call = dyn_cast<CallInst>(i)) {
            Function *f = call->getCalledFunction();
            if (f && f->hasName() && f->getName() == "helper_raise_exception") {
                return false;
            }
        }
        return true;
    }

    void Export::evaluateFunctions(
        Function *newFunc,
        Function *oldFunc /*old Func*/,
        bool *aIsValid,
        bool *bIsValid)
    {
        int bbCounter = 0;
        for (Function::iterator blocka = newFunc->begin(), blockb = oldFunc->begin();
             blocka != newFunc->end();
             blocka++, blockb++)
        {
            bbCounter++;
            s2e()->getDebugStream()
                << "-------------BasicBlock " << bbCounter << " --------------\n";
            s2e()->getDebugStream() << "Func new:: bb size: " << blocka->size() << "\n";
            s2e()->getDebugStream() << "Func old:: bb size: " << blockb->size() << "\n";
            s2e()->getDebugStream() << "-------------->\n";
            // s2e()->getDebugStream().flush();
            for (BasicBlock::iterator insta = skipAllocas(blocka->begin()),
                                      instb = skipAllocas(blockb->begin());
                 insta != blocka->end() /*&& instb != blockb->end()*/;
                 insta++, instb++)
            {
                if (!isFuncCallAndValid(&*insta)) {
                    *aIsValid = false;
                    s2e()->getDebugStream()
                        << "Func new:: bb " << bbCounter << " has exception call\n";
                    s2e()->getDebugStream().flush();
                }
                if (!isFuncCallAndValid(&*instb)) {
                    *bIsValid = false;
                    s2e()->getDebugStream()
                        << "Func old:: bb " << bbCounter << " has exception call\n";
                    s2e()->getDebugStream().flush();
                }
            }

            //   for (BasicBlock::iterator insta = skipAllocas(blocka->begin());
            //           insta != blocka->end(); insta++) {
            /* if (!isFuncCallAndValid(&*insta)) {
                 *aIsValid = false;
                 s2e()->getDebugStream() << "Func new:: bb " << bbCounter << " has exception
             call\n"; s2e()->getDebugStream().flush();
             }*/
            //   }
            //   for (BasicBlock::iterator instb = skipAllocas(blockb->begin());
            //           instb != blockb->end(); instb++) {
            /*if (!isFuncCallAndValid(&*instb)) {
                *bIsValid = false;
                s2e()->getDebugStream() << "Func old:: bb " << bbCounter << " has exception call\n";
                s2e()->getDebugStream().flush();
            }*/
            //    }
        }
    }

    namespace {
        auto skipInst(BasicBlock::iterator it, int size) -> BasicBlock::iterator
        {
            if (size < 0)
                return it;
            while (size) {
                it++;
                --size;
            }
            return it;
        }
        auto skipBB(Function::iterator it, size_t size) -> Function::iterator
        {
            if (size < 0)
                return it;
            while (size) {
                it++;
                --size;
            }
            return it;
        }
    } // namespace

    auto Export::areBBsEqual(Function *a, Function *b /*old Func*/, bool *aIsValid, bool *bIsValid)
        -> bool
    {
        /*int aSumInst = 0;
        int bSumInst = 0;

        for (Function::iterator blocka = a->begin(), blockb = b->begin();
                blocka != a->end() && blockb != b->end(); blocka++, blockb++) {
            BasicBlock::iterator insta = skipAllocas(blocka->begin());
            BasicBlock::iterator instb = skipAllocas(blockb->begin());
            aSumInst += distance(insta, blocka->end());
            bSumInst += distance(instb, blockb->end());
            for ( ; insta != blocka->end() && instb != blockb->end(); insta++, instb++) {
                if (!isFuncCallAndValid(&*insta)) {
                    *aIsValid = false;
                    s2e()->getDebugStream() << "Func new:: bb has exception call\n";
                }
                if (!isFuncCallAndValid(&*instb)) {
                    *bIsValid = false;
                    s2e()->getDebugStream() << "Func old:: bb has exception call\n";
                }

            }
        }*/

        int aSumInst = 0;
        int bSumInst = 0;
        for (Function::iterator blocka = skipBB(a->begin(), a->size() - 1); blocka != a->end();
             blocka++) {
            // BasicBlock::iterator insta = blocka->begin();
            BasicBlock::iterator insta = skipAllocas(blocka->begin());
            int dist = distance(insta, blocka->end());
            aSumInst += dist;
            // exception call is always the second inst from at the end of basic block
            for (insta = skipInst(insta, dist - 3); insta != blocka->end(); insta++) {
                // s2e()->getDebugStream() << "A: " << *insta;
                if (!isFuncCallAndValid(&*insta)) {
                    *aIsValid = false;
                    s2e()->getDebugStream() << "Func new has exception call\n";
                }
            }
        }
        for (Function::iterator blockb = skipBB(b->begin(), b->size() - 1); blockb != b->end();
             blockb++) {
            // BasicBlock::iterator instb = blockb->begin();
            BasicBlock::iterator instb = skipAllocas(blockb->begin());
            int dist = distance(instb, blockb->end());
            bSumInst += dist;
            // s2e()->getInfoStream() << "aSumInst:" << aSumInst << " bSumInst:" << bSumInst <<
            // "\n";

            // exception call is always the second inst from at the end of basic block
            for (instb = skipInst(instb, dist - 3); instb != blockb->end(); instb++) {
                // s2e()->getDebugStream() << "B: " << *instb;
                if (!isFuncCallAndValid(&*instb)) {
                    *bIsValid = false;
                    s2e()->getDebugStream() << "Func old has exception call\n";
                }
            }
        }

        int diff = aSumInst - bSumInst;
        if (*aIsValid && *bIsValid && diff != 0) {
            s2e()->getDebugStream() << "Exceptional Case: Investigate\n";
            return false;
        }
        for (Function::iterator blockb = skipBB(b->begin(), b->size() - 1); blockb != b->end();
             blockb++) {
            // BasicBlock::iterator instb = blockb->begin();
            BasicBlock::iterator instb = skipAllocas(blockb->begin());
            int dist = distance(instb, blockb->end());
            bSumInst += dist;
            // s2e()->getInfoStream() << "aSumInst:" << aSumInst << " bSumInst:" << bSumInst <<
            // "\n";

            // exception call is always the second inst from at the end of basic block
            for (instb = skipInst(instb, dist - 3); instb != blockb->end(); instb++) {
                // s2e()->getDebugStream() << "B: " << *instb;
                if (!isFuncCallAndValid(&*instb)) {
                    *bIsValid = false;
                    s2e()->getDebugStream() << "Func old has exception call\n";
                }
            }
        }

        return true;
    }

    void Export::clearLLVMFunction(TranslationBlock *tb)
    {
        tb->llvm_function = nullptr;
    }

    auto Export::regenCode(S2EExecutionState *state, Function *old) -> S2ETranslationBlock *
    {
        TranslationBlock *tb = state->getTb();

        if (tb->llvm_function)
            clearLLVMFunction(tb);
        cpu_gen_llvm(env, (struct TranslationBlock *)tb);

        Function *newF = static_cast<Function *>(tb->llvm_function);

        bool oldIsValid = true;
        bool newIsValid = true;
        bool equal = areBBsEqual(newF, old, &newIsValid, &oldIsValid);

        if (oldIsValid && newIsValid && equal) {
            s2e()->getDebugStream() << "Finalized Function: "
                                    << "PC= " << hexval(state->regs()->getPc())
                                    << " #ofBB= " << m_bbCounts[state->regs()->getPc()] << "\n";
            m_bbFinalized[state->regs()->getPc()] = true;
            clearLLVMFunction(tb);
            return nullptr;
        }
        if (oldIsValid && !newIsValid) {
            s2e()->getDebugStream() << "Old function is valid but not the new one\n";
            s2e()->getDebugStream() << "function size :: "
                                    << "new: " << newF->size() << " vs "
                                    << "old: " << old->size() << "\n";
            s2e()->getDebugStream() << *newF << *old;
            clearLLVMFunction(tb);
            return nullptr;
        }
        if (!oldIsValid && newIsValid) {
            s2e()->getDebugStream() << "Old function is not valid but the new one is valid\n";
        } else if (!oldIsValid && !newIsValid) {
            s2e()->getDebugStream() << "Both functions are not valid\n";
        }

        s2e()->getDebugStream() << "function size :: "
                                << "new: " << newF->size() << " vs "
                                << "old: " << old->size() << "\n";

        s2e()->getInfoStream() << "[Export] regenerated BB for pc "
                               << hexval(state->regs()->getPc()) << "\n";

        s2e()->getDebugStream() << *newF << *old;

        old->eraseFromParent();

        // FIXME: enable this?
        // clearLLVMFunction(tb);

        S2ETranslationBlock *se_tb = static_cast<S2ETranslationBlock *>(tb->se_tb);
        assert(
            se_tb && se_tb->translationBlock == tb->llvm_function &&
            "Inconsistent state: S2E translation block does not reference disassembled function");

        return se_tb;
    }

    auto Export::getBB(uint64_t pc) -> Function *
    {
        auto iter = m_tbs.find(pc);
        return iter != m_tbs.end() ? iter->second->translationBlock : nullptr;
    }

    auto Export::addSuccessor(uint64_t predPc, uint64_t pc) -> bool
    {
        if (!predPc || !getBB(pc))
            return false;

        Successor successor;
        successor.pc = predPc;
        successor.successor = pc;
        ti->successors.insert(successor);
        return true;
    }

    auto Export::getMetadataInst(uint64_t pc) -> Instruction *
    {
        auto func = getBB(pc);
        return func ? func->getEntryBlock().getTerminator() : nullptr;
    }

    void Export::stopRegeneratingBlocks()
    {
        m_regenerateBlocks = false;
        s2e()->getInfoStream() << "stopped regenerating exported blocks\n";
    }

} // namespace s2e::plugins
