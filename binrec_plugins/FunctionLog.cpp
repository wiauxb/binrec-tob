#include "FunctionLog.h"
#include "ModuleSelector.h"
#include "util.h"
#include <cassert>
#include <iostream>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <s2e/ConfigFile.h>
#include <s2e/CorePlugin.h>
#include <s2e/Plugins/OSMonitors/Support/ModuleExecutionDetector.h>
#include <s2e/S2E.h>
#include <s2e/Utils.h>
#include <tcg/tcg-llvm.h>

using namespace binrec;

namespace s2e::plugins {
    S2E_DEFINE_PLUGIN(
        FunctionLog,
        "Log register values at basic block start.",
        "FunctionLog",
        S2E_NOOP("ModuleSelector", "FunctionMonitor"));

    void FunctionLog::initialize()
    {
        ti = TraceInfo::get();

        m_saveInterval = s2e()->getConfig()->getInt(getConfigKey() + ".saveInterval", 0);

        ModuleSelector *selector = (ModuleSelector *)(s2e()->getPlugin("ModuleSelector"));
        selector->onModuleLoad.connect(sigc::mem_fun(*this, &FunctionLog::slotModuleLoad));
        selector->onModuleExecute.connect(sigc::mem_fun(*this, &FunctionLog::slotModuleExecute));

        s2e()->getCorePlugin()->onStateFork.connect(
            sigc::mem_fun(*this, &FunctionLog::slotStateFork));
        s2e()->getCorePlugin()->onStateSwitch.connect(
            sigc::mem_fun(*this, &FunctionLog::slotStateSwitch));

        m_functionMonitor = s2e()->getPlugin<FunctionMonitor>();

        s2e()->getDebugStream() << "[FunctionLog] Plugin initialized. \n";
        m_callerPc = 0;
        ti->functionLog.entries.push_back(0);
    }

    FunctionLog::~FunctionLog()
    {
        if (ti->functionLog.entries.back() == 0) {
            ti->functionLog.entries.pop_back();
        }

        saveTraceInfo(-1);
    }

    void FunctionLog::saveTraceInfo(int stateNum)
    {
        s2e()->getDebugStream() << "[FunctionLog] Saving Trace Info... \n";

        std::string fileName = TraceInfo::defaultName;
        std::string suffix = ".json";

        if (stateNum >= 0) {
            fileName += "_" + std::to_string(stateNum);
        }
        s2e()->getDebugStream() << fileName + suffix + "\n";

        std::ofstream traceInfoOut(
            s2e()->getOutputFilename(fileName + suffix).c_str(),
            std::ios::out | std::ios::trunc);
        traceInfoOut << *ti;
    }

    uint64_t entrypoint;
    void FunctionLog::slotModuleLoad(S2EExecutionState *state, const ModuleDescriptor &module)
    {
        s2e()->getDebugStream(state) << "[FunctionLog] ==> ModulePid: " << module.Pid << '\n';
        m_functionMonitor->onCall.connect(sigc::mem_fun(*this, &FunctionLog::onFunctionCall));
        m_moduleEntryPoint = module.EntryPoint;
    }

    void FunctionLog::slotModuleExecute(S2EExecutionState *state, uint64_t pc)
    {
        if (!ti->functionLog.entries.back()) {
            s2e()->getDebugStream(state) << "[FunctionLog] New entry " << hexval(pc) << '\n';
            ti->functionLog.entries.back() = pc;
            // NOTE (hbrodin): Push the top-level entry onto the call stack to track it's translated
            // blocks as well
            m_callStack.push(pc);
        }

        m_modulePcs.insert(pc);

        m_executedBBPc = pc;

        if (!m_callStack.empty()) {
            ti->functionLog.entryToTbs[m_callStack.top()].insert(pc);
            if (m_callerPc) {
                ti->functionLog.callerToFollowUp.insert(std::make_pair(m_callerPc, pc));
                m_callerPc = 0;
            }
        } else {
            s2e()->getWarningsStream(state)
                << "[FunctionLog] Call stack is empty: " << hexval(pc) << "\n";
        }
        //FIXME improve condition for the save
        if (m_saveInterval && ++m_saveCounter % m_saveInterval == 0)
        {
            saveTraceInfo(state->getID());
        }
        
    }

    void FunctionLog::onFunctionCall(
        S2EExecutionState *state,
        const ModuleDescriptorConstPtr &source,
        const ModuleDescriptorConstPtr &dest,
        uint64_t callerPc,
        uint64_t calleePc,
        const FunctionMonitor::ReturnSignalPtr &returnSignal)
    {
        // TODO (hbrodin): Should there be any filtering done here? Only relevant modules.
        // Consider adding the pid from slotModuleLoad and check it here. If ELFSelector have
        // choosen only a specific binary there might not be a need to do such filtering.

        // NOTE (hbrodin): Ideally there should be a better way of filtering calls that are only
        // relevant to the module being analyzed. There probably is. Just need to find it...
        if ((source && source->EntryPoint == m_moduleEntryPoint) ||
            (dest && dest->EntryPoint == m_moduleEntryPoint))
        {
            m_callStack.push(calleePc);
            returnSignal->connect(sigc::bind(
                sigc::mem_fun(*this, &FunctionLog::onFunctionReturn),
                callerPc,
                calleePc));
        }
    }

