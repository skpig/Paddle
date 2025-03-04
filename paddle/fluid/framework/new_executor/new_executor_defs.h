// Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "paddle/fluid/framework/operator.h"
#include "paddle/fluid/framework/rw_lock.h"
#include "paddle/fluid/framework/variable_helper.h"
#include "paddle/fluid/platform/device_event_base.h"
#include "paddle/fluid/platform/event.h"

// When in inference scenario, the scopes will not be written by two threads in
// a mean time, but a scope may be read by multiple threads concurrently, and
// the mutex will cause serious performance issue.
// So the mutex is disabled when `ON_INFER`.
#ifdef PADDLE_ON_INFERENCE
#define SCOPE_VARS_READER_LOCK
#define SCOPE_VARS_WRITER_LOCK
#else
#define SCOPE_VARS_READER_LOCK AutoRDLock auto_lock(&vars_lock_);
#define SCOPE_VARS_WRITER_LOCK AutoWRLock auto_lock(&vars_lock_);
#endif

namespace paddle {
namespace framework {

using OpKernelComputeFunc = std::function<void(const ExecutionContext&)>;
using OpKernelMap =
    std::unordered_map<OpKernelType, OpKernelComputeFunc, OpKernelType::Hash>;

class InterpretercoreInferShapeContext : public InferShapeContext {
 public:
  InterpretercoreInferShapeContext(const OperatorBase& op,
                                   const RuntimeContext& ctx);

  bool HasInput(const std::string& name) const override;

  bool HasOutput(const std::string& name) const override;

  bool HasInputs(const std::string& name) const override;

  bool HasOutputs(const std::string& name) const override;

  AttrReader Attrs() const override;

  std::vector<std::string> Inputs(const std::string& name) const override;

  std::vector<std::string> Outputs(const std::string& name) const override;

  std::string GetInputNameByIdx(size_t idx) const override;

  std::string GetOutputNameByIdx(size_t idx) const override;

  void ShareDim(const std::string& in, const std::string& out, size_t i = 0,
                size_t j = 0) override;

  void ShareAllLoD(const std::string& in,
                   const std::string& out) const override;

  void ShareLoD(const std::string& in, const std::string& out, size_t i = 0,
                size_t j = 0) const override;

  int32_t GetLoDLevel(const std::string& in, size_t i = 0) const override;

  void SetLoDLevel(const std::string& out, int32_t lod_level,
                   size_t j = 0) const override;

  bool IsRuntime() const override;

  // TODO(paddle-dev): Can this be template?
  std::vector<InferShapeVarPtr> GetInputVarPtrs(
      const std::string& name) override;

  std::vector<InferShapeVarPtr> GetOutputVarPtrs(
      const std::string& name) override;

  DDim GetInputDim(const std::string& name) const override;

  std::vector<DDim> GetInputsDim(const std::string& name) const override;

  std::vector<proto::VarType::Type> GetInputsVarType(
      const std::string& name) const override;

  std::vector<proto::VarType::Type> GetOutputsVarType(
      const std::string& name) const override;

  void SetOutputDim(const std::string& name, const DDim& dim) override;

  void SetOutputsDim(const std::string& name,
                     const std::vector<DDim>& dims) override;

  void SetSkipLoD(bool skip);

 protected:
  DDim GetDim(Variable* var) const;

  std::vector<DDim> GetDims(const std::vector<Variable*>& vars) const;

  std::vector<DDim> GetRepeatedDims(const std::string& name) const override;

  void SetDim(Variable* var, const DDim& dim);

  void SetDims(const std::vector<Variable*>& vars,
               const std::vector<DDim>& dims);

  void SetRepeatedDims(const std::string& name,
                       const std::vector<DDim>& dims) override;

  std::vector<proto::VarType::Type> GetVarTypes(
      const std::vector<Variable*>& vars) const;

  proto::VarType::Type GetVarType(Variable* var) const;

 private:
  const std::vector<Variable*>& InputVars(const std::string& name) const;

  const std::vector<Variable*>& OutputVars(const std::string& name) const;

  const OperatorBase& op_;
  const RuntimeContext& ctx_;
  bool can_skip_lod_;
};

struct OpKernelFunc {
  OpKernelComputeFunc compute_func_;
};

struct VariableMetaInfo {
  int var_ref_count_{0};
  framework::VarDesc* var_desc_{nullptr};

