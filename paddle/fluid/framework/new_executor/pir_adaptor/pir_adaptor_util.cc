// Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
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

#include "paddle/fluid/framework/new_executor/pir_adaptor/pir_adaptor_util.h"

#include "paddle/fluid/framework/op_info.h"
#include "paddle/fluid/framework/operator.h"
#include "paddle/fluid/framework/scope.h"
#include "paddle/fluid/framework/string_array.h"
#include "paddle/fluid/framework/tensor.h"
#include "paddle/fluid/framework/tensor_ref_array.h"
#include "paddle/fluid/framework/variable.h"
#include "paddle/fluid/framework/variable_helper.h"
#include "paddle/fluid/ir_adaptor/translator/op_compat_info.h"
#include "paddle/fluid/pir/dialect/kernel/ir/kernel_attribute.h"
#include "paddle/fluid/pir/dialect/kernel/ir/kernel_type.h"
#include "paddle/fluid/pir/dialect/operator/interface/op_yaml_info.h"
#include "paddle/fluid/pir/dialect/operator/ir/control_flow_op.h"
#include "paddle/fluid/pir/dialect/operator/ir/manual_op.h"
#include "paddle/fluid/pir/dialect/operator/ir/op_attribute.h"
#include "paddle/fluid/pir/dialect/operator/ir/op_dialect.h"
#include "paddle/fluid/pir/dialect/operator/ir/op_type.h"
#include "paddle/fluid/pir/dialect/operator/utils/op_yaml_info_parser.h"
#include "paddle/fluid/pir/dialect/operator/utils/utils.h"
#include "paddle/phi/core/enforce.h"
#include "paddle/phi/core/kernel_context.h"
#include "paddle/phi/core/meta_tensor.h"
#include "paddle/pir/core/builtin_attribute.h"
#include "paddle/pir/core/builtin_op.h"
#include "paddle/pir/core/ir_context.h"
#include "paddle/pir/core/program.h"
#include "paddle/pir/core/utils.h"

#include "glog/logging.h"

