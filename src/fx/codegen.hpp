#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "effect_codegen.hpp"

namespace sopt::fx {

// What the reshadefx parser emits for one effect, recorded as a dataflow graph: every
// SSA value with its type, source location and basic block, plus the statements that
// store values into variables. Regions are read from this (see frontend.hpp).
struct Value {
  enum class Kind { Const, Load, Chain, Unary, Binary, Ternary, Intrinsic, Call, Construct, Phi };
  Kind kind = Kind::Const;
  reshadefx::type type{};
  reshadefx::location loc;
  uint32_t block = 0;
  uint32_t seq = 0;                       // emission order (shared with Statement::seq)
  reshadefx::tokenid op{};                // Unary / Binary / Ternary
  std::string name;                       // Intrinsic name, or called function's unique name
  std::vector<uint32_t> args;             // operand value ids
  reshadefx::constant constant{};         // Const
  uint32_t base = 0;                      // Load: variable id; Chain: value id
  std::vector<reshadefx::expression::operation> chain;  // Load / Chain access operations
};

struct Variable {
  enum class Kind { Local, Param, Global, Uniform };
  Kind kind = Kind::Local;
  std::string name;                               // source name (globals: unique name)
  std::string uniqueName;                         // Uniform / Global
  reshadefx::type type{};
  std::string semantic;                           // Param
  std::vector<reshadefx::annotation> annotations; // Uniform
  reshadefx::location loc;
  uint32_t initializer = 0;                       // value id, if any
};

struct Statement {
  enum class Kind { Init, Store, Return };
  Kind kind = Kind::Store;
  uint32_t var = 0;  // Init / Store
  std::vector<reshadefx::expression::operation> chain;  // Store: lvalue access chain
  uint32_t value = 0;
  reshadefx::location loc;  // Init: variable name; Store: lvalue start; Return: value start
  uint32_t block = 0;
  uint32_t seq = 0;         // emission order within the effect
};

struct Function {
  uint32_t id = 0;
  std::string name, uniqueName;
  reshadefx::location loc;
  reshadefx::type returnType{};
  std::string returnSemantic;
  reshadefx::shader_type type = reshadefx::shader_type::unknown;  // set for entry points
  std::vector<uint32_t> params;  // variable ids
  std::vector<Statement> stmts;
  std::vector<std::string> calls;  // unique names of called functions
};

struct SamplerInfo {
  std::string name;
  std::string textureName;
  std::string textureSemantic;  // COLOR, DEPTH, ...
  reshadefx::texture_format format = reshadefx::texture_format::unknown;
  bool srgb = false;
};

class Codegen final : public reshadefx::codegen {
 public:
  std::unordered_map<uint32_t, Value> values;
  std::unordered_map<uint32_t, Variable> variables;
  std::unordered_map<uint32_t, SamplerInfo> samplers;
  std::vector<std::unique_ptr<Function>> functions;  // in definition order
  std::unordered_map<std::string, reshadefx::shader_type> entryPoints;  // unique name -> type
  std::vector<std::pair<uint32_t, uint32_t>> loops;  // [first, last] seq of each loop

  std::string structMemberName(uint32_t structId, uint32_t index) const;
  const Function* function(const std::string& uniqueName) const;
  const reshadefx::effect_module& mod() const { return _module; }

  std::string finalize_code() const override { return {}; }
  bool assemble_code_for_entry_point(const std::string&, std::string&, std::string&,
                                     std::string&) const override {
    return false;
  }

 private:
  Value& newValue(Value::Kind kind, const reshadefx::type& type, const reshadefx::location& loc,
                  id& res);
  void addStatement(Statement s);

  id define_struct(const reshadefx::location& loc, reshadefx::struct_type& info) override;
  id define_texture(const reshadefx::location& loc, reshadefx::texture& info) override;
  id define_sampler(const reshadefx::location& loc, const reshadefx::texture& tex,
                    reshadefx::sampler& info) override;
  id define_storage(const reshadefx::location& loc, const reshadefx::texture& tex,
                    reshadefx::storage& info) override;
  id define_uniform(const reshadefx::location& loc, reshadefx::uniform& info) override;
  id define_variable(const reshadefx::location& loc, const reshadefx::type& type, std::string name,
                     bool global, id initializer_value) override;
  id define_function(const reshadefx::location& loc, reshadefx::function& info) override;
  void define_entry_point(reshadefx::function& function) override;
  id emit_load(const reshadefx::expression& chain, bool force_new_id) override;
  void emit_store(const reshadefx::expression& chain, id value) override;
  id emit_constant(const reshadefx::type& type, const reshadefx::constant& data) override;
  id emit_unary_op(const reshadefx::location& loc, reshadefx::tokenid op,
                   const reshadefx::type& type, id val) override;
  id emit_binary_op(const reshadefx::location& loc, reshadefx::tokenid op,
                    const reshadefx::type& res_type, const reshadefx::type& type, id lhs,
                    id rhs) override;
  id emit_ternary_op(const reshadefx::location& loc, reshadefx::tokenid op,
                     const reshadefx::type& type, id condition, id true_value,
                     id false_value) override;
  id emit_call(const reshadefx::location& loc, id function, const reshadefx::type& res_type,
               const std::vector<reshadefx::expression>& args) override;
  id emit_call_intrinsic(const reshadefx::location& loc, id function,
                         const reshadefx::type& res_type,
                         const std::vector<reshadefx::expression>& args) override;
  id emit_construct(const reshadefx::location& loc, const reshadefx::type& type,
                    const std::vector<reshadefx::expression>& args) override;
  void emit_if(const reshadefx::location&, id, id, id, id, unsigned int) override {}
  id emit_phi(const reshadefx::location& loc, id condition_value, id condition_block,
              id true_value, id true_statement_block, id false_value, id false_statement_block,
              const reshadefx::type& type) override;
  void emit_loop(const reshadefx::location&, id, id, id header, id, id, id, unsigned int) override {
    const auto it = blockSeq_.find(header);
    loops.emplace_back(it == blockSeq_.end() ? 0 : it->second, seq_);
  }
  void emit_switch(const reshadefx::location&, id, id, id, id, const std::vector<id>&,
                   const std::vector<id>&, unsigned int) override {}
  void emit_pragma(const std::string&) override {}
  id set_block(id id) override;
  void enter_block(id id) override {
    _current_block = id;
    blockSeq_.emplace(id, seq_);
  }
  id leave_block_and_kill() override;
  id leave_block_and_return(id value) override;
  id leave_block_and_switch(id, id) override;
  id leave_block_and_branch(id, unsigned int) override;
  id leave_block_and_branch_conditional(id, id, id) override;
  void leave_function() override;

  Function* current_ = nullptr;
  uint32_t seq_ = 0;
  std::unordered_map<uint32_t, uint32_t> blockSeq_;  // block id -> seq when first entered
};

// Name of a reshadefx intrinsic id ("lerp", "tex2D", ...).
std::string intrinsicName(uint32_t id);

}  // namespace sopt::fx