  VariableMetaInfo() {}
  VariableMetaInfo(int var_ref_count, framework::VarDesc* var_desc)
      : var_ref_count_(var_ref_count), var_desc_(var_desc) {}
};

class VariableScope;
class VariableScopeListener : public ScopeListener {
 public:
  explicit VariableScopeListener(VariableScope* var_scope_);
  void onCreateVariable(const std::string& name) override;
  void onDeleteVariable(const std::string& name) override;
  void onRenameVariable(const std::string& old_name,
                        const std::string& new_name) override;
  void onCreateScope(Scope* Scope) override;
  void onDeleteScope(Scope* Scope) override;
  void onClear() override;

 private:
  VariableScope* var_scope_;  // not owned
};

// TODO(zhiqiu): Maybe we need to add rwlock for VariableScope?

// NOTE(xiongkun03): Use scope as a member of VariableScope, we don't need
// ScopeBase. Scope manager the variables and VariableScope is just a quick
// access machanism. ScopeListener is the callback to sync changes in Original
// Scope. We can make it a membership of VariableScope. Here we use inherent.
class VariableScope : public ScopeBase {
 public:
  explicit VariableScope(Scope* scope);

  const Scope* GetScope() const;

  Variable* FindVar(const std::string& name) const;

  ~VariableScope();

  // Get variable id by name, return -1 if not found
  int GetIdByName(const std::string& name) const;

  // Get variable name by id, return "" if not found
  std::string GetNameById(int id) const;

  bool HasVar(const std::string& name) const;

  int VarId(const std::string& name) const;

  Variable* Var(int id) const;

  Variable* Var(const std::string& name) const;

  size_t VarSize() const;

  void AddVar(const std::string& name, VarDesc* var_desc);

  void AddVar(const std::string& name, const Variable& var);

  void SetVarDesc(const std::string& name, framework::VarDesc* var_desc);

  paddle::framework::VarDesc* VarDesc(const std::string& name) const;

  paddle::framework::VarDesc* VarDesc(int id) const;

  void CheckExist(int id) const;

  void CheckExist(const std::string& name) const;

  std::vector<VariableMetaInfo>& MutableVecMetaInfo() { return vec_meta_info_; }

  const std::vector<VariableMetaInfo>& VecMetaInfo() const {
    return vec_meta_info_;
  }

  friend class VariableScopeListener;

 private:
  std::vector<Variable*> var_list_;
  std::map<std::string, int> name2id_;
  std::vector<VariableMetaInfo> vec_meta_info_;
  Scope* scope_ = nullptr;
  // mutable RWLock vars_lock_;
  std::shared_ptr<VariableScopeListener> listener_;
};

class NextInstruction {
 public:
  void AddDirectRun(size_t id) { direct_run_.push_back(id); }

  void ADDEventRun(size_t id) { event_wait_run_.push_back(id); }

  void AddSyncRun(size_t id) { synchronize_run_.push_back(id); }

  const std::vector<size_t>& DirectRunIds() const { return direct_run_; }

  const std::vector<size_t>& EventRunIds() const { return event_wait_run_; }

  const std::vector<size_t>& SyncRunIds() const { return synchronize_run_; }

 private:
  std::vector<size_t> direct_run_;
  std::vector<size_t> event_wait_run_;
  std::vector<size_t> synchronize_run_;
};

struct EventInter {
  explicit EventInter(size_t var_id,
                      std::shared_ptr<platform::DeviceEvent> event,
                      platform::DeviceType waiter_type)
      : var_id_(var_id), event_(event), waiter_type_(waiter_type) {}
  size_t var_id_;
  std::shared_ptr<platform::DeviceEvent> event_;
  platform::DeviceType waiter_type_;
};

struct InstructionInfo {
  std::vector<size_t> dependecy_count_;
};

enum class OpFuncType {
  kQueueSync = 0,   // CPU kernel, block host
  kQueueAsync = 1,  // GPU Kernel or d2h, h2d, send, recv, broadcast
};
class RuntimeInferShapeContext;

struct OpFuncNode {
  OperatorBase* operator_base_;
  std::map<std::string, std::vector<int>> input_index;
  std::map<std::string, std::vector<int>> output_index;
  std::unordered_set<int> no_data_transform_index;

  OpKernelComputeFunc kernel_func_;
  platform::DeviceContext* dev_ctx_;  // not owned
  OpFuncType type_;
};

class Instruction {
 public:
  Instruction(size_t id, const OpFuncNode& op_func_node,
              const platform::DeviceContext& dev_ctx)
      : id_(id), op_func_node_(op_func_node), dev_ctx_(dev_ctx) {
    PADDLE_ENFORCE_GE(id, 0, platform::errors::PreconditionNotMet(
                                 "Required id >= 0, but received id = %d", id));
  }

