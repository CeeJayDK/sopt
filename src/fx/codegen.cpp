#include "fx/codegen.hpp"

#include <array>

namespace sopt::fx {

namespace {

// Intrinsic ids are numbered in the order of IMPLEMENT_INTRINSIC_SPIRV in the table
// (see effect_symbol_table.cpp), so the same expansion gives their names.
const std::vector<std::string>& intrinsicNames() {
  static const std::vector<std::string> names = {
#define IMPLEMENT_INTRINSIC_SPIRV(name, i, code) #name,
#include "effect_symbol_table_intrinsics.inl"
  };
  return names;
}

}  // namespace

std::string intrinsicName(uint32_t id) {
  const auto& names = intrinsicNames();
  return id < names.size() ? names[id] : std::string();
}

std::string Codegen::structMemberName(uint32_t structId, uint32_t index) const {
  for (const auto& s : _structs)
    if (s.id == structId && index < s.member_list.size()) return s.member_list[index].name;
  return "m" + std::to_string(index);
}

const Function* Codegen::function(const std::string& uniqueName) const {
  for (const auto& f : functions)
    if (f->uniqueName == uniqueName) return f.get();
  return nullptr;
}

Value& Codegen::newValue(Value::Kind kind, const reshadefx::type& type,
                         const reshadefx::location& loc, id& res) {
  res = make_id();
  Value& v = values[res];
  v.kind = kind;
  v.type = type;
  v.loc = loc;
  v.block = _current_block;
  v.seq = seq_++;
  return v;
}

void Codegen::addStatement(Statement s) {
  if (!current_) return;
  s.block = _current_block;
  s.seq = seq_++;
  current_->stmts.push_back(std::move(s));
}

reshadefx::codegen::id Codegen::define_struct(const reshadefx::location&,
                                              reshadefx::struct_type& info) {
  info.id = make_id();
  _structs.push_back(info);
  return info.id;
}

reshadefx::codegen::id Codegen::define_texture(const reshadefx::location&,
                                               reshadefx::texture& info) {
  info.id = make_id();
  _module.textures.push_back(info);
  return info.id;
}

reshadefx::codegen::id Codegen::define_sampler(const reshadefx::location&,
                                               const reshadefx::texture& tex,
                                               reshadefx::sampler& info) {
  info.id = make_id();
  _module.samplers.push_back(info);
  samplers[info.id] = {info.name, tex.name, tex.semantic, tex.format, info.srgb};
  return info.id;
}

reshadefx::codegen::id Codegen::define_storage(const reshadefx::location&,
                                               const reshadefx::texture&,
                                               reshadefx::storage& info) {
  info.id = make_id();
  _module.storages.push_back(info);
  return info.id;
}

reshadefx::codegen::id Codegen::define_uniform(const reshadefx::location& loc,
                                               reshadefx::uniform& info) {
  const id res = make_id();
  Variable& v = variables[res];
  v.kind = Variable::Kind::Uniform;
  v.name = info.name;
  v.uniqueName = info.unique_name;
  v.type = info.type;
  v.annotations = info.annotations;
  v.loc = loc;
  _module.uniforms.push_back(info);
  return res;
}

reshadefx::codegen::id Codegen::define_variable(const reshadefx::location& loc,
                                                const reshadefx::type& type, std::string name,
                                                bool global, id initializer_value) {
  const id res = make_id();
  Variable& v = variables[res];
  v.kind = global ? Variable::Kind::Global : Variable::Kind::Local;
  if (global) v.uniqueName = name;
  v.name = std::move(name);
  v.type = type;
  v.loc = loc;
  v.initializer = initializer_value;
  if (!global && initializer_value != 0) {
    Statement s;
    s.kind = Statement::Kind::Init;
    s.var = res;
    s.value = initializer_value;
    s.loc = loc;
    addStatement(std::move(s));
  }
  return res;
}

reshadefx::codegen::id Codegen::define_function(const reshadefx::location& loc,
                                                reshadefx::function& info) {
  const id res = info.id = make_id();
  auto f = std::make_unique<Function>();
  f->id = res;
  f->name = info.name;
  f->uniqueName = info.unique_name;
  f->loc = loc;
  f->returnType = info.return_type;
  f->returnSemantic = info.return_semantic;
  for (auto& param : info.parameter_list) {
    param.id = make_id();
    Variable& v = variables[param.id];
    v.kind = Variable::Kind::Param;
    v.name = param.name;
    v.type = param.type;
    v.semantic = param.semantic;
    v.loc = param.location;
    f->params.push_back(param.id);
  }
  current_ = f.get();
  functions.push_back(std::move(f));
  _functions.push_back(std::make_unique<reshadefx::function>(info));
  _current_function = _functions.back().get();
  return res;
}

void Codegen::define_entry_point(reshadefx::function& function) {
  entryPoints[function.unique_name] = function.type;
  for (auto& f : functions)
    if (f->uniqueName == function.unique_name) f->type = function.type;
  bool known = false;
  for (const auto& e : _module.entry_points) known = known || e.first == function.unique_name;
  if (!known) _module.entry_points.emplace_back(function.unique_name, function.type);
}

reshadefx::codegen::id Codegen::emit_load(const reshadefx::expression& exp, bool force_new_id) {
  if (exp.is_constant) return emit_constant(exp.type, exp.constant);
  if (exp.chain.empty() && !exp.is_lvalue && !force_new_id) return exp.base;
  id res;
  Value& v = newValue(exp.is_lvalue ? Value::Kind::Load : Value::Kind::Chain, exp.type,
                      exp.location, res);
  v.base = exp.base;
  v.chain = exp.chain;
  return res;
}

void Codegen::emit_store(const reshadefx::expression& exp, id value) {
  Statement s;
  s.kind = Statement::Kind::Store;
  s.var = exp.base;
  s.chain = exp.chain;
  s.value = value;
  s.loc = exp.location;
  addStatement(std::move(s));
}

reshadefx::codegen::id Codegen::emit_constant(const reshadefx::type& type,
                                              const reshadefx::constant& data) {
  id res;
  Value& v = newValue(Value::Kind::Const, type, {}, res);
  v.constant = data;
  return res;
}

reshadefx::codegen::id Codegen::emit_unary_op(const reshadefx::location& loc, reshadefx::tokenid op,
                                              const reshadefx::type& type, id val) {
  id res;
  Value& v = newValue(Value::Kind::Unary, type, loc, res);
  v.op = op;
  v.args = {val};
  return res;
}

reshadefx::codegen::id Codegen::emit_binary_op(const reshadefx::location& loc,
                                               reshadefx::tokenid op,
                                               const reshadefx::type& res_type,
                                               const reshadefx::type&, id lhs, id rhs) {
  id res;
  Value& v = newValue(Value::Kind::Binary, res_type, loc, res);
  v.op = op;
  v.args = {lhs, rhs};
  return res;
}

reshadefx::codegen::id Codegen::emit_ternary_op(const reshadefx::location& loc,
                                                reshadefx::tokenid op, const reshadefx::type& type,
                                                id condition, id true_value, id false_value) {
  id res;
  Value& v = newValue(Value::Kind::Ternary, type, loc, res);
  v.op = op;
  v.args = {condition, true_value, false_value};
  return res;
}

reshadefx::codegen::id Codegen::emit_call(const reshadefx::location& loc, id function,
                                          const reshadefx::type& res_type,
                                          const std::vector<reshadefx::expression>& args) {
  id res;
  Value& v = newValue(Value::Kind::Call, res_type, loc, res);
  for (const auto& f : functions)
    if (f->id == function) v.name = f->uniqueName;
  for (const auto& a : args) v.args.push_back(a.base);
  if (current_) current_->calls.push_back(v.name);
  return res;
}

reshadefx::codegen::id Codegen::emit_call_intrinsic(const reshadefx::location& loc, id function,
                                                    const reshadefx::type& res_type,
                                                    const std::vector<reshadefx::expression>& args) {
  id res;
  Value& v = newValue(Value::Kind::Intrinsic, res_type, loc, res);
  v.name = intrinsicName(function);
  for (const auto& a : args) v.args.push_back(a.base);
  return res;
}

reshadefx::codegen::id Codegen::emit_construct(const reshadefx::location& loc,
                                               const reshadefx::type& type,
                                               const std::vector<reshadefx::expression>& args) {
  id res;
  Value& v = newValue(Value::Kind::Construct, type, loc, res);
  for (const auto& a : args) v.args.push_back(a.base);
  return res;
}

reshadefx::codegen::id Codegen::emit_phi(const reshadefx::location& loc, id condition_value, id,
                                         id true_value, id, id false_value, id,
                                         const reshadefx::type& type) {
  id res;
  Value& v = newValue(Value::Kind::Phi, type, loc, res);
  v.args = {condition_value, true_value, false_value};
  return res;
}

reshadefx::codegen::id Codegen::set_block(id id) {
  _last_block = _current_block;
  _current_block = id;
  return _last_block;
}

reshadefx::codegen::id Codegen::leave_block_and_kill() {
  if (!is_in_block()) return 0;
  return set_block(0);
}

reshadefx::codegen::id Codegen::leave_block_and_return(id value) {
  if (!is_in_block()) return 0;
  if (value != 0) {
    Statement s;
    s.kind = Statement::Kind::Return;
    s.value = value;
    if (auto it = values.find(value); it != values.end()) s.loc = it->second.loc;
    addStatement(std::move(s));
  }
  return set_block(0);
}

reshadefx::codegen::id Codegen::leave_block_and_switch(id, id) {
  if (!is_in_block()) return _last_block;
  return set_block(0);
}

reshadefx::codegen::id Codegen::leave_block_and_branch(id, unsigned int) {
  if (!is_in_block()) return _last_block;
  return set_block(0);
}

reshadefx::codegen::id Codegen::leave_block_and_branch_conditional(id, id, id) {
  if (!is_in_block()) return _last_block;
  return set_block(0);
}

void Codegen::leave_function() {
  current_ = nullptr;
  _current_function = nullptr;
}

}  // namespace sopt::fx
