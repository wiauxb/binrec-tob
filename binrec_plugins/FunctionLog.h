#ifndef __PLUGIN_FUNCTIONLOG_H__
#define __PLUGIN_FUNCTIONLOG_H__

#include "binrec/tracing/trace_info.hpp"
#include <fstream>
#include <map>
#include <s2e/ConfigFile.h>
#include <s2e/CorePlugin.h>
#include <s2e/Plugin.h>
#include <s2e/Plugins/ExecutionMonitors/FunctionMonitor.h>
#include <s2e/Plugins/OSMonitors/ModuleDescriptor.h>
#include <s2e/Plugins/OSMonitors/Support/ModuleExecutionDetector.h>
#include <s2e/S2EExecutionState.h>
#include <set>
#include <stack>
#include <utility>
#include <vector>

namespace s2e::plugins {
    class FunctionLog : public Plugin {
        S2E_PLUGIN
    public:
        FunctionLog(S2E *s2e) : Plugin(s2e) {}

        void initialize();
        ~FunctionLog();

    protected:
        void saveTraceInfo(int stateNum);
        unsigned m_saveInterval;

    private:
        void slotModuleLoad(S2EExecutionState *state, const ModuleDescriptor &module);
        void slotModuleExecute(S2EExecutionState *state, uint64_t pc);

        void onFunctionCall(
            S2EExecutionState *state,
            const ModuleDescriptorConstPtr &source,
            const ModuleDescriptorConstPtr &dest,
            uint64_t callerPc,
            uint64_t calleePc,
            const FunctionMonitor::ReturnSignalPtr &returnSignal);
        void onFunctionReturn(
            S2EExecutionState *state,
            const ModuleDescriptorConstPtr &source,
            const ModuleDescriptorConstPtr &dest,
            uint64_t returnSite,
            uint64_t func_caller,
            uint64_t func_begin);

        void slotStateFork(
            S2EExecutionState *state,
            const std::vector<S2EExecutionState *> &newStates,
            const std::vector<klee::ref<klee::Expr>> &newCondition);
        void slotStateSwitch(S2EExecutionState *state, S2EExecutionState *newState);

    private:
        FunctionMonitor *m_functionMonitor;
        std::shared_ptr<binrec::TraceInfo> ti;
        uint32_t m_executedBBPc;
        uint32_t m_callerPc;
        uint64_t m_moduleEntryPoint;
        std::set<uint32_t> m_modulePcs;
        std::stack<uint32_t> m_callStack;

        unsigned m_saveCounter;

        std::map<int, binrec::TraceInfo *> m_tracesByState;
        std::map<int, uint32_t> m_execPcByState;
        std::map<int, uint32_t> m_callerPcByState;
        std::map<int, std::stack<uint32_t>> m_stacksByState;
    };

} // namespace s2e::plugins

#endif