    void FunctionLog::onFunctionReturn(
        S2EExecutionState *state,
        const ModuleDescriptorConstPtr &source,
        const ModuleDescriptorConstPtr &dest,
        uint64_t returnSite,
        uint64_t func_caller,
        uint64_t func_begin)
    {
        if (m_callStack.empty()) {
            s2e()->getWarningsStream() << "[FunctionLog] Returning from func: " << func_begin
                                       << ", but call stack is empty.\n";
        }

        uint32_t top = m_callStack.top();
        m_callStack.pop();
        if (top != func_begin) {
            // That's required because of how dynamic libraries are called. Without this, there can
            // be an extra return address on the stack. Why? And is this way of doing it correct?
            // Investigate...
            if (m_callStack.empty()) {
                s2e()->getWarningsStream()
                    << "[FunctionLog] Couldn't match caller func: " << top
                    << " with returned func: " << func_begin << " and call stack is empty now.\n";
                if (source)
                    s2e()->getWarningsStream() << "\tSource: " << source->Name << " \n";
                if (dest)
                    s2e()->getWarningsStream() << "\tDest: " << dest->Name << " \n";
                s2e()->getWarningsStream() << " returnSite: " << hexval(returnSite) << "\n";

                m_callStack.push(top);
            } else if (func_begin != m_callStack.top()) {
                s2e()->getWarningsStream() << "[FunctionLog] Couldn't match caller func: " << top
                                           << " with returned func: " << func_begin << "\n";
                if (source)
                    s2e()->getWarningsStream() << "\tSource: " << source->Name << " \n";
                if (dest)
                    s2e()->getWarningsStream() << "\tDest: " << dest->Name << " \n";
                s2e()->getWarningsStream() << " returnSite: " << hexval(returnSite) << "\n";

                m_callStack.push(top);
                return;
            } else {
                m_callStack.pop();
            }
        }

        if (func_begin == ti->functionLog.entries.back()) {
            // So you might wanna ask, why reset this? Why can we have multiple entry pcs? That is a
            // very good question to ask. To answer this, understand how init_env.so works:
            // init_env.so uses LD_PRELOAD to hook into the execution of our binary and enables
            // tracing right before calling __libc_start_main. That means, we do not trace the
            // actual entry point of the binary. There might be an elegant solution to this (new S2E
            // for example, but maybe old S2E can also be convinced to trace a specific binary
            // without init_env.so). In any case, __libc_start_main calls __libc_csu_init (which
            // calls global constructors) and then main. We want to be aware of both calls and in
            // which order they appear (so we can essentially generate our own version
            // __libc_start_main). So when we return from the first entry pc, reset this so we can
            // record the next "entry", too.
            ti->functionLog.entries.push_back(0);
            s2e()->getDebugStream(state) << "[FunctionLog] Return from entry " << hexval(func_begin)
                                         << " at " << hexval(state->regs()->getPc()) << '\n';
        }

        m_callerPc = func_caller;
        std::pair<uint32_t, uint32_t> entryToCaller(func_begin, func_caller);
        std::pair<uint32_t, uint32_t> entryToReturn(func_begin, m_executedBBPc);
        ti->functionLog.entryToCaller.insert(entryToCaller);
        ti->functionLog.entryToReturn.insert(entryToReturn);
    }

    void FunctionLog::slotStateFork(
        S2EExecutionState *state,
        const std::vector<S2EExecutionState *> &newStates,
        const std::vector<klee::ref<klee::Expr>> &newCondition)
    {
        // Store a copy the current private vars for each new state for eventual state switch
        for (auto newState : newStates) {
            int newStateID = newState->getID();

            s2e()->getDebugStream()
                << "[FunctionLog] Storing copy of tracing vars for state: " << newState->getID()
                << "\n";
            m_tracesByState.emplace(std::make_pair(newStateID, ti->getCopy()));

            std::stack<uint32_t> stackCopy(m_callStack);
            m_stacksByState.emplace(std::make_pair(newStateID, stackCopy));

            m_execPcByState.emplace(std::make_pair(newStateID, m_executedBBPc));
            m_callerPcByState.emplace(std::make_pair(newStateID, m_callerPc));
        }
    }

    void FunctionLog::slotStateSwitch(S2EExecutionState *state, S2EExecutionState *newState)
    {
        s2e()->getDebugStream() << "[FunctionLog] StateSwitch !\n";
        int curStateID = state->getID();
        int newStateID = newState->getID();

        saveTraceInfo(curStateID);

        // Restore the private vars we have from the fork point
        s2e()->getDebugStream() << "[FunctionLog] Restoring tracing vars for state: " << newStateID
                                << "\n";
        assert(
            m_tracesByState.find(newStateID) != m_tracesByState.end() &&
            " Could not restore traceinfo state!");

        TraceInfo *copyTi = m_tracesByState.at(newStateID);
        ti->restoreFromCopy(copyTi);
        m_callStack = m_stacksByState.at(newStateID);
        m_executedBBPc = m_execPcByState.at(newStateID);
        m_callerPc = m_callerPcByState.at(newStateID);

        // Delete the copies we just restored
        m_tracesByState.erase(newStateID);
        delete copyTi;
        m_stacksByState.erase(newStateID);
        m_execPcByState.erase(newStateID);
        m_callerPcByState.erase(newStateID);

        // Also check if we need to remove the previous state's copies
        if (m_tracesByState.find(curStateID) != m_tracesByState.end()) {
            TraceInfo *prevTi = m_tracesByState.at(curStateID);
            delete prevTi;
        }
        m_tracesByState.erase(curStateID);
        m_stacksByState.erase(curStateID);
        m_execPcByState.erase(curStateID);
        m_callerPcByState.erase(curStateID);
    }

} // namespace s2e::plugins