namespace paddle {
namespace framework {
std::shared_ptr<ValueExecutionInfo> ValueExecutionInfo::NewChild(Scope* scope) {
  std::shared_ptr<ValueExecutionInfo> info =
      std::make_shared<ValueExecutionInfo>(scope);
  info->parent_ = this;
  info->value_2_var_name_ = this->value_2_var_name_;
  info->var_2_var_name_ = this->var_2_var_name_;
  info->var_name_2_id_ = this->var_name_2_id_;
  info->id_2_var_name_ = this->id_2_var_name_;
  info->var_list_ = this->var_list_;
  return info;
}

void ValueExecutionInfo::Add(::pir::Value value, std::string var_name) {
  auto* var = scope_->FindVar(var_name);
  PADDLE_ENFORCE_NOT_NULL(
      var, platform::errors::NotFound("Cannot find %s in scope.", var_name));

  if (value_2_var_name_.count(value) == 0) {
    value_2_var_name_.emplace(value, var_name);
  }

  var_2_var_name_.emplace(var, var_name);

  if (var_name_2_id_.count(var_name) == 0) {
    auto id = var_name_2_id_.size();
    var_name_2_id_.emplace(var_name, id);
    id_2_var_name_.emplace(id, var_name);
    var_list_.push_back(var);
  }

  PADDLE_ENFORCE_EQ(
      var_list_.size(),
      var_name_2_id_.size(),
      paddle::platform::errors::InvalidArgument(
          "The size of variable_list and var_name_2_id map should be equal"));
}

void ValueExecutionInfo::Rename(pir::Value value,
                                std::string new_name,
                                std::string orig_name) {
  value_2_var_name_[value] = new_name;

  for (auto kv : value_2_var_name_) {
    if (kv.second == orig_name) {
      value_2_var_name_[kv.first] = new_name;
    }
  }

  for (auto kv : var_2_var_name_) {
    if (kv.second == orig_name) {
      var_2_var_name_[kv.first] = new_name;
    }
  }

  for (auto kv : var_name_2_id_) {
    if (kv.first == orig_name) {
      var_name_2_id_.emplace(new_name, kv.second);
      id_2_var_name_[kv.second] = new_name;
    }
  }
  var_name_2_id_.erase(orig_name);
}

int ValueExecutionInfo::GetIdByName(const std::string& name) const {
  auto it = var_name_2_id_.find(name);
  if (it != var_name_2_id_.end()) {
    return it->second;
  }
  return -1;
}

std::string ValueExecutionInfo::GetNameById(int id) const {
  // NOTE(zhiqiu): do not use vec_meta_info_[id].vardesc_->Name() since
  // vec_meta_info_[id] may be nullptr,
  // typically when the target variable is not existed in the original program
  // desc, but created by interpretercore.
  // For example, created and used by d2h_copy or h2d_copy operator.
  auto it = id_2_var_name_.find(id);
  if (it != id_2_var_name_.end()) {
    return it->second;
  }
  return "";
}

const std::unordered_map<::pir::Value, std::string>&
ValueExecutionInfo::GetValue2VarName() const {
  return value_2_var_name_;
}

void ValueExecutionInfo::AddValue2VarName(::pir::Value value,
                                          const std::string& var_name) {
  value_2_var_name_.emplace(value, var_name);
}

const std::unordered_map<const Variable*, std::string>&
ValueExecutionInfo::GetVar2VarName() const {
  return var_2_var_name_;
}

const std::map<std::string, int>& ValueExecutionInfo::GetVarName2Id() const {
  return var_name_2_id_;
}

const std::unordered_map<int, std::string>& ValueExecutionInfo::GetId2VarName()
    const {
  return id_2_var_name_;
}

const std::vector<Variable*>& ValueExecutionInfo::GetVarList() const {
  return var_list_;
}

void ValueExecutionInfo::ResetVarList(int id, Variable* var) {
  var_list_[id] = var;
}

bool ValueExecutionInfo::HasVar(const std::string& var_name) const {
  auto it = var_name_2_id_.find(var_name);
  if (it != var_name_2_id_.end()) {
    return true;
  }
  return false;
}

bool ValueExecutionInfo::HasValue(::pir::Value value) const {
  auto it = value_2_var_name_.find(value);
  if (it != value_2_var_name_.end()) {
    return true;
  }
  return false;
}

std::string ValueExecutionInfo::GetVarName(::pir::Value value) const {
  auto it = value_2_var_name_.find(value);
  if (it != value_2_var_name_.end()) {
    return it->second;
  }
  return "";
}

std::string ValueExecutionInfo::GetVarName(const Variable* var) const {
  auto it = var_2_var_name_.find(var);
  if (it != var_2_var_name_.end()) {
    return it->second;
  }
  return "";
}

int ValueExecutionInfo::GetVarId(::pir::Value value) const {
  auto var_name = GetVarName(value);
  auto it = var_name_2_id_.find(var_name);
  if (it != var_name_2_id_.end()) {
    return it->second;
  }
  return -1;
}

int ValueExecutionInfo::GetVarId(const Variable* var) const {
  auto var_name = GetVarName(var);
  auto it = var_name_2_id_.find(var_name);
  if (it != var_name_2_id_.end()) {
    return it->second;
  }
  return -1;
}

const std::unordered_set<std::string> SpecialOps = {"pd_op.feed",
                                                    "pd_op.fetch",
                                                    "builtin.combine",
                                                    "builtin.set_parameter",
                                                    "builtin.get_parameter",
                                                    "builtin.slice",
                                                    "builtin.split",
                                                    "pd_op.data",
                                                    "builtin.shadow_output",
                                                    "pd_op.if",
                                                    "pd_op.while"};

Variable* CreateVar(pir::Value value,
                    const std::string& var_name_prefix,
                    bool force_persisable,
                    ValueExecutionInfo* value_exe_info) {
  pir::Operation* def_op = value.dyn_cast<pir::OpResult>().owner();
  bool is_persisable = false;
  if (def_op->isa<::pir::GetParameterOp>()) {
    is_persisable = true;
  } else if (def_op->HasAttribute(kAttrIsPersisable)) {
    is_persisable = def_op->attribute(kAttrIsPersisable)
                        .dyn_cast<pir::ArrayAttribute>()
                        .AsVector()[value.dyn_cast<pir::OpResult>().index()]
                        .dyn_cast<pir::BoolAttribute>()
                        .data();
  }

  Variable* var = nullptr;
  std::string name = var_name_prefix + "_inner_var_" +
                     std::to_string(value_exe_info->GetVar2VarName().size());

  if (force_persisable || is_persisable) {
    VLOG(6) << "Create var: " << name << " in scope "
            << value_exe_info->GetScope()->root();
    var = const_cast<Scope*>(value_exe_info->GetScope()->root())->Var(name);
  } else {
    VLOG(6) << "Create var: " << name << " in scope "
            << value_exe_info->GetScope();
    var = value_exe_info->GetScope()->Var(name);
  }

  value_exe_info->Add(value, name);

  return var;
}

void CheckInputVars(pir::Operation* op,
                    const std::string& op_name,
                    ValueExecutionInfo* execution_info) {
  size_t input_num = op->num_operands();
  if (input_num > 0) {
    for (size_t i = 0; i < input_num; ++i) {
      auto value = op->operand_source(i);
      if (IsInvalid(value)) {
        PADDLE_ENFORCE_EQ(
            execution_info->HasValue(value),
            true,
            phi::errors::PreconditionNotMet(
                "input should in execution_info, [%d] 'th input of [%s] op",
                i,
                op_name));
      }
    }
  }
}

void BuildValue(pir::Value value,
                const std::string& var_name_prefix,
                ValueExecutionInfo* value_exe_info) {
  if (!IsInvalid(value)) {
    VLOG(8) << "Value is not invalid, so skip build a variable.";
    return;
  }

  Variable* var = nullptr;
  auto& value_2_var_name = value_exe_info->GetValue2VarName();
  if (value_2_var_name.find(value) != value_2_var_name.end()) {
    var = value_exe_info->GetScope()->FindVar(value_2_var_name.at(value));
  } else {
    var = CreateVar(value, var_name_prefix, false, value_exe_info);
  }
  // Only support DenseTensor or Vector<DenseTensor>
  if (!value.type() ||
      value.type().isa<paddle::dialect::AllocatedDenseTensorType>()) {
    var->GetMutable<phi::DenseTensor>();
  } else if (value.type().isa<paddle::dialect::AllocatedSelectedRowsType>()) {
    var->GetMutable<phi::SelectedRows>();
  } else if (value.type().isa<pir::VectorType>()) {
    auto tensor_array = var->GetMutable<VariableRefArray>();
    for (size_t i = 0; i < value.type().dyn_cast<pir::VectorType>().size();
         i++) {
      PADDLE_ENFORCE(value.type()
                         .dyn_cast<pir::VectorType>()[i]
                         .isa<paddle::dialect::AllocatedDenseTensorType>(),
                     paddle::platform::errors::Fatal(
                         "Element of VectorType output only support "
                         "DenseTensorType"));
      auto var_i = CreateVar(value, var_name_prefix, false, value_exe_info);

      var_i->GetMutable<phi::DenseTensor>();
      tensor_array->emplace_back(var_i);
    }
  } else {
    PADDLE_THROW(
        phi::errors::PreconditionNotMet("Output only support DenseTensorType "
                                        "or SelectedRowsType or VectorType"));
  }
}

void HandleForSpecialOp(pir::Operation* op,
                        const std::string& var_name_prefix,
                        ValueExecutionInfo* value_exe_info) {
  std::string op_name = op->name();
  if (op->attributes().count("op_name")) {
    op_name =
        op->attributes().at("op_name").dyn_cast<pir::StrAttribute>().AsString();
  }

  if (op_name == "pd_op.fetch") {
    // fetch is a very special op, with no output
    auto fetch_src_name =
        op->attributes().at("name").dyn_cast<pir::StrAttribute>().AsString();

    auto fetch_var_name = fetch_src_name + "@fetch";
    auto* var = const_cast<Scope*>(value_exe_info->GetScope()->root())
                    ->Var(fetch_var_name);
    var->GetMutable<phi::DenseTensor>();
    auto value = op->result(0);

    value_exe_info->Add(value, fetch_var_name);
  }

  if (op_name == "pd_op.feed" || op_name == "pd_op.data") {
    VLOG(6) << "Handle for" << op_name;
    auto value = op->result(0);
    VLOG(6) << "link feed output to feed in variable"
            << value_exe_info->GetScope();

    std::string name =
        op->attributes().at("name").dyn_cast<pir::StrAttribute>().AsString();
    Variable* var = value_exe_info->GetScope()->Var(name);
    PADDLE_ENFORCE(var,
                   paddle::platform::errors::InvalidArgument(
                       "The variable %s shoud exist", name));

    value_exe_info->Add(value, name);
  }

  if (op_name == "builtin.combine") {
    auto out_value = op->result(0);

    Variable* var = nullptr;
    auto& value_2_var_name = value_exe_info->GetValue2VarName();
    if (value_2_var_name.find(out_value) != value_2_var_name.end()) {
      var = value_exe_info->GetScope()->FindVar(value_2_var_name.at(out_value));
    } else {
      var = CreateVar(out_value, var_name_prefix, false, value_exe_info);
    }

    auto tensor_array = var->GetMutable<VariableRefArray>();
    // clear tensor array
    tensor_array->clear();
    size_t input_num = op->num_operands();
    for (size_t i = 0; i < input_num; ++i) {
      auto value = op->operand_source(i);
      PADDLE_ENFORCE_EQ(
          value_2_var_name.count(value),
          true,
          phi::errors::PreconditionNotMet("can not found input of combine op"));
      tensor_array->emplace_back(
          value_exe_info->GetScope()->FindVar(value_2_var_name.at(value)));
    }
  }

  if (op_name == "builtin.set_parameter") {
    VLOG(6) << "Handle for builtin.set_parameter:";
    auto param_name = op->attributes()
                          .at("parameter_name")
                          .dyn_cast<pir::StrAttribute>()
                          .AsString();

    auto value = op->operand_source(0);
    // change opreand name to param_name
    auto orig_name = value_exe_info->GetValue2VarName().at(value);

    PADDLE_ENFORCE_NE(
        param_name,
        orig_name,
        phi::errors::PreconditionNotMet(
            "SetParamer param name should not equal with var name"));

    if (value_exe_info->GetScope()->root()->FindVar(param_name) == nullptr) {
      const_cast<Scope*>(value_exe_info->GetScope()->root())
          ->Rename(orig_name, param_name);
      VLOG(6) << "set_parameter rename var: " << orig_name << " -> "
              << param_name;
    }

    value_exe_info->Rename(value, param_name, orig_name);
  }
  if (op_name.compare(pir::ShadowOutputOp::name()) == 0) {
    VLOG(6) << "Handle for builtin.shadow_ouptut";
    auto var_name = op->attributes()
                        .at("output_name")
                        .dyn_cast<pir::StrAttribute>()
                        .AsString();

    auto value = op->operand_source(0);
    // change opreand name to param_name
    auto orig_name = value_exe_info->GetValue2VarName().at(value);

    if (value_exe_info->GetScope()->FindVar(var_name) != nullptr) {
      const_cast<Scope*>(value_exe_info->GetScope())->EraseVars({var_name});
      VLOG(1) << "var " << var_name << " has been removed from scope";
    }
    const_cast<Scope*>(value_exe_info->GetScope())->Rename(orig_name, var_name);
    VLOG(8) << "var " << orig_name << " has been renamed to " << var_name;

    value_exe_info->Rename(value, var_name, orig_name);
  }

  if (op_name == "builtin.get_parameter") {
    VLOG(6) << "Handle for builtin.get_parameter:";
    auto param_name = op->attributes()
                          .at("parameter_name")
                          .dyn_cast<pir::StrAttribute>()
                          .AsString();
    auto value = op->result(0);

    value_exe_info->Add(value, param_name);
  }

  if (op_name == "builtin.slice") {
    VLOG(6) << "Handle for builtin.slice";
    auto out_value = op->result(0);
    auto in_value = op->operand_source(0);
    PADDLE_ENFORCE_EQ(value_exe_info->GetValue2VarName().count(in_value),
                      true,
                      phi::errors::PreconditionNotMet(
                          "input of buildin slice not in name map"));

    int index =
        op->attributes().at("index").dyn_cast<pir::Int32Attribute>().data();
    auto in_var = value_exe_info->GetScope()->FindVar(
        value_exe_info->GetValue2VarName().at(in_value));
    auto variable_array = in_var->Get<VariableRefArray>();

    PADDLE_ENFORCE_EQ(
        value_exe_info->GetVar2VarName().count(variable_array[index]),
        true,
        phi::errors::PreconditionNotMet("[%d] the variable in build slice "
                                        "input MUST in variable name map",
                                        index));

    std::string var_name =
        value_exe_info->GetVar2VarName().at(variable_array[index]);
    value_exe_info->AddValue2VarName(out_value, var_name);
  }

  if (op_name == "builtin.split") {
    VLOG(6) << "Handle for builtin.split";
    auto in_value = op->operand_source(0);
    PADDLE_ENFORCE_EQ(value_exe_info->GetValue2VarName().count(in_value),
                      true,
                      phi::errors::PreconditionNotMet(
                          "input of buildin split not in name map"));

    auto in_var = value_exe_info->GetScope()->FindVar(
        value_exe_info->GetValue2VarName().at(in_value));
    auto variable_array = in_var->Get<VariableRefArray>();

    for (uint64_t idx = 0; idx < variable_array.size(); ++idx) {
      auto out_value = op->result(idx);
      PADDLE_ENFORCE_EQ(
          value_exe_info->GetVar2VarName().count(variable_array[idx]),
          true,
          phi::errors::PreconditionNotMet("[%d] the variable in build split "
                                          "input MUST in variable name map",
                                          idx));
      std::string var_name =
          value_exe_info->GetVar2VarName().at(variable_array[idx]);
      value_exe_info->AddValue2VarName(out_value, var_name);
    }
  }

  if (op_name == "pd_op.if") {
    auto if_op = op->dyn_cast<paddle::dialect::IfOp>();
    for (size_t i = 0; i < if_op->num_results(); ++i) {
      auto if_op_out_value = if_op->result(i);
      BuildValue(if_op_out_value, var_name_prefix, value_exe_info);
    }
  }

  if (op_name == "pd_op.while") {
    auto while_op = op->dyn_cast<paddle::dialect::WhileOp>();

    for (size_t i = 0; i < while_op->num_results(); ++i) {
      auto while_op_out_value = while_op->result(i);
      BuildValue(while_op_out_value, var_name_prefix, value_exe_info);
    }
  }
}

void HandleForInplaceOp(pir::Operation* op,
                        const std::string& var_name_prefix,
                        ValueExecutionInfo* value_exe_info) {
  if (op->num_results() < 1) return;
  pir::IrContext* ctx = pir::IrContext::Instance();
  std::string op_name = op->name();
  if (op->attributes().count("op_name")) {
    op_name =
        op->attributes().at("op_name").dyn_cast<pir::StrAttribute>().AsString();
  }

  pir::OpInfo op_info = ctx->GetRegisteredOpInfo(op_name);
  paddle::dialect::OpYamlInfoParser yaml_parser(
      op_info.GetInterfaceImpl<paddle::dialect::OpYamlInfoInterface>()
          ->get_op_info_());

  for (size_t i = 0; i < op->num_results(); ++i) {
    pir::Value value = op->result(i);
    if (!IsInvalid(value)) {
      VLOG(8) << "Number " << i << " result of " << op_name
              << " is not invalid, so skip build a variable.";
      continue;
    }
    std::string value_name = yaml_parser.OutputNames()[i];
    if (yaml_parser.HasInplace(value_name)) {
      const std::string& inplace_name = yaml_parser.InplaceName(value_name);
      pir::Value inplace_value =
          op->operand_source(yaml_parser.InputName2Id().at(inplace_name));
      std::string var_name = value_exe_info->GetVarName(inplace_value);
      VLOG(4) << "inplace: " << value_name << " -> " << inplace_name
              << " (var: " << var_name << ")";
      value_exe_info->AddValue2VarName(value, var_name);
    } else if (yaml_parser.HasView(value_name)) {
      const std::string& view_name = yaml_parser.ViewName(value_name);
      pir::Value view_value =
          op->operand_source(yaml_parser.InputName2Id().at(view_name));
      // const std::string& var_name = value_2_var_name->at(view_value);
      std::string var_name = value_exe_info->GetVarName(view_value);
      VLOG(4) << "view: " << value_name << " -> " << view_name
              << " (var: " << var_name << ")";
      value_exe_info->AddValue2VarName(value, var_name);
    } else {
      BuildValue(value, var_name_prefix, value_exe_info);
    }
  }
}

// NOTE(zhiqiu): the persistable is created in inner_scope's root, and other
// is created in inner_scope.
void BuildScope(const pir::Block& block,
                const std::string& var_name_prefix,
                ValueExecutionInfo* value_exe_info) {
  VLOG(4) << "***** [before build] scope"
          << "(" << value_exe_info->GetScope() << ") ******\n"
          << GenScopeTreeDebugInfo(
                 const_cast<Scope*>(value_exe_info->GetScope()->root()));

  for (auto op : block) {
    std::string op_name = op->name();
    if (op->attributes().count("op_name")) {
      op_name = op->attributes()
                    .at("op_name")
                    .dyn_cast<pir::StrAttribute>()
                    .AsString();
    }
    VLOG(4) << "build op:" << op_name;
    if (SpecialOps.count(op_name)) {
      HandleForSpecialOp(op, var_name_prefix, value_exe_info);
      continue;
    }

    CheckInputVars(op, op_name, value_exe_info);

    if (op->num_results() < 1) continue;
    if (op->attributes().count("is_inplace") != 0 &&
        op->attributes()
            .at("is_inplace")
            .dyn_cast<pir::BoolAttribute>()
            .data()) {
      HandleForInplaceOp(op, var_name_prefix, value_exe_info);
      continue;
    } else {
      for (size_t i = 0; i < op->num_results(); ++i) {
        BuildValue(op->result(i), var_name_prefix, value_exe_info);
      }
    }
  }

  VLOG(4) << "***** [after build] scope"
          << "(" << value_exe_info->GetScope() << ") ******\n"
          << GenScopeTreeDebugInfo(
                 const_cast<Scope*>(value_exe_info->GetScope()->root()));
}

void BuildRuntimeContext(pir::Operation* op,
                         const ValueExecutionInfo& value_exec_info,
                         const paddle::dialect::OpYamlInfoParser& op_yaml_info,
                         RuntimeContext* runtime_ctx) {
  const Scope* inner_scope = value_exec_info.GetScope();
  VLOG(6) << "BuildPhiContext in scope[" << inner_scope << "]";

  auto& vec_kernel_fn_tensor_params = op_yaml_info.TensorParams(true);

  auto& name2id = op_yaml_info.InputName2Id();

  std::string fluid_op_name = op_yaml_info.GetOriginOpName();

  auto& op_normalizer = paddle::translator::OpNameNormalizer::instance();

  for (auto& name : vec_kernel_fn_tensor_params) {
    PADDLE_ENFORCE_EQ(
        name2id.count(name),
        true,
        phi::errors::NotFound("param [%s] MUST in name2id map", name));
    auto index = op_yaml_info.InputName2Id().at(name);
    pir::Value ptr = op->operand_source(index);

    if (!IsInvalid(ptr)) {
      VLOG(8) << "ctx->EmplaceBackInput : an optioanl input " << name;
      continue;
    }

    auto legacy_attr_name = op_normalizer.GetLegacyArgName(fluid_op_name, name);
    auto in_var_name = value_exec_info.GetVarName(ptr);
    VLOG(6) << "ctx->EmplaceBackInput: " << name << "\t" << in_var_name;
    PADDLE_ENFORCE_NOT_NULL(inner_scope->FindVar(in_var_name),
                            phi::errors::PreconditionNotMet(
                                "can not find var[%s] in scope", in_var_name));
    auto var = inner_scope->FindVar(in_var_name);
    runtime_ctx->inputs[legacy_attr_name].push_back(var);
  }

  auto& output_name_list = op_yaml_info.OutputNames();
  for (size_t i = 0; i < output_name_list.size(); ++i) {
    auto name = output_name_list[i];
    pir::Value ptr = op->result(i);
    auto legacy_arg_name = op_normalizer.GetLegacyArgName(fluid_op_name, name);

    if (!IsInvalid(ptr)) {
      VLOG(8) << "ctx->EmplaceBackOutput : an optioanl output " << name;
      continue;
    }

    auto in_var_name = value_exec_info.GetVarName(ptr);
    VLOG(6) << "ctx->EmplaceBackOutput: " << name << "\t" << in_var_name;

    PADDLE_ENFORCE_NOT_NULL(inner_scope->FindVar(in_var_name),
                            phi::errors::PreconditionNotMet(
                                "can not find var[%s] in scope", in_var_name));

    auto var = inner_scope->FindVar(in_var_name);

    auto type = ptr.type();
    if (type.isa<paddle::dialect::AllocatedDenseTensorType>() ||
        type.isa<paddle::dialect::AllocatedSelectedRowsType>()) {
      runtime_ctx->outputs[legacy_arg_name] = {var};
    } else if (type.isa<pir::VectorType>()) {
      auto var_ref = var->Get<VariableRefArray>();
      std::vector<Variable*> vec_tmp;
      vec_tmp.reserve(var_ref.size());
      for (size_t k = 0; k < var_ref.size(); ++k) {
        vec_tmp.push_back(const_cast<Variable*>(var_ref[k]));
      }
      runtime_ctx->outputs[legacy_arg_name] = vec_tmp;
    } else {
      PADDLE_THROW(phi::errors::Unimplemented(
          "only support AllocatedDenseTensor, AllocatedSelectedRowsType  and "
          "pir::vector type"));
    }
  }
}

std::shared_ptr<OperatorBase> BuildOperatorBase(
    pir::Operation* op,
    const ValueExecutionInfo& value_exec_info,
    const paddle::dialect::OpYamlInfoParser& op_yaml_info) {
  paddle::framework::VariableNameMap in_name_map;
  paddle::framework::VariableNameMap out_name_map;
  paddle::framework::AttributeMap attr_map;
  auto& vec_kernel_fn_tensor_params = op_yaml_info.TensorParams(true);

  auto& name2id = op_yaml_info.InputName2Id();

  std::string fluid_op_name = op_yaml_info.GetOriginOpName();

  auto& op_normalizer = paddle::translator::OpNameNormalizer::instance();

  auto scope = value_exec_info.GetScope();

  // build inputs
  for (auto& name : vec_kernel_fn_tensor_params) {
    PADDLE_ENFORCE_EQ(
        name2id.count(name),
        true,
        phi::errors::NotFound("param [%s] MUST in name2id map", name));
    auto index = op_yaml_info.InputName2Id().at(name);
    pir::Value ptr = op->operand_source(index);
    auto legacy_attr_name = op_normalizer.GetLegacyArgName(fluid_op_name, name);

    if (!IsInvalid(ptr)) {
      VLOG(8) << "Push back inputs to VariableNameMap : an optioanl input "
              << name;
      continue;
    }
    VLOG(6) << "Push back inputs to VariableNameMap : "
            << value_exec_info.GetVarName(ptr);
    in_name_map[legacy_attr_name].push_back(value_exec_info.GetVarName(ptr));
  }

  // build attribute
  auto& op_attr_map = op->attributes();
  auto attr_name_list = op_yaml_info.AttrParams(true);
  for (auto& name : attr_name_list) {
    auto& val = op_attr_map.at(name);

    if (val.isa<pir::StrAttribute>()) {
      attr_map[name] = val.dyn_cast<pir::StrAttribute>().AsString();
    } else if (val.isa<pir::Int32Attribute>()) {
      attr_map[name] = val.dyn_cast<pir::Int32Attribute>().data();
    } else if (val.isa<pir::BoolAttribute>()) {
      attr_map[name] = val.dyn_cast<pir::BoolAttribute>().data();
    } else if (val.isa<pir::FloatAttribute>()) {
      attr_map[name] = val.dyn_cast<pir::FloatAttribute>().data();
    } else if (val.isa<pir::DoubleAttribute>()) {
      attr_map[name] = val.dyn_cast<pir::DoubleAttribute>().data();
    } else if (val.isa<pir::Int64Attribute>()) {
      attr_map[name] = val.dyn_cast<pir::Int64Attribute>().data();
    } else if (val.isa<pir::ArrayAttribute>()) {
      auto array_list = val.dyn_cast<pir::ArrayAttribute>().AsVector();
      PADDLE_ENFORCE(
          array_list.size() > 0,
          paddle::platform::errors::Fatal("Attribute %s is empty", name));
      if (array_list[0].isa<pir::Int32Attribute>()) {
        std::vector<int> vec_int;
        for (auto attribute : array_list) {
          vec_int.push_back(attribute.dyn_cast<pir::Int32Attribute>().data());
        }
        attr_map[name] = vec_int;
      } else if (array_list[0].isa<pir::Int64Attribute>()) {
        std::vector<int> vec_int64;
        for (auto attribute : array_list) {
          vec_int64.push_back(
              attribute.dyn_cast<pir::Int64Attribute>().data());  // NOLINT
        }
        attr_map[name] = vec_int64;
      } else if (array_list[0].isa<pir::BoolAttribute>()) {
        std::vector<int> vec_bool;
        for (auto attribute : array_list) {
          vec_bool.push_back(attribute.dyn_cast<pir::BoolAttribute>().data());
        }
        attr_map[name] = vec_bool;
      } else if (array_list[0].isa<pir::FloatAttribute>()) {
        std::vector<int> vec_float;
        for (auto attribute : array_list) {
          vec_float.push_back(
              attribute.dyn_cast<pir::FloatAttribute>().data());  // NOLINT
        }
        attr_map[name] = vec_float;
      } else if (array_list[0].isa<pir::DoubleAttribute>()) {
        std::vector<int> vec_double;
        for (auto attribute : array_list) {
          vec_double.push_back(
              attribute.dyn_cast<pir::DoubleAttribute>().data());  // NOLINT
        }
        attr_map[name] = vec_double;
      } else {
        std::stringstream ss;
        val.Print(ss);
        VLOG(1) << "type not support " << ss.str() << std::endl;
        PADDLE_THROW("Type[%s] in attribute map not support yet", ss.str());
      }
    } else if (val.isa<paddle::dialect::DataTypeAttribute>()) {
      attr_map[name] = paddle::framework::TransToProtoVarType(
          val.dyn_cast<paddle::dialect::DataTypeAttribute>().data());
    } else {
      std::stringstream ss;
      val.Print(ss);
      VLOG(1) << "type not support " << ss.str() << std::endl;
      PADDLE_THROW("Type[%s] in attribute map not support yet", ss.str());
    }
  }

  // build outputs
  auto& output_name_list = op_yaml_info.OutputNames();
  for (size_t i = 0; i < output_name_list.size(); ++i) {
    pir::Value ptr = op->result(i);
    auto legacy_arg_name =
        op_normalizer.GetLegacyArgName(fluid_op_name, output_name_list[i]);

    if (!IsInvalid(ptr)) {
      VLOG(8) << "Push back outputs to VariableNameMap : an optioanl output "
              << legacy_arg_name;
      continue;
    }

    if (ptr.type().isa<paddle::dialect::AllocatedDenseTensorType>() ||
        ptr.type().isa<paddle::dialect::AllocatedSelectedRowsType>()) {
      out_name_map[legacy_arg_name].push_back(value_exec_info.GetVarName(ptr));
      VLOG(6) << "Push back outputs to VariableNameMap : "
              << value_exec_info.GetVarName(ptr);
    } else if (ptr.type().isa<pir::VectorType>()) {
      auto var = scope->FindVar(value_exec_info.GetVarName(ptr));
      auto var_ref = var->Get<VariableRefArray>();
      for (size_t k = 0; k < var_ref.size(); ++k) {
        out_name_map[legacy_arg_name].push_back(
            value_exec_info.GetVarName(var_ref[k]));
        VLOG(6) << "Push back outputs to VariableNameMap : "
                << value_exec_info.GetVarName(var_ref[k]);
      }
    } else {
      PADDLE_THROW(phi::errors::Unimplemented(
          "only support AllocatedDenseTensor, AllocatedSelectedRowsType  and "
          "pir::vector type"));
    }
  }

  auto& op_info = OpInfoMap::Instance().Get(fluid_op_name);
  auto ptr =
      op_info.Creator()(fluid_op_name, in_name_map, out_name_map, attr_map);

  std::shared_ptr<OperatorBase> res(ptr);
  return res;
}

}  // namespace framework
}  // namespace paddle