  size_t Id() const { return id_; }

  const std::map<std::string, std::vector<int>>& Inputs() const {
    return op_func_node_.input_index;
  }

  const std::map<std::string, std::vector<int>>& Outputs() const {
    return op_func_node_.output_index;
  }

  const std::unordered_set<int>& NoDataTransformVars() const {
    return op_func_node_.no_data_transform_index;
  }

  OpKernelComputeFunc KernelFunc() const { return op_func_node_.kernel_func_; }

  OpFuncType KernelType() const { return op_func_node_.type_; }

  OperatorBase* OpBase() const {
    auto* op_base = op_func_node_.operator_base_;
    PADDLE_ENFORCE_NOT_NULL(op_base, platform::errors::PreconditionNotMet(
                                         "op_base shall not be nullptr."));
    return op_base;
  }

  NextInstruction& NextInstructions() { return next_instruction_; }

  const NextInstruction& NextInstructions() const { return next_instruction_; }

  void AddGCCheckVar(size_t id) { gc_check_var_list_.push_back(id); }

  const std::vector<size_t>& GCCheckVars() const { return gc_check_var_list_; }

  void ResetContext(const VariableValueMap& in_vars,
                    const VariableValueMap& out_vars) {
    runtime_ctx_.reset(new RuntimeContext(in_vars, out_vars));
    infershape_ctx_.reset(
        new InterpretercoreInferShapeContext(*OpBase(), *runtime_ctx_.get()));
    // NOTE: Because execution_ctx_ is constructed by `scope&`, so we fake an
    // empty here to avoid illegal local reference.
    static framework::Scope scope_;
    execution_ctx_.reset(
        new ExecutionContext(*OpBase(), scope_, dev_ctx_, *runtime_ctx_.get()));
  }

  std::shared_ptr<RuntimeContext> InnerRuntimeContext() const {
    return runtime_ctx_;
  }

  std::shared_ptr<InterpretercoreInferShapeContext> InnerInferShapeContext()
      const {
    return infershape_ctx_;
  }

  std::shared_ptr<ExecutionContext> InnerExecutionContext() const {
    return execution_ctx_;
  }

  const platform::DeviceContext& DeviceContext() const { return dev_ctx_; }

  const std::vector<std::pair<Variable*, Variable*>>& InplaceInfo() const {
    return vec_inplace_in_to_out_;
  }

  void AddInplace(Variable* in, Variable* out) {
    vec_inplace_in_to_out_.emplace_back(in, out);
  }

  const std::vector<EventInter>& InputEvents() const { return intput_events_; }

  const std::vector<EventInter>& OutputEvents() const { return output_events_; }

  void AddInputEvent(size_t var_id,
                     std::shared_ptr<platform::DeviceEvent> event,
                     platform::DeviceType waiter_type) {
    intput_events_.emplace_back(var_id, event, waiter_type);
  }

  void AddOutputEvent(size_t var_id,
                      std::shared_ptr<platform::DeviceEvent> event,
                      platform::DeviceType waiter_type) {
    output_events_.emplace_back(var_id, event, waiter_type);
  }

 private:
  size_t id_;
  const OpFuncNode& op_func_node_;          // not owned
  const platform::DeviceContext& dev_ctx_;  // not owned

  std::shared_ptr<RuntimeContext> runtime_ctx_;
  std::shared_ptr<InterpretercoreInferShapeContext> infershape_ctx_;
  std::shared_ptr<ExecutionContext> execution_ctx_;

  std::vector<size_t> gc_check_var_list_;
  NextInstruction next_instruction_;

  std::vector<EventInter> intput_events_;
  std::vector<EventInter> output_events_;

  std::vector<std::pair<Variable*, Variable*>> vec_inplace_in_to_out_;
};

namespace interpreter {
static constexpr char kMemcpyH2D[] = "memcpy_h2d";
static constexpr char kMemcpyD2H[] = "memcpy_d2h";

static bool IsMemcpyH2D(const Instruction& instr) {
  return instr.OpBase()->Type() == kMemcpyH2D;
}

static bool IsMemcpyD2H(const Instruction& instr) {
  return instr.OpBase()->Type() == kMemcpyD2H;
}
}  // namespace interpreter

}  // namespace framework
}  // namespace paddle
