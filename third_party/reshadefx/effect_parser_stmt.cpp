/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "effect_lexer.hpp"
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include <limits>
#include <cctype> // std::toupper
#include <cassert>
#include <cstring> // sopt
#include <iterator> // std::back_inserter
#include <algorithm> // std::max, std::replace, std::transform
#include <string_view>

template <typename ENTER_TYPE, typename LEAVE_TYPE>
struct scope_guard
{
	explicit scope_guard(ENTER_TYPE &&enter_lambda, LEAVE_TYPE &&leave_lambda) :
		leave_lambda(std::forward<LEAVE_TYPE>(leave_lambda)) { enter_lambda(); }
	~scope_guard() { leave_lambda(); }

private:
	LEAVE_TYPE leave_lambda;
};

bool reshadefx::parser::parse(std::string source, codegen *backend)
{
	_lexer = new lexer(std::move(source));
	_codegen = backend;

	consume();

	bool parse_success = true;
	bool current_success = true;

	while (!peek(tokenid::end_of_file))
	{
		if (!parse_top(current_success))
		{
			parse_success = false;
			break;
		}
		if (!current_success)
		{
			parse_success = false;
			continue;
		}
	}

	delete _lexer;

	if (parse_success)
	{
		backend->optimize_bindings();

		assert(_loop_break_target_stack.empty() && _loop_continue_target_stack.empty());
	}

	_loop_break_target_stack.clear();
	_loop_continue_target_stack.clear();

	return parse_success;
}

bool reshadefx::parser::parse_top(bool &parse_success)
{
	if (accept(tokenid::pragma))
	{
		if (!expect('(') || !expect(tokenid::string_literal))
			return false;

		_codegen->emit_pragma(_token.literal_as_string);

		if (!expect(')'))
			return false;
	}

	if (accept(tokenid::namespace_))
	{
		// Anonymous namespaces are not supported right now, so an identifier is a must
		if (!expect(tokenid::identifier))
			return false;

		const std::string name = std::move(_token.literal_as_string);

		if (!expect('{'))
			return false;

		enter_namespace(name);

		bool current_success = true;
		bool parse_success_namespace = true;

		// Recursively parse top level statements until the namespace is closed again
		while (!peek('}') && !peek(tokenid::end_of_file)) // Empty namespaces are valid
		{
			if (!parse_top(current_success))
			{
				return false;
			}
			if (!current_success)
			{
				parse_success_namespace = false;
				continue;
			}
		}

		leave_namespace();

		parse_success = expect('}') && parse_success_namespace;
	}
	else if (accept(tokenid::struct_)) // Structure keyword found, parse the structure definition
	{
		// Structure definitions are terminated with a semicolon
		parse_success = parse_struct() && expect(';');
	}
	else if (accept(tokenid::technique)) // Technique keyword found, parse the technique definition
	{
		parse_success = parse_technique();
	}
	else
	{
		// sopt: HLSL resource declarations (cbuffer, textures, sampler states)
		if (sopt_hlsl)
		{
			bool handled = false;
			if (!sopt_hlsl_declaration(handled, parse_success))
				return false;
			if (handled)
				return true;
		}
		// sopt: GLSL declarations (precision, layout, global in / out, samplers, uniform blocks)
		if (sopt_glsl)
		{
			bool handled = false;
			if (!sopt_glsl_declaration(handled, parse_success))
				return false;
			if (handled)
				return true;
		}

		location attribute_location;
		shader_type stype = shader_type::unknown;
		int num_threads[3] = { 0, 0, 0 };

		// Read any function attributes first
		while (accept('['))
		{
			if (!expect(tokenid::identifier))
				return false;

			const std::string attribute = std::move(_token.literal_as_string);

			if (attribute == "shader")
			{
				attribute_location = _token_next.location;

				if (!expect('(') || !expect(tokenid::string_literal))
					return false;

				if (_token.literal_as_string == "vertex")
					stype = shader_type::vertex;
				else if (_token.literal_as_string == "pixel")
					stype = shader_type::pixel;
				else if (_token.literal_as_string == "compute")
					stype = shader_type::compute;

				if (!expect(')'))
					return false;
			}
			else if (attribute == "numthreads")
			{
				attribute_location = _token_next.location;

				expression x, y, z;
				if (!expect('(') || !parse_expression_multary(x, 8) || !expect(',') || !parse_expression_multary(y, 8) || !expect(',') || !parse_expression_multary(z, 8) || !expect(')'))
					return false;

				if (!x.is_constant)
				{
					error(x.location, 3011, "value must be a literal expression");
					parse_success = false;
				}
				if (!y.is_constant)
				{
					error(y.location, 3011, "value must be a literal expression");
					parse_success = false;
				}
				if (!z.is_constant)
				{
					error(z.location, 3011, "value must be a literal expression");
					parse_success = false;
				}
				x.add_cast_operation({ type::t_int, 1, 1 });
				y.add_cast_operation({ type::t_int, 1, 1 });
				z.add_cast_operation({ type::t_int, 1, 1 });
				num_threads[0] = x.constant.as_int[0];
				num_threads[1] = y.constant.as_int[0];
				num_threads[2] = z.constant.as_int[0];
			}
			else
			{
				warning(_token.location, 0, "unknown attribute '" + attribute + "'");
			}

			if (!expect(']'))
				return false;
		}

		if (type type = {}; parse_type(type)) // Type found, this can be either a variable or a function declaration
		{
			parse_success = expect(tokenid::identifier);
			if (!parse_success)
				return true;

			if (peek('('))
			{
				const std::string name = std::move(_token.literal_as_string);

				// sopt: the HLSL entry point (a compute shader when it has [numthreads])
				if ((sopt_hlsl || sopt_glsl) && stype == shader_type::unknown && name == sopt_hlsl_entry)
					stype = num_threads[0] != 0 ? shader_type::compute : shader_type::pixel;

				// This is definitely a function declaration, so parse it
				if (!parse_function(type, name, stype, num_threads))
				{
					// Insert dummy function into symbol table, so later references can be resolved despite the error
					insert_symbol(name, { symbol_type::function, UINT32_MAX, { type::t_function } }, true);
					parse_success = false;
					return true;
				}
			}
			else
			{
				if (!attribute_location.source.empty())
				{
					error(attribute_location, 0, "attribute is valid only on functions");
					parse_success = false;
				}

				// sopt: GLSL globals are private unless uniform (ReShade FX globals need static)
				if (sopt_glsl && !type.has(type::q_uniform))
					type.qualifiers |= type::q_static;

				// There may be multiple variable names after the type, handle them all
				unsigned int count = 0;
				do
				{
					if (count++ > 0 && !(expect(',') && expect(tokenid::identifier)))
					{
						parse_success = false;
						return false;
					}

					const std::string name = std::move(_token.literal_as_string);

					if (!parse_variable(type, name, true))
					{
						// Insert dummy variable into symbol table, so later references can be resolved despite the error
						insert_symbol(name, { symbol_type::variable, UINT32_MAX, type }, true);
						// Skip the rest of the statement
						consume_until(';');
						parse_success = false;
						return true;
					}
				}
				while (!peek(';'));

				// Variable declarations are terminated with a semicolon
				parse_success = expect(';');
			}
		}
		else if (accept(';')) // Ignore single semicolons in the source
		{
			parse_success = true;
		}
		else
		{
			// Unexpected token in source stream, consume and report an error about it
			consume();
			// Only add another error message if succeeded parsing previously
			// This is done to avoid walls of error messages because of consequential errors following a top-level syntax mistake
			if (parse_success)
				error(_token.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token.id) + '\'');
			parse_success = false;
		}
	}

	return true;
}

bool reshadefx::parser::parse_statement(bool scoped)
{
	if (!_codegen->is_in_block())
	{
		error(_token_next.location, 0, "unreachable code");
		return false;
	}

	unsigned int loop_control = 0;
	unsigned int selection_control = 0;

	// Read any loop and branch control attributes first
	while (accept('['))
	{
		enum control_mask
		{
			unroll = 0x1,
			dont_unroll = 0x2,
			flatten = (0x1 << 4),
			dont_flatten = (0x2 << 4),
			switch_force_case = (0x4 << 4),
			switch_call = (0x8 << 4)
		};

		const std::string attribute = std::move(_token_next.literal_as_string);

		if (!expect(tokenid::identifier) || !expect(']'))
			return false;

		if (attribute == "unroll")
			loop_control |= unroll;
		else if (attribute == "loop" || attribute == "fastopt")
			loop_control |= dont_unroll;
		else if (attribute == "flatten")
			selection_control |= flatten;
		else if (attribute == "branch")
			selection_control |= dont_flatten;
		else if (attribute == "forcecase")
			selection_control |= switch_force_case;
		else if (attribute == "call")
			selection_control |= switch_call;
		else
			warning(_token.location, 0, "unknown attribute '" + attribute + "'");

		if ((loop_control & (unroll | dont_unroll)) == (unroll | dont_unroll))
		{
			error(_token.location, 3524, "can't use loop and unroll attributes together");
			return false;
		}
		if ((selection_control & (flatten | dont_flatten)) == (flatten | dont_flatten))
		{
			error(_token.location, 3524, "can't use branch and flatten attributes together");
			return false;
		}
	}

	// Shift by two so that the possible values are 0x01 for 'flatten' and 0x02 for 'dont_flatten', equivalent to 'unroll' and 'dont_unroll'
	selection_control >>= 4;

	if (peek('{')) // Parse statement block
		return parse_statement_block(scoped);

	if (accept(';')) // Ignore empty statements
		return true;

	// Most statements with the exception of declarations are only valid inside functions
	if (_codegen->is_in_function())
	{
		const location statement_location = _token_next.location;

		if (accept(tokenid::if_))
		{
			codegen::id true_block = _codegen->create_block(); // Block which contains the statements executed when the condition is true
			codegen::id false_block = _codegen->create_block(); // Block which contains the statements executed when the condition is false
			const codegen::id merge_block = _codegen->create_block(); // Block that is executed after the branch re-merged with the current control flow

			expression condition_exp;
			if (!expect('(') || !parse_expression(condition_exp) || !expect(')'))
				return false;

			if (!condition_exp.type.is_scalar())
			{
				error(condition_exp.location, 3019, "if statement conditional expressions must evaluate to a scalar");
				return false;
			}

			// Load condition and convert to boolean value as required by 'OpBranchConditional' in SPIR-V
			condition_exp.add_cast_operation({ type::t_bool, 1, 1 });

			const codegen::id condition_value = _codegen->emit_load(condition_exp);
			const codegen::id condition_block = _codegen->leave_block_and_branch_conditional(condition_value, true_block, false_block);

			{ // Then block of the if statement
				_codegen->enter_block(true_block);

				if (!parse_statement(true))
					return false;

				true_block = _codegen->leave_block_and_branch(merge_block);
			}
			{ // Else block of the if statement
				_codegen->enter_block(false_block);

				if (accept(tokenid::else_) && !parse_statement(true))
					return false;

				false_block = _codegen->leave_block_and_branch(merge_block);
			}

			_codegen->enter_block(merge_block);

			// Emit structured control flow for an if statement and connect all basic blocks
			_codegen->emit_if(statement_location, condition_value, condition_block, true_block, false_block, selection_control);

			return true;
		}

		if (accept(tokenid::switch_))
		{
			const codegen::id merge_block = _codegen->create_block(); // Block that is executed after the switch re-merged with the current control flow

			expression selector_exp;
			if (!expect('(') || !parse_expression(selector_exp) || !expect(')'))
				return false;

			if (!selector_exp.type.is_scalar())
			{
				error(selector_exp.location, 3019, "switch statement expression must evaluate to a scalar");
				return false;
			}

			// Load selector and convert to integral value as required by switch instruction
			selector_exp.add_cast_operation({ type::t_int, 1, 1 });

			const codegen::id selector_value = _codegen->emit_load(selector_exp);
			const codegen::id selector_block = _codegen->leave_block_and_switch(selector_value, merge_block);

			if (!expect('{'))
				return false;

			scope_guard _(
				[this, merge_block]() {
					_loop_break_target_stack.push_back(merge_block);
				},
				[this]() {
					_loop_break_target_stack.pop_back();
				});

			bool parse_success = true;
			// The default case jumps to the end of the switch statement if not overwritten
			codegen::id default_label = merge_block, default_block = merge_block;
			codegen::id current_label = _codegen->create_block();
			std::vector<codegen::id> case_literal_and_labels, case_blocks;
			size_t last_case_label_index = 0;

			// Enter first switch statement body block
			_codegen->enter_block(current_label);

			while (!peek(tokenid::end_of_file))
			{
				while (accept(tokenid::case_) || accept(tokenid::default_))
				{
					if (_token.id == tokenid::case_)
					{
						expression case_label;
						if (!parse_expression(case_label))
						{
							consume_until('}');
							return false;
						}

						if (!case_label.type.is_scalar() || !case_label.type.is_integral() || !case_label.is_constant)
						{
							error(case_label.location, 3020, "invalid type for case expression - value must be an integer scalar");
							consume_until('}');
							return false;
						}

						// Check for duplicate case values
						for (size_t i = 0; i < case_literal_and_labels.size(); i += 2)
						{
							if (case_literal_and_labels[i] == case_label.constant.as_uint[0])
							{
								parse_success = false;
								error(case_label.location, 3532, "duplicate case " + std::to_string(case_label.constant.as_uint[0]));
								break;
							}
						}

						case_blocks.emplace_back(); // This is set to the actual block below
						case_literal_and_labels.push_back(case_label.constant.as_uint[0]);
						case_literal_and_labels.push_back(current_label);
					}
					else
					{
						// Check if the default label was already changed by a previous 'default' statement
						if (default_label != merge_block)
						{
							parse_success = false;
							error(_token.location, 3532, "duplicate default in switch statement");
						}

						default_label = current_label;
						default_block = 0; // This is set to the actual block below
					}

					if (!expect(':'))
					{
						consume_until('}');
						return false;
					}
				}

				// It is valid for no statement to follow if this is the last label in the switch body
				const bool end_of_switch = peek('}');

				if (!end_of_switch && !parse_statement(true))
				{
					consume_until('}');
					return false;
				}

				// Handle fall-through case and end of switch statement
				if (peek(tokenid::case_) || peek(tokenid::default_) || end_of_switch)
				{
					if (_codegen->is_in_block()) // Disallow fall-through for now
					{
						parse_success = false;
						error(_token_next.location, 3533, "non-empty case statements must have break or return");
					}

					const codegen::id next_label = end_of_switch ? merge_block : _codegen->create_block();
					// This is different from 'current_label', since there may have been branching logic inside the case, which would have changed the active block
					const codegen::id current_block = _codegen->leave_block_and_branch(next_label);

					if (0 == default_block)
						default_block = current_block;
					for (size_t i = last_case_label_index; i < case_blocks.size(); ++i)
						// Need to use the initial label for the switch table, but the current block to merge all the block data
						case_blocks[i] = current_block;

					current_label = next_label;
					_codegen->enter_block(current_label);

					if (end_of_switch) // We reached the end, nothing more to do
						break;

					last_case_label_index = case_blocks.size();
				}
			}

			if (case_literal_and_labels.empty() && default_label == merge_block)
				warning(statement_location, 5002, "switch statement contains no 'case' or 'default' labels");

			// Emit structured control flow for a switch statement and connect all basic blocks
			_codegen->emit_switch(statement_location, selector_value, selector_block, default_label, default_block, case_literal_and_labels, case_blocks, selection_control);

			return expect('}') && parse_success;
		}

		if (accept(tokenid::for_))
		{
			if (!expect('('))
				return false;

			scope_guard _(
				[this]() { enter_scope(); },
				[this]() { leave_scope(); });

			// Parse initializer first
			if (type type = {}; parse_type(type))
			{
				unsigned int count = 0;
				do
				{
					// There may be multiple declarations behind a type, so loop through them
					if (count++ > 0 && !expect(','))
						return false;

					if (!expect(tokenid::identifier) || !parse_variable(type, std::move(_token.literal_as_string)))
						return false;
				}
				while (!peek(';'));
			}
			else
			{
				// Initializer can also contain an expression if not a variable declaration list and not empty
				if (!peek(';'))
				{
					expression initializer_exp;
					if (!parse_expression(initializer_exp))
						return false;
				}
			}

			if (!expect(';'))
				return false;

			const codegen::id merge_block = _codegen->create_block(); // Block that is executed after the loop
			const codegen::id header_label = _codegen->create_block(); // Pointer to the loop merge instruction
			const codegen::id continue_label = _codegen->create_block(); // Pointer to the continue block
			codegen::id loop_block = _codegen->create_block(); // Pointer to the main loop body block
			codegen::id condition_block = _codegen->create_block(); // Pointer to the condition check
			codegen::id condition_value = 0;

			// End current block by branching to the next label
			const codegen::id prev_block = _codegen->leave_block_and_branch(header_label);

			{ // Begin loop block (this header is used for explicit structured control flow)
				_codegen->enter_block(header_label);

				_codegen->leave_block_and_branch(condition_block);
			}

			{ // Parse condition block
				_codegen->enter_block(condition_block);

				if (!peek(';'))
				{
					expression condition_exp;
					if (!parse_expression(condition_exp))
						return false;

					if (!condition_exp.type.is_scalar())
					{
						error(condition_exp.location, 3019, "scalar value expected");
						return false;
					}

					// Evaluate condition and branch to the right target
					condition_exp.add_cast_operation({ type::t_bool, 1, 1 });

					condition_value = _codegen->emit_load(condition_exp);
					condition_block = _codegen->leave_block_and_branch_conditional(condition_value, loop_block, merge_block);
				}
				else // It is valid for there to be no condition expression
				{
					condition_block = _codegen->leave_block_and_branch(loop_block);
				}

				if (!expect(';'))
					return false;
			}

			{ // Parse loop continue block into separate block so it can be appended to the end down the line
				_codegen->enter_block(continue_label);

				if (!peek(')'))
				{
					expression continue_exp;
					if (!parse_expression(continue_exp))
						return false;
				}

				if (!expect(')'))
					return false;

				// Branch back to the loop header at the end of the continue block
				_codegen->leave_block_and_branch(header_label);
			}

			{ // Parse loop body block
				_codegen->enter_block(loop_block);

				_loop_break_target_stack.push_back(merge_block);
				_loop_continue_target_stack.push_back(continue_label);

				const bool parse_success = parse_statement(false);

				_loop_break_target_stack.pop_back();
				_loop_continue_target_stack.pop_back();

				if (!parse_success)
					return false;

				loop_block = _codegen->leave_block_and_branch(continue_label);
			}

			// Add merge block label to the end of the loop
			_codegen->enter_block(merge_block);

			// Emit structured control flow for a loop statement and connect all basic blocks
			_codegen->emit_loop(statement_location, condition_value, prev_block, header_label, condition_block, loop_block, continue_label, loop_control);

			return true;
		}

		if (accept(tokenid::while_))
		{
			scope_guard _(
				[this]() { enter_scope(); },
				[this]() { leave_scope(); });

			const codegen::id merge_block = _codegen->create_block();
			const codegen::id header_label = _codegen->create_block();
			const codegen::id continue_label = _codegen->create_block();
			codegen::id loop_block = _codegen->create_block();
			codegen::id condition_block = _codegen->create_block();
			codegen::id condition_value = 0;

			// End current block by branching to the next label
			const codegen::id prev_block = _codegen->leave_block_and_branch(header_label);

			{ // Begin loop block
				_codegen->enter_block(header_label);

				_codegen->leave_block_and_branch(condition_block);
			}

			{ // Parse condition block
				_codegen->enter_block(condition_block);

				expression condition_exp;
				if (!expect('(') || !parse_expression(condition_exp) || !expect(')'))
					return false;

				if (!condition_exp.type.is_scalar())
				{
					error(condition_exp.location, 3019, "scalar value expected");
					return false;
				}

				// Evaluate condition and branch to the right target
				condition_exp.add_cast_operation({ type::t_bool, 1, 1 });

				condition_value = _codegen->emit_load(condition_exp);
				condition_block = _codegen->leave_block_and_branch_conditional(condition_value, loop_block, merge_block);
			}

			{ // Parse loop body block
				_codegen->enter_block(loop_block);

				_loop_break_target_stack.push_back(merge_block);
				_loop_continue_target_stack.push_back(continue_label);

				const bool parse_success = parse_statement(false);

				_loop_break_target_stack.pop_back();
				_loop_continue_target_stack.pop_back();

				if (!parse_success)
					return false;

				loop_block = _codegen->leave_block_and_branch(continue_label);
			}

			{ // Branch back to the loop header in empty continue block
				_codegen->enter_block(continue_label);

				_codegen->leave_block_and_branch(header_label);
			}

			// Add merge block label to the end of the loop
			_codegen->enter_block(merge_block);

			// Emit structured control flow for a loop statement and connect all basic blocks
			_codegen->emit_loop(statement_location, condition_value, prev_block, header_label, condition_block, loop_block, continue_label, loop_control);

			return true;
		}

		if (accept(tokenid::do_))
		{
			const codegen::id merge_block = _codegen->create_block();
			const codegen::id header_label = _codegen->create_block();
			const codegen::id continue_label = _codegen->create_block();
			codegen::id loop_block = _codegen->create_block();
			codegen::id condition_value = 0;

			// End current block by branching to the next label
			const codegen::id prev_block = _codegen->leave_block_and_branch(header_label);

			{ // Begin loop block
				_codegen->enter_block(header_label);

				_codegen->leave_block_and_branch(loop_block);
			}

			{ // Parse loop body block
				_codegen->enter_block(loop_block);

				_loop_break_target_stack.push_back(merge_block);
				_loop_continue_target_stack.push_back(continue_label);

				const bool parse_success = parse_statement(true);

				_loop_break_target_stack.pop_back();
				_loop_continue_target_stack.pop_back();

				if (!parse_success)
					return false;

				loop_block = _codegen->leave_block_and_branch(continue_label);
			}

			{ // Continue block does the condition evaluation
				_codegen->enter_block(continue_label);

				expression condition_exp;
				if (!expect(tokenid::while_) || !expect('(') || !parse_expression(condition_exp) || !expect(')') || !expect(';'))
					return false;

				if (!condition_exp.type.is_scalar())
				{
					error(condition_exp.location, 3019, "scalar value expected");
					return false;
				}

				// Evaluate condition and branch to the right target
				condition_exp.add_cast_operation({ type::t_bool, 1, 1 });

				condition_value = _codegen->emit_load(condition_exp);

				_codegen->leave_block_and_branch_conditional(condition_value, header_label, merge_block);
			}

			// Add merge block label to the end of the loop
			_codegen->enter_block(merge_block);

			// Emit structured control flow for a loop statement and connect all basic blocks
			_codegen->emit_loop(statement_location, condition_value, prev_block, header_label, 0, loop_block, continue_label, loop_control);

			return true;
		}

		if (accept(tokenid::break_))
		{
			if (_loop_break_target_stack.empty())
			{
				error(statement_location, 3518, "break must be inside loop");
				return false;
			}

			// Branch to the break target of the inner most loop on the stack
			_codegen->leave_block_and_branch(_loop_break_target_stack.back(), 1);

			return expect(';');
		}

		if (accept(tokenid::continue_))
		{
			if (_loop_continue_target_stack.empty())
			{
				error(statement_location, 3519, "continue must be inside loop");
				return false;
			}

			// Branch to the continue target of the inner most loop on the stack
			_codegen->leave_block_and_branch(_loop_continue_target_stack.back(), 2);

			return expect(';');
		}

		if (accept(tokenid::return_))
		{
			const type &return_type = _codegen->_current_function->return_type;

			if (!peek(';'))
			{
				expression return_exp;
				if (!parse_expression(return_exp))
				{
					consume_until(';');
					return false;
				}

				// Cannot return to void
				if (return_type.is_void())
				{
					error(statement_location, 3079, "void functions cannot return a value");
					// Consume the semicolon that follows the return expression so that parsing may continue
					accept(';');
					return false;
				}

				// Cannot return arrays from a function
				if (return_exp.type.is_array() || !type::rank(return_exp.type, return_type))
				{
					error(statement_location, 3017, "expression (" + return_exp.type.description() + ") does not match function return type (" + return_type.description() + ')');
					accept(';');
					return false;
				}

				// Load return value and perform implicit cast to function return type
				if (return_exp.type.components() > return_type.components())
					warning(return_exp.location, 3206, "implicit truncation of vector type");

				return_exp.add_cast_operation(return_type);

				const codegen::id return_value = _codegen->emit_load(return_exp);

				_codegen->leave_block_and_return(return_value);
			}
			else if (!return_type.is_void())
			{
				// No return value was found, but the function expects one
				error(statement_location, 3080, "function must return a value");

				// Consume the semicolon that follows the return expression so that parsing may continue
				accept(';');

				return false;
			}
			else
			{
				_codegen->leave_block_and_return();
			}

			return expect(';');
		}

		if (accept(tokenid::discard_))
		{
			// Leave the current function block
			_codegen->leave_block_and_kill();

			return expect(';');
		}
	}

	// Handle variable declarations
	if (type type = {}; parse_type(type))
	{
		unsigned int count = 0;
		do
		{
			// There may be multiple declarations behind a type, so loop through them
			if (count++ > 0 && !expect(','))
			{
				// Try to consume the rest of the declaration so that parsing may continue despite the error
				consume_until(';');
				return false;
			}

			if (!expect(tokenid::identifier) || !parse_variable(type, std::move(_token.literal_as_string)))
			{
				consume_until(';');
				return false;
			}
		}
		while (!peek(';'));

		return expect(';');
	}

	// Handle expression statements
	expression statement_exp;
	if (parse_expression(statement_exp))
		return expect(';'); // A statement has to be terminated with a semicolon

	// Gracefully consume any remaining characters until the statement would usually end, so that parsing may continue despite the error
	consume_until(';');

	return false;
}
bool reshadefx::parser::parse_statement_block(bool scoped)
{
	if (!expect('{'))
		return false;

	if (scoped)
		enter_scope();

	// Parse statements until the end of the block is reached
	while (!peek('}') && !peek(tokenid::end_of_file))
	{
		if (!parse_statement(true))
		{
			if (scoped)
				leave_scope();

			// Ignore the rest of this block
			unsigned int level = 0;

			while (!peek(tokenid::end_of_file))
			{
				if (accept('{'))
				{
					++level;
				}
				else if (accept('}'))
				{
					if (level-- == 0)
						break;
				} // These braces are necessary to match the 'else' to the correct 'if' statement
				else
				{
					consume();
				}
			}

			return false;
		}
	}

	if (scoped)
		leave_scope();

	return expect('}');
}

bool reshadefx::parser::parse_type(type &type)
{
	type.qualifiers = 0;
	accept_type_qualifiers(type);

	if (!accept_type_class(type))
		return false;

	if (type.is_integral() && (type.has(type::q_centroid) || type.has(type::q_noperspective)))
	{
		error(_token.location, 4576, "signature specifies invalid interpolation mode for integer component type");
		return false;
	}

	if (type.has(type::q_centroid) && !type.has(type::q_noperspective))
		type.qualifiers |= type::q_linear;

	return true;
}
bool reshadefx::parser::parse_array_length(type &type)
{
	// Reset array length to zero before checking if one exists
	type.array_length = 0;

	if (accept('['))
	{
		if (accept(']'))
		{
			// No length expression, so this is an unbounded array
			type.array_length = 0xFFFFFFFF;
		}
		else if (expression length_exp; parse_expression(length_exp) && expect(']'))
		{
			if (!length_exp.is_constant || !(length_exp.type.is_scalar() && length_exp.type.is_integral()))
			{
				error(length_exp.location, 3058, "array dimensions must be literal scalar expressions");
				return false;
			}

			type.array_length = length_exp.constant.as_uint[0];

			if (type.array_length < 1 || type.array_length > 65536)
			{
				error(length_exp.location, 3059, "array dimension must be between 1 and 65536");
				return false;
			}
		}
		else
		{
			return false;
		}
	}

	// Multi-dimensional arrays are not supported
	if (peek('['))
	{
		error(_token_next.location, 3119, "arrays cannot be multi-dimensional");
		return false;
	}

	return true;
}

bool reshadefx::parser::parse_annotations(std::vector<annotation> &annotations)
{
	// Check if annotations exist and return early if none do
	if (!accept('<'))
		return true;

	bool parse_success = true;

	while (!peek('>'))
	{
		if (type type /* = {} */; accept_type_class(type))
			warning(_token.location, 4717, "type prefixes for annotations are deprecated and ignored");

		if (!expect(tokenid::identifier))
		{
			consume_until('>');
			return false;
		}

		std::string name = std::move(_token.literal_as_string);

		expression annotation_exp;
		if (!expect('=') || !parse_expression_multary(annotation_exp) || !expect(';'))
		{
			consume_until('>');
			return false;
		}

		if (annotation_exp.is_constant)
		{
			annotations.push_back({ annotation_exp.type, std::move(name), std::move(annotation_exp.constant) });
		}
		else // Continue parsing annotations despite this not being a constant, since the syntax is still correct
		{
			parse_success = false;
			error(annotation_exp.location, 3011, "value must be a literal expression");
		}
	}

	return expect('>') && parse_success;
}

bool reshadefx::parser::parse_struct()
{
	const location struct_location = std::move(_token.location);

	struct_type info;
	// The structure name is optional
	if (accept(tokenid::identifier))
		info.name = std::move(_token.literal_as_string);
	else
		info.name = "_anonymous_struct_" + std::to_string(struct_location.line) + '_' + std::to_string(struct_location.column);

	info.unique_name = 'S' + current_scope().name + info.name;
	std::replace(info.unique_name.begin(), info.unique_name.end(), ':', '_');

	if (!expect('{'))
		return false;

	bool parse_success = true;

	while (!peek('}')) // Empty structures are possible
	{
		member_type member;

		if (!parse_type(member.type))
		{
			error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected struct member type");
			consume_until('}');
			accept(';');
			return false;
		}

		unsigned int count = 0;
		do
		{
			if ((count++ > 0 && !expect(',')) || !expect(tokenid::identifier))
			{
				consume_until('}');
				accept(';');
				return false;
			}

			member.name = std::move(_token.literal_as_string);
			member.location = std::move(_token.location);

			if (member.type.is_void())
			{
				parse_success = false;
				error(member.location, 3038, '\'' + member.name + "': struct members cannot be void");
			}
			if (member.type.is_struct()) // Nesting structures would make input/output argument flattening more complicated, so prevent it for now
			{
				parse_success = false;
				error(member.location, 3090, '\'' + member.name + "': nested struct members are not supported");
			}

			if (member.type.has(type::q_in) || member.type.has(type::q_out))
			{
				parse_success = false;
				error(member.location, 3055, '\'' + member.name + "': struct members cannot be declared 'in' or 'out'");
			}
			if (member.type.has(type::q_const))
			{
				parse_success = false;
				error(member.location, 3035, '\'' + member.name + "': struct members cannot be declared 'const'");
			}
			if (member.type.has(type::q_extern))
			{
				parse_success = false;
				error(member.location, 3006, '\'' + member.name + "': struct members cannot be declared 'extern'");
			}
			if (member.type.has(type::q_static))
			{
				parse_success = false;
				error(member.location, 3007, '\'' + member.name + "': struct members cannot be declared 'static'");
			}
			if (member.type.has(type::q_uniform))
			{
				parse_success = false;
				error(member.location, 3047, '\'' + member.name + "': struct members cannot be declared 'uniform'");
			}
			if (member.type.has(type::q_groupshared))
			{
				parse_success = false;
				error(member.location, 3010, '\'' + member.name + "': struct members cannot be declared 'groupshared'");
			}

			// Modify member specific type, so that following members in the declaration list are not affected by this
			if (!parse_array_length(member.type))
			{
				consume_until('}');
				accept(';');
				return false;
			}

			if (member.type.is_unbounded_array())
			{
				parse_success = false;
				error(member.location, 3072, '\'' + member.name + "': array dimensions of struct members must be explicit");
			}

			// Structure members may have semantics to use them as input/output types
			if (accept(':'))
			{
				if (!expect(tokenid::identifier))
				{
					consume_until('}');
					accept(';');
					return false;
				}

				member.semantic = std::move(_token.literal_as_string);
				// Make semantic upper case to simplify comparison later on
				std::transform(member.semantic.begin(), member.semantic.end(), member.semantic.begin(),
					[](std::string::value_type c) {
						return static_cast<std::string::value_type>(std::toupper(c));
					});

				if (member.semantic.compare(0, 3, "SV_") != 0)
				{
					// Always numerate semantics, so that e.g. TEXCOORD and TEXCOORD0 point to the same location
					if (const char c = member.semantic.back(); c < '0' || c > '9')
						member.semantic += '0';

					if (member.type.is_integral() && !member.type.has(type::q_nointerpolation))
					{
						member.type.qualifiers |= type::q_nointerpolation; // Integer fields do not interpolate, so make this explicit (to avoid issues with GLSL)
						warning(member.location, 4568, '\'' + member.name + "': integer fields have the 'nointerpolation' qualifier by default");
					}
				}
				else
				{
					// Remove optional trailing zero from system value semantics, so that e.g. SV_POSITION and SV_POSITION0 mean the same thing
					if (member.semantic.back() == '0' && (member.semantic[member.semantic.size() - 2] < '0' || member.semantic[member.semantic.size() - 2] > '9'))
						member.semantic.pop_back();
				}
			}

			// Save member name and type for bookkeeping
			info.member_list.push_back(member);
		}
		while (!peek(';'));

		if (!expect(';'))
		{
			consume_until('}');
			accept(';');
			return false;
		}
	}

	// Empty structures are valid, but not usually intended, so emit a warning
	if (info.member_list.empty())
		warning(struct_location, 5001, "struct has no members");

	// Define the structure now that information about all the member types was gathered
	const codegen::id id = _codegen->define_struct(struct_location, info);

	// Insert the symbol into the symbol table
	symbol symbol = { symbol_type::structure, id };

	if (!insert_symbol(info.name, symbol, true))
	{
		error(struct_location, 3003, "redefinition of '" + info.name + '\'');
		return false;
	}

	return expect('}') && parse_success;
}

bool reshadefx::parser::parse_function(type type, std::string name, shader_type stype, int num_threads[3])
{
	const location function_location = std::move(_token.location);

	if (!expect('(')) // Functions always have a parameter list
		return false;

	if (type.qualifiers != 0)
	{
		error(function_location, 3047, '\'' + name + "': function return type cannot have any qualifiers");
		return false;
	}

	function info;
	info.name = name;
	info.unique_name = 'F' + current_scope().name + name;
	std::replace(info.unique_name.begin(), info.unique_name.end(), ':', '_');

	info.return_type = type;
	info.type = stype;
	info.num_threads[0] = num_threads[0];
	info.num_threads[1] = num_threads[1];
	info.num_threads[2] = num_threads[2];

	_codegen->_current_function = &info;

	bool parse_success = true;
	bool expect_parenthesis = true;

	// sopt: GLSL's f(void)
	if (sopt_glsl && peek(tokenid::void_))
	{
		backup();
		consume();
		if (!peek(')'))
			restore();
	}

	// Enter function scope (and leave it again when parsing this function finished)
	scope_guard _(
		[this]() {
			enter_scope();
		},
		[this]() {
			leave_scope();
			_codegen->_current_function = nullptr;
		});

	while (!peek(')'))
	{
		if (!info.parameter_list.empty() && !expect(','))
		{
			parse_success = false;
			expect_parenthesis = false;
			consume_until(')');
			break;
		}

		member_type param;

		if (!parse_type(param.type))
		{
			error(_token_next.location, 3000, "syntax error: unexpected '" + token::id_to_name(_token_next.id) + "', expected parameter type");
			parse_success = false;
			expect_parenthesis = false;
			consume_until(')');
			break;
		}

		if (!expect(tokenid::identifier))
		{
			parse_success = false;
			expect_parenthesis = false;
			consume_until(')');
			break;
		}

		param.name = std::move(_token.literal_as_string);
		param.location = std::move(_token.location);

		if (param.type.is_void())
		{
			parse_success = false;
			error(param.location, 3038, '\'' + param.name + "': function parameters cannot be void");
		}

		if (param.type.has(type::q_extern))
		{
			parse_success = false;
			error(param.location, 3006, '\'' + param.name + "': function parameters cannot be declared 'extern'");
		}
		if (param.type.has(type::q_static))
		{
			parse_success = false;
			error(param.location, 3007, '\'' + param.name + "': function parameters cannot be declared 'static'");
		}
		if (param.type.has(type::q_uniform))
		{
			parse_success = false;
			error(param.location, 3047, '\'' + param.name + "': function parameters cannot be declared 'uniform', consider placing in global scope instead");
		}
		if (param.type.has(type::q_groupshared))
		{
			parse_success = false;
			error(param.location, 3010, '\'' + param.name + "': function parameters cannot be declared 'groupshared'");
		}

		if (param.type.has(type::q_out) && param.type.has(type::q_const))
		{
			parse_success = false;
			error(param.location, 3046, '\'' + param.name + "': output parameters cannot be declared 'const'");
		}
		else if (!param.type.has(type::q_out))
		{
			// Function parameters are implicitly 'in' if not explicitly defined as 'out'
			param.type.qualifiers |= type::q_in;
		}

		if (!parse_array_length(param.type))
		{
			parse_success = false;
			expect_parenthesis = false;
			consume_until(')');
			break;
		}

		if (param.type.is_unbounded_array())
		{
			parse_success = false;
			error(param.location, 3072, '\'' + param.name + "': array dimensions of function parameters must be explicit");
			param.type.array_length = 0;
		}

		// Handle parameter type semantic
		if (accept(':'))
		{
			if (!expect(tokenid::identifier))
			{
				parse_success = false;
				expect_parenthesis = false;
				consume_until(')');
				break;
			}

			param.semantic = std::move(_token.literal_as_string);
			// Make semantic upper case to simplify comparison later on
			std::transform(param.semantic.begin(), param.semantic.end(), param.semantic.begin(),
				[](std::string::value_type c) {
					return static_cast<std::string::value_type>(std::toupper(c));
				});

			if (param.semantic.compare(0, 3, "SV_") != 0)
			{
				// Always numerate semantics, so that e.g. TEXCOORD and TEXCOORD0 point to the same location
				if (const char c = param.semantic.back(); c < '0' || c > '9')
					param.semantic += '0';

				if (param.type.is_integral() && !param.type.has(type::q_nointerpolation))
				{
					param.type.qualifiers |= type::q_nointerpolation; // Integer parameters do not interpolate, so make this explicit (to avoid issues with GLSL)
					warning(param.location, 4568, '\'' + param.name + "': integer parameters have the 'nointerpolation' qualifier by default");
				}
			}
			else
			{
				// Remove optional trailing zero from system value semantics, so that e.g. SV_POSITION and SV_POSITION0 mean the same thing
				if (param.semantic.back() == '0' && (param.semantic[param.semantic.size() - 2] < '0' || param.semantic[param.semantic.size() - 2] > '9'))
					param.semantic.pop_back();
			}
		}

		// Handle default argument
		if (accept('='))
		{
			expression default_value_exp;
			if (!parse_expression_multary(default_value_exp))
			{
				parse_success = false;
				expect_parenthesis = false;
				consume_until(')');
				break;
			}

			default_value_exp.add_cast_operation(param.type);

			if (!default_value_exp.is_constant)
			{
				parse_success = false;
				error(default_value_exp.location, 3011, '\'' + param.name + "': value must be a literal expression");
			}

			param.default_value = std::move(default_value_exp.constant);
			param.has_default_value = true;
		}
		else
		{
			if (!info.parameter_list.empty() && info.parameter_list.back().has_default_value)
			{
				parse_success = false;
				error(param.location, 3044, '\'' + name + "': missing default value for parameter '" + param.name + '\'');
			}
		}

		info.parameter_list.push_back(std::move(param));
	}

	if (expect_parenthesis && !expect(')'))
		return false;

	// sopt: the GLSL entry point's global in / out variables (and gl_FragCoord) as its parameters
	if (sopt_glsl && name == sopt_hlsl_entry && stype != shader_type::unknown)
	{
		bool has_out = false;
		for (const sopt_glsl_io &io : _sopt_glsl_io)
		{
			member_type param;
			param.name = io.name;
			param.location = io.loc;
			if (!sopt_glsl_type(io.type_text, param.type))
			{
				reshadefx::type t = {};
				if (io.type_text == "float") t = { type::t_float, 1, 1 };
				else if (io.type_text == "int") t = { type::t_int, 1, 1 };
				else if (io.type_text == "uint") t = { type::t_uint, 1, 1 };
				else if (io.type_text == "bool") t = { type::t_bool, 1, 1 };
				else
				{
					error(io.loc, 3000, "sopt: unsupported GLSL " + std::string(io.out ? "output" : "input") + " type '" + io.type_text + '\'');
					return false;
				}
				param.type = t;
			}
			param.type.qualifiers = (io.out ? type::q_out : type::q_in) | io.interpolation;
			if (param.type.is_integral())
				param.type.qualifiers |= type::q_nointerpolation;
			param.semantic = io.semantic;
			has_out = has_out || io.out;
			info.parameter_list.push_back(std::move(param));
		}
		member_type frag_coord;
		frag_coord.name = "gl_FragCoord";
		frag_coord.location = function_location;
		frag_coord.type = { type::t_float, 4, 1, type::q_in };
		frag_coord.semantic = "SV_POSITION";
		info.parameter_list.push_back(std::move(frag_coord));
		if (!has_out)
		{
			member_type frag_color;
			frag_color.name = "gl_FragColor";
			frag_color.location = function_location;
			frag_color.type = { type::t_float, 4, 1, type::q_out };
			frag_color.semantic = "SV_TARGET";
			info.parameter_list.push_back(std::move(frag_color));
		}
	}

	// Handle return type semantic
	if (accept(':'))
	{
		if (!expect(tokenid::identifier))
			return false;

		if (type.is_void())
		{
			error(_token.location, 3076, '\'' + name + "': void function cannot have a semantic");
			return false;
		}

		info.return_semantic = std::move(_token.literal_as_string);
		// Make semantic upper case to simplify comparison later on
		std::transform(info.return_semantic.begin(), info.return_semantic.end(), info.return_semantic.begin(),
			[](std::string::value_type c) {
				return static_cast<std::string::value_type>(std::toupper(c));
			});
	}

	// Check if this is a function declaration without a body
	if (accept(';'))
	{
		error(function_location, 3510, '\'' + name + "': function is missing an implementation");
		return false;
	}

	// Define the function now that information about the declaration was gathered
	const codegen::id id = _codegen->define_function(function_location, info);

	// Insert the function and parameter symbols into the symbol table and update current function pointer to the permanent one
	symbol symbol = { symbol_type::function, id, { type::t_function } };
	symbol.function = &_codegen->get_function(id);

	if (!insert_symbol(name, symbol, true))
	{
		_codegen->leave_function();

		error(function_location, 3003, "redefinition of '" + name + '\'');
		return false;
	}

	for (const member_type &param : info.parameter_list)
	{
		if (!insert_symbol(param.name, { symbol_type::variable, param.id, param.type }))
		{
			_codegen->leave_function();

			error(param.location, 3003, "redefinition of '" + param.name + '\'');
			return false;
		}
	}

	// A function has to start with a new block
	_codegen->enter_block(_codegen->create_block());

	if (!parse_statement_block(false))
		parse_success = false;

	// Add implicit return statement to the end of functions
	if (_codegen->is_in_block())
		_codegen->leave_block_and_return();

	_codegen->leave_function();

	return parse_success;
}

bool reshadefx::parser::parse_variable(type type, std::string name, bool global)
{
	const location variable_location = std::move(_token.location);
	bool sopt_named = false; // sopt: becomes a named expression (see sopt_named_expressions)

	if (type.is_void())
	{
		error(variable_location, 3038, '\'' + name + "': variables cannot be void");
		return false;
	}
	if (type.has(type::q_in) || type.has(type::q_out))
	{
		error(variable_location, 3055, '\'' + name + "': variables cannot be declared 'in' or 'out'");
		return false;
	}

	// Local and global variables have different requirements
	if (global)
	{
		// Check that type qualifier combinations are valid
		if (type.has(type::q_static))
		{
			// Global variables that are 'static' cannot be of another storage class
			if (type.has(type::q_uniform))
			{
				error(variable_location, 3007, '\'' + name + "': uniform global variables cannot be declared 'static'");
				return false;
			}
			// The 'volatile' qualifier is only valid memory object declarations that are storage images or uniform blocks
			if (type.has(type::q_volatile))
			{
				error(variable_location, 3008, '\'' + name + "': global variables cannot be declared 'volatile'");
				return false;
			}
		}
		else if (!type.has(type::q_groupshared))
		{
			// Make all global variables 'uniform' by default, since they should be externally visible without the 'static' keyword
			if (!type.has(type::q_uniform) && !type.is_object())
				warning(variable_location, 5000, '\'' + name + "': global variables are considered 'uniform' by default");

			// Global variables that are not 'static' are always 'extern' and 'uniform'
			type.qualifiers |= type::q_extern | type::q_uniform;

			// It is invalid to make 'uniform' variables constant, since they can be modified externally
			if (type.has(type::q_const))
			{
				error(variable_location, 3035, '\'' + name + "': variables which are 'uniform' cannot be declared 'const'");
				return false;
			}
		}
	}
	else
	{
		// Static does not really have meaning on local variables
		if (type.has(type::q_static))
			type.qualifiers &= ~type::q_static;

		if (type.has(type::q_extern))
		{
			error(variable_location, 3006, '\'' + name + "': local variables cannot be declared 'extern'");
			return false;
		}
		if (type.has(type::q_uniform))
		{
			error(variable_location, 3047, '\'' + name + "': local variables cannot be declared 'uniform'");
			return false;
		}
		if (type.has(type::q_groupshared))
		{
			error(variable_location, 3010, '\'' + name + "': local variables cannot be declared 'groupshared'");
			return false;
		}

		if (type.is_object())
		{
			error(variable_location, 3038, '\'' + name + "': local variables cannot be texture, sampler or storage objects");
			return false;
		}
	}

	// The variable name may be followed by an optional array size expression
	if (!parse_array_length(type))
		return false;

	bool parse_success = true;
	expression initializer;
	texture texture_info;
	sampler sampler_info;
	storage storage_info;

	// sopt: HLSL register(...) / packoffset(...) annotations are skipped
	bool sopt_annotation = false;
	if (sopt_hlsl && peek(':'))
	{
		backup();
		consume();
		if ((peek(tokenid::identifier) || peek(tokenid::reserved)) &&
			(_token_next.literal_as_string == "register" || _token_next.literal_as_string == "packoffset"))
		{
			consume();
			if (!sopt_skip_parens())
				return false;
			sopt_annotation = true;
		}
		else
		{
			restore();
		}
	}

	if (!sopt_annotation && accept(':'))
	{
		if (!expect(tokenid::identifier))
			return false;

		if (!global) // Only global variables can have a semantic
		{
			error(_token.location, 3043, '\'' + name + "': local variables cannot have semantics");
			return false;
		}

		std::string &semantic = texture_info.semantic;
		semantic = std::move(_token.literal_as_string);

		// Make semantic upper case to simplify comparison later on
		std::transform(semantic.begin(), semantic.end(), semantic.begin(),
			[](std::string::value_type c) {
				return static_cast<std::string::value_type>(std::toupper(c));
			});
	}
	else
	{
		// Global variables can have optional annotations
		if (global && !parse_annotations(sampler_info.annotations))
			parse_success = false;

		// Variables without a semantic may have an optional initializer
		if (accept('='))
		{
			// The precise qualifier propagates to operations that contribute to the value assigned to the qualified variable
			if (type.has(type::q_precise))
				initializer.type.qualifiers |= type::q_precise;

			const size_t sopt_begin = _token_next.offset;
			const location sopt_location = _token_next.location;

			if (!parse_expression_assignment(initializer))
				return false;

			if (type.has(type::q_groupshared))
			{
				error(initializer.location, 3009, '\'' + name + "': variables declared 'groupshared' cannot have an initializer");
				return false;
			}

			// TODO: This could be resolved by initializing these at the beginning of the entry point
			if (global && !initializer.is_constant)
			{
				// sopt: a named expression instead (see sopt_named_expressions)
				if (sopt_named_expressions && type.has(type::q_static) && type.has(type::q_const) && type.is_numeric() && !type.is_array())
				{
					sopt_named = true;
					_sopt_named.emplace_back(_lexer->input_string().substr(sopt_begin, _token.offset + _token.length - sopt_begin), sopt_location);
				}
				else
				{
					error(initializer.location, 3011, '\'' + name + "': initial value must be a literal expression");
					return false;
				}
			}

			// Check type compatibility
			if ((!type.is_unbounded_array() && initializer.type.array_length != type.array_length) || !type::rank(initializer.type, type))
			{
				error(initializer.location, 3017, '\'' + name + "': initial value (" + initializer.type.description() + ") does not match variable type (" + type.description() + ')');
				return false;
			}
			if ((initializer.type.rows < type.rows || initializer.type.cols < type.cols) && !initializer.type.is_scalar())
			{
				error(initializer.location, 3017, '\'' + name + "': cannot implicitly convert these vector types (from " + initializer.type.description() + " to " + type.description() + ')');
				return false;
			}

			// Deduce array size from the initializer expression
			if (initializer.type.is_array())
				type.array_length = initializer.type.array_length;

			// Perform implicit cast from initializer expression to variable type
			if (initializer.type.components() > type.components())
				warning(initializer.location, 3206, "implicit truncation of vector type");

			initializer.add_cast_operation(type);

			if (type.has(type::q_static))
				initializer.type.qualifiers |= type::q_static;
		}
		else if (type.is_numeric() || type.is_struct()) // Numeric variables without an initializer need special handling
		{
			if (type.has(type::q_const)) // Constants have to have an initial value
			{
				error(variable_location, 3012, '\'' + name + "': missing initial value");
				return false;
			}

			if (!type.has(type::q_uniform)) // Zero initialize all global variables
				initializer.reset_to_rvalue_constant(variable_location, {}, type);
		}
		else if (global && accept('{')) // Textures and samplers can have a property block attached to their declaration
		{
			// Non-numeric variables cannot be constants
			if (type.has(type::q_const))
			{
				error(variable_location, 3035, '\'' + name + "': this variable type cannot be declared 'const'");
				return false;
			}

			// Default texture format to RGBA8 in case it is not specified
			texture_info.format = texture_format::rgba8;
			// Integral texture formats cannot use linear filtering, so default to point filtering for them
			sampler_info.filter = type.is_integral() ? filter_mode::min_mag_mip_point : filter_mode::min_mag_mip_linear;

			while (!peek('}'))
			{
				if (!expect(tokenid::identifier))
				{
					consume_until('}');
					return false;
				}

				location property_location = std::move(_token.location);
				const std::string property_name = std::move(_token.literal_as_string);

				if (!expect('='))
				{
					consume_until('}');
					return false;
				}

				backup();

				expression property_exp;

				if (accept(tokenid::identifier)) // Handle special enumeration names for property values
				{
					// Transform identifier to uppercase to do case-insensitive comparison
					std::transform(_token.literal_as_string.begin(), _token.literal_as_string.end(), _token.literal_as_string.begin(),
						[](std::string::value_type c) {
							return static_cast<std::string::value_type>(std::toupper(c));
						});

					static const std::unordered_map<std::string_view, uint32_t> s_enum_values = {
						{ "NONE", 0 }, { "POINT", 0 },
						{ "LINEAR", 1 },
						{ "ANISOTROPIC", 0x55 },
						{ "WRAP", uint32_t(texture_address_mode::wrap) }, { "REPEAT", uint32_t(texture_address_mode::wrap) },
						{ "MIRROR", uint32_t(texture_address_mode::mirror) },
						{ "CLAMP", uint32_t(texture_address_mode::clamp) },
						{ "BORDER", uint32_t(texture_address_mode::border) },
						{ "R8", uint32_t(texture_format::r8) },
						{ "R16", uint32_t(texture_format::r16) },
						{ "R16F", uint32_t(texture_format::r16f) },
						{ "R32I", uint32_t(texture_format::r32i) },
						{ "R32U", uint32_t(texture_format::r32u) },
						{ "R32F", uint32_t(texture_format::r32f) },
						{ "RG8", uint32_t(texture_format::rg8) }, { "R8G8", uint32_t(texture_format::rg8) },
						{ "RG16", uint32_t(texture_format::rg16) }, { "R16G16", uint32_t(texture_format::rg16) },
						{ "RG16F", uint32_t(texture_format::rg16f) }, { "R16G16F", uint32_t(texture_format::rg16f) },
						{ "RG32F", uint32_t(texture_format::rg32f) }, { "R32G32F", uint32_t(texture_format::rg32f) },
						{ "RGBA8", uint32_t(texture_format::rgba8) }, { "R8G8B8A8", uint32_t(texture_format::rgba8) },
						{ "RGBA16", uint32_t(texture_format::rgba16) }, { "R16G16B16A16", uint32_t(texture_format::rgba16) },
						{ "RGBA16F", uint32_t(texture_format::rgba16f) }, { "R16G16B16A16F", uint32_t(texture_format::rgba16f) },
						{ "RGBA32I", uint32_t(texture_format::rgba32i) }, { "R32G32B32A32I", uint32_t(texture_format::rgba32i) },
						{ "RGBA32U", uint32_t(texture_format::rgba32u) }, { "R32G32B32A32U", uint32_t(texture_format::rgba32u) },
						{ "RGBA32F", uint32_t(texture_format::rgba32f) }, { "R32G32B32A32F", uint32_t(texture_format::rgba32f) },
						{ "RGB10A2", uint32_t(texture_format::rgb10a2) }, { "R10G10B10A2", uint32_t(texture_format::rgb10a2) },
						{ "RG11B10F", uint32_t(texture_format::rg11b10f) }, { "R11G11B10F", uint32_t(texture_format::rg11b10f) },
					};

					// Look up identifier in list of possible enumeration names
					if (const auto it = s_enum_values.find(_token.literal_as_string);
						it != s_enum_values.end())
						property_exp.reset_to_rvalue_constant(_token.location, it->second);
					else // No match found, so rewind to parser state before the identifier was consumed and try parsing it as a normal expression
						restore();
				}

				// Parse right hand side as normal expression if no special enumeration name was matched already
				if (!property_exp.is_constant && !parse_expression_multary(property_exp))
				{
					consume_until('}');
					return false;
				}

				if (property_name == "Texture")
				{
					// Ignore invalid symbols that were added during error recovery
					if (property_exp.base == UINT32_MAX)
					{
						consume_until('}');
						return false;
					}

					if (!property_exp.type.is_texture())
					{
						error(property_exp.location, 3020, "type mismatch, expected texture name");
						consume_until('}');
						return false;
					}

					if (type.is_sampler() || type.is_storage())
					{
						texture &target_info = _codegen->get_texture(property_exp.base);
						if (type.is_storage())
							// Texture is used as storage
							target_info.storage_access = true;

						texture_info = target_info;
						sampler_info.texture_name = target_info.unique_name;
						storage_info.texture_name = target_info.unique_name;
					}
				}
				else
				{
					if (!property_exp.is_constant || !property_exp.type.is_scalar())
					{
						error(property_exp.location, 3538, "value must be a literal scalar expression");
						consume_until('}');
						return false;
					}

					// All states below expect the value to be of an integer type
					property_exp.add_cast_operation({ type::t_int, 1, 1 });
					const int value = property_exp.constant.as_int[0];

					if (value < 0) // There is little use for negative values, so warn in those cases
						warning(property_exp.location, 3571, "negative value specified for property '" + property_name + '\'');

					if (type.is_texture())
					{
						if (property_name == "Width")
							texture_info.width = value > 0 ? value : 1;
						else if (type.texture_dimension() >= 2 && property_name == "Height")
							texture_info.height = value > 0 ? value : 1;
						else if (type.texture_dimension() >= 3 && property_name == "Depth")
							texture_info.depth = value > 0 && value <= std::numeric_limits<uint16_t>::max() ? static_cast<uint16_t>(value) : 1;
						else if (property_name == "MipLevels")
							// Also ensures negative values do not cause problems
							texture_info.levels = value > 0 && value <= std::numeric_limits<uint16_t>::max() ? static_cast<uint16_t>(value) : 1;
						else if (property_name == "Format")
							texture_info.format = static_cast<texture_format>(value);
						else
							error(property_location, 3004, "unrecognized property '" + property_name + '\'');
					}
					else if (type.is_sampler())
					{
						if (property_name == "SRGBTexture" || property_name == "SRGBReadEnable")
							sampler_info.srgb = value != 0;
						else if (property_name == "AddressU")
							sampler_info.address_u = static_cast<texture_address_mode>(value);
						else if (property_name == "AddressV")
							sampler_info.address_v = static_cast<texture_address_mode>(value);
						else if (property_name == "AddressW")
							sampler_info.address_w = static_cast<texture_address_mode>(value);
						else if (property_name == "MinFilter")
							// Combine sampler filter components into a single filter enumeration value
							sampler_info.filter = static_cast<filter_mode>((uint32_t(sampler_info.filter) & 0x4F) | ((value & 0x03) << 4) | (value & 0x40));
						else if (property_name == "MagFilter")
							sampler_info.filter = static_cast<filter_mode>((uint32_t(sampler_info.filter) & 0x73) | ((value & 0x03) << 2) | (value & 0x40));
						else if (property_name == "MipFilter")
							sampler_info.filter = static_cast<filter_mode>((uint32_t(sampler_info.filter) & 0x7C) | ((value & 0x03)     ) | (value & 0x40));
						else if (property_name == "MinLOD" || property_name == "MaxMipLevel")
							sampler_info.min_lod = static_cast<float>(value);
						else if (property_name == "MaxLOD")
							sampler_info.max_lod = static_cast<float>(value);
						else if (property_name == "MipLODBias" || property_name == "MipMapLodBias")
							sampler_info.lod_bias = static_cast<float>(value);
						else
							error(property_location, 3004, "unrecognized property '" + property_name + '\'');
					}
					else if (type.is_storage())
					{
						if (property_name == "MipLOD" || property_name == "MipLevel")
							storage_info.level = value > 0 && value < std::numeric_limits<uint16_t>::max() ? static_cast<uint16_t>(value) : 0;
						else
							error(property_location, 3004, "unrecognized property '" + property_name + '\'');
					}
				}

				if (!expect(';'))
				{
					consume_until('}');
					return false;
				}
			}

			if (!expect('}'))
				return false;
		}
	}

	// At this point the array size should be known (either from the declaration or the initializer)
	if (type.is_unbounded_array())
	{
		error(variable_location, 3074, '\'' + name + "': implicit array missing initial value");
		return false;
	}

	symbol symbol;

	if (sopt_named)
	{
		symbol = { symbol_type::sopt_named, static_cast<uint32_t>(_sopt_named.size() - 1), type };
	}
	// Variables with a constant initializer and constant type are named constants
	// Skip this for very large arrays though, to avoid large amounts of duplicated values when that array constant is accessed with a dynamic index
	else if (type.is_numeric() && type.has(type::q_const) && initializer.is_constant && type.array_length < 100)
	{
		// Named constants are special symbols
		symbol = { symbol_type::constant, 0, type, initializer.constant };
	}
	else if (type.is_texture())
	{
		assert(global);

		texture_info.name = name;
		texture_info.type = static_cast<texture_type>(type.texture_dimension());

		// Add namespace scope to avoid name clashes
		texture_info.unique_name = 'V' + current_scope().name + name;
		std::replace(texture_info.unique_name.begin(), texture_info.unique_name.end(), ':', '_');

		texture_info.annotations = std::move(sampler_info.annotations);

		const codegen::id id = _codegen->define_texture(variable_location, texture_info);
		symbol = { symbol_type::variable, id, type };
	}
	// Samplers are actually combined image samplers
	else if (type.is_sampler())
	{
		assert(global);

		if (sampler_info.texture_name.empty())
		{
			error(variable_location, 3012, '\'' + name + "': missing 'Texture' property");
			return false;
		}
		if (type.texture_dimension() != static_cast<unsigned int>(texture_info.type))
		{
			error(variable_location, 3521, '\'' + name + "': type mismatch between texture and sampler type");
			return false;
		}

		if (texture_info.format != texture_format::unknown)
		{
			if (sampler_info.srgb && texture_info.format != texture_format::rgba8)
			{
				error(variable_location, 4582, '\'' + name + "': texture does not support sRGB sampling (only textures with RGBA8 format do)");
				return false;
			}

			bool matching_type = false;
			switch (texture_info.format)
			{
			case texture_format::r32i:
				matching_type = type.is_integral() && type.is_signed() && type.rows == 1 && type.cols == 1;
				break;
			case texture_format::r32u:
				matching_type = type.is_integral() && type.is_unsigned() && type.rows == 1 && type.cols == 1;
				break;
			case texture_format::rgba32i:
				matching_type = type.is_integral() && type.is_signed() && type.rows == 4 && type.cols == 1;
				break;
			case texture_format::rgba32u:
				matching_type = type.is_integral() && type.is_unsigned() && type.rows == 4 && type.cols == 1;
				break;
			default:
				matching_type = type.is_floating_point();
				break;
			}

			if (!matching_type)
			{
				error(variable_location, 4582, '\'' + name + "': type mismatch between texture format and sampler element type");
				return false;
			}
		}
		else if (type.is_integral())
		{
			// Set appropriate texture format so that code generation can choose correct texture type
			texture_info.format = type.is_signed() ?
				(type.rows > 1 ? texture_format::rgba32i : texture_format::r32i) :
				(type.rows > 1 ? texture_format::rgba32u : texture_format::r32u);
		}

		sampler_info.name = name;
		sampler_info.type = type;

		// Add namespace scope to avoid name clashes
		sampler_info.unique_name = 'V' + current_scope().name + name;
		std::replace(sampler_info.unique_name.begin(), sampler_info.unique_name.end(), ':', '_');

		const codegen::id id = _codegen->define_sampler(variable_location, texture_info, sampler_info);
		symbol = { symbol_type::variable, id, type };
	}
	else if (type.is_storage())
	{
		assert(global);

		if (storage_info.texture_name.empty())
		{
			error(variable_location, 3012, '\'' + name + "': missing 'Texture' property");
			return false;
		}
		if (type.texture_dimension() != static_cast<unsigned int>(texture_info.type))
		{
			error(variable_location, 3521, '\'' + name + "': type mismatch between texture and storage type");
			return false;
		}

		if (texture_info.format != texture_format::unknown)
		{
			bool matching_type = false;
			switch (texture_info.format)
			{
			case texture_format::r32i:
				matching_type = type.is_integral() && type.is_signed() && type.rows == 1 && type.cols == 1;
				break;
			case texture_format::r32u:
				matching_type = type.is_integral() && type.is_unsigned() && type.rows == 1 && type.cols == 1;
				break;
			case texture_format::rgba32i:
				matching_type = type.is_integral() && type.is_signed() && type.rows == 4 && type.cols == 1;
				break;
			case texture_format::rgba32u:
				matching_type = type.is_integral() && type.is_unsigned() && type.rows == 4 && type.cols == 1;
				break;
			default:
				matching_type = type.is_floating_point();
				break;
			}

			if (!matching_type)
			{
				error(variable_location, 4582, '\'' + name + "': type mismatch between texture format and storage element type");
				return false;
			}
		}
		else if (type.is_integral())
		{
			// Set appropriate texture format so that code generation can choose correct texture type
			texture_info.format = type.is_signed() ?
				(type.rows > 1 ? texture_format::rgba32i : texture_format::r32i) :
				(type.rows > 1 ? texture_format::rgba32u : texture_format::r32u);
		}

		storage_info.name = name;
		storage_info.type = type;

		// Add namespace scope to avoid name clashes
		storage_info.unique_name = 'V' + current_scope().name + name;
		std::replace(storage_info.unique_name.begin(), storage_info.unique_name.end(), ':', '_');

		if (storage_info.level > texture_info.levels - 1)
			storage_info.level = texture_info.levels - 1;

		const codegen::id id = _codegen->define_storage(variable_location, texture_info, storage_info);
		symbol = { symbol_type::variable, id, type };
	}
	// Uniform variables are put into a global uniform buffer structure
	else if (type.has(type::q_uniform))
	{
		assert(global);

		uniform uniform_info;
		uniform_info.name = name;
		uniform_info.type = type;

		// Add namespace scope to avoid name clashes
		uniform_info.unique_name = 'V' + current_scope().name + name;
		std::replace(uniform_info.unique_name.begin(), uniform_info.unique_name.end(), ':', '_');

		uniform_info.annotations = std::move(sampler_info.annotations);

		uniform_info.initializer_value = std::move(initializer.constant);
		uniform_info.has_initializer_value = initializer.is_constant;

		const codegen::id id = _codegen->define_uniform(variable_location, uniform_info);
		symbol = { symbol_type::variable, id, type };
	}
	// All other variables are separate entities
	else
	{
		// Update global variable names to contain the namespace scope to avoid name clashes
		std::string unique_name = global ? 'V' + current_scope().name + name : name;
		std::replace(unique_name.begin(), unique_name.end(), ':', '_');

		symbol = { symbol_type::variable, 0, type };
		symbol.id = _codegen->define_variable(variable_location, type, std::move(unique_name), global,
			// Shared variables cannot have an initializer
			type.has(type::q_groupshared) ? 0 : _codegen->emit_load(initializer));
	}

	// Insert the symbol into the symbol table
	if (!insert_symbol(name, symbol, global))
	{
		error(variable_location, 3003, "redefinition of '" + name + '\'');
		return false;
	}

	return parse_success;
}

bool reshadefx::parser::parse_technique()
{
	if (!expect(tokenid::identifier))
		return false;

	technique info;
	info.name = std::move(_token.literal_as_string);

	bool parse_success = parse_annotations(info.annotations);

	if (!expect('{'))
		return false;

	while (!peek('}'))
	{
		pass pass;
		if (parse_technique_pass(pass))
		{
			info.passes.push_back(std::move(pass));
		}
		else
		{
			parse_success = false;
			if (!peek(tokenid::pass) && !peek('}')) // If there is another pass definition following, try to parse that despite the error
			{
				consume_until('}');
				return false;
			}
		}
	}

	_codegen->define_technique(std::move(info));

	return expect('}') && parse_success;
}
bool reshadefx::parser::parse_technique_pass(pass &info)
{
	if (!expect(tokenid::pass))
		return false;

	const location pass_location = std::move(_token.location);

	// Passes can have an optional name
	if (accept(tokenid::identifier))
		info.name = std::move(_token.literal_as_string);

	bool parse_success = true;
	bool targets_support_srgb = true;
	function vs_info = {}, ps_info = {}, cs_info = {};

	if (!expect('{'))
		return false;

	while (!peek('}'))
	{
		// Parse pass states
		if (!expect(tokenid::identifier))
		{
			consume_until('}');
			return false;
		}

		location state_location = std::move(_token.location);
		const std::string state_name = std::move(_token.literal_as_string);

		if (!expect('='))
		{
			consume_until('}');
			return false;
		}

		const bool is_shader_state = state_name.size() > 6 && state_name.compare(state_name.size() - 6, 6, "Shader") == 0; // VertexShader, PixelShader, ComputeShader, ...
		const bool is_texture_state = state_name.compare(0, 12, "RenderTarget") == 0 && (state_name.size() == 12 || (state_name[12] >= '0' && state_name[12] < '8'));

		// Shader and render target assignment looks up values in the symbol table, so handle those separately from the other states
		if (is_shader_state || is_texture_state)
		{
			std::string identifier;
			scoped_symbol symbol;
			if (!accept_symbol(identifier, symbol))
			{
				consume_until('}');
				return false;
			}

			state_location = std::move(_token.location);

			int num_threads[3] = { 0, 0, 0 };
			if (accept('<'))
			{
				expression x, y, z;
				if (!parse_expression_multary(x, 8) || !expect(',') || !parse_expression_multary(y, 8))
				{
					consume_until('}');
					return false;
				}

				// Parse optional third dimension (defaults to 1)
				z.reset_to_rvalue_constant({}, 1);
				if (accept(',') && !parse_expression_multary(z, 8))
				{
					consume_until('}');
					return false;
				}

				if (!x.is_constant)
				{
					error(x.location, 3011, "value must be a literal expression");
					consume_until('}');
					return false;
				}
				if (!y.is_constant)
				{
					error(y.location, 3011, "value must be a literal expression");
					consume_until('}');
					return false;
				}
				if (!z.is_constant)
				{
					error(z.location, 3011, "value must be a literal expression");
					consume_until('}');
					return false;
				}
				x.add_cast_operation({ type::t_int, 1, 1 });
				y.add_cast_operation({ type::t_int, 1, 1 });
				z.add_cast_operation({ type::t_int, 1, 1 });
				num_threads[0] = x.constant.as_int[0];
				num_threads[1] = y.constant.as_int[0];
				num_threads[2] = z.constant.as_int[0];

				if (!expect('>'))
				{
					consume_until('}');
					return false;
				}
			}

			// Ignore invalid symbols that were added during error recovery
			if (symbol.id != UINT32_MAX)
			{
				if (is_shader_state)
				{
					if (!symbol.id)
					{
						parse_success = false;
						error(state_location, 3501, "undeclared identifier '" + identifier + "', expected function name");
					}
					else if (!symbol.type.is_function())
					{
						parse_success = false;
						error(state_location, 3020, "type mismatch, expected function name");
					}
					else if (std::count_if(_codegen->_functions.begin(), _codegen->_functions.end(),
						[&unique_name = symbol.function->unique_name](const std::unique_ptr<function> &info) { return info->unique_name == unique_name; }) > 1)
					{
						parse_success = false;
						error(state_location, 3067, "ambiguous function '" + identifier + '\'');
					}
					else
					{
						// We potentially need to generate a special entry point function which translates between function parameters and input/output variables
						switch (state_name[0])
						{
						case 'V':
							vs_info = *symbol.function;
							if (vs_info.type != shader_type::unknown && vs_info.type != shader_type::vertex)
							{
								parse_success = false;
								error(state_location, 3020, "type mismatch, expected vertex shader function");
								break;
							}
							vs_info.type = shader_type::vertex;
							_codegen->define_entry_point(vs_info);
							info.vs_entry_point = vs_info.unique_name;
							break;
						case 'P':
							ps_info = *symbol.function;
							if (ps_info.type != shader_type::unknown && ps_info.type != shader_type::pixel)
							{
								parse_success = false;
								error(state_location, 3020, "type mismatch, expected pixel shader function");
								break;
							}
							ps_info.type = shader_type::pixel;
							_codegen->define_entry_point(ps_info);
							info.ps_entry_point = ps_info.unique_name;
							break;
						case 'C':
							cs_info = *symbol.function;
							if (cs_info.type != shader_type::unknown && cs_info.type != shader_type::compute)
							{
								parse_success = false;
								error(state_location, 3020, "type mismatch, expected compute shader function");
								break;
							}
							cs_info.type = shader_type::compute;
							// Only use number of threads from pass when specified, otherwise fall back to number specified on the function definition with an attribute
							if (num_threads[0] != 0)
							{
								cs_info.num_threads[0] = num_threads[0];
								cs_info.num_threads[1] = num_threads[1];
								cs_info.num_threads[2] = num_threads[2];
							}
							else
							{
								cs_info.num_threads[0] = std::max(cs_info.num_threads[0], 1);
								cs_info.num_threads[1] = std::max(cs_info.num_threads[1], 1);
								cs_info.num_threads[2] = std::max(cs_info.num_threads[2], 1);
							}
							_codegen->define_entry_point(cs_info);
							info.cs_entry_point = cs_info.unique_name;
							break;
						}
					}
				}
				else
				{
					assert(is_texture_state);

					if (!symbol.id)
					{
						parse_success = false;
						error(state_location, 3004, "undeclared identifier '" + identifier + "', expected texture name");
					}
					else if (!symbol.type.is_texture())
					{
						parse_success = false;
						error(state_location, 3020, "type mismatch, expected texture name");
					}
					else if (symbol.type.texture_dimension() != 2)
					{
						parse_success = false;
						error(state_location, 3020, "cannot use texture" + std::to_string(symbol.type.texture_dimension()) + "D as render target");
					}
					else
					{
						texture &target_info = _codegen->get_texture(symbol.id);

						if (target_info.semantic.empty())
						{
							// Texture is used as a render target
							target_info.render_target = true;

							// Verify that all render targets in this pass have the same dimensions
							if ((info.viewport_width != 0 && info.viewport_height != 0) &&
								(target_info.width != info.viewport_width || target_info.height != info.viewport_height))
							{
								parse_success = false;
								error(state_location, 4545, "cannot use multiple render targets with different texture dimensions (is " + std::to_string(target_info.width) + 'x' + std::to_string(target_info.height) + ", but expected " + std::to_string(info.viewport_width) + 'x' + std::to_string(info.viewport_height) + ')');
							}

							info.viewport_width = target_info.width;
							info.viewport_height = target_info.height;

							const int target_index = state_name.size() > 12 ? (state_name[12] - '0') : 0;
							info.render_target_names[target_index] = target_info.unique_name;

							// Only RGBA8 format supports sRGB writes across all APIs
							if (target_info.format != texture_format::rgba8)
								targets_support_srgb = false;
						}
						else
						{
							parse_success = false;
							error(state_location, 3020, "cannot use texture with semantic as render target");
						}
					}
				}
			}
			else
			{
				parse_success = false;
			}
		}
		else // Handle the rest of the pass states
		{
			backup();

			expression state_exp;

			if (accept(tokenid::identifier)) // Handle special enumeration names for pass states
			{
				// Transform identifier to uppercase to do case-insensitive comparison
				std::transform(_token.literal_as_string.begin(), _token.literal_as_string.end(), _token.literal_as_string.begin(),
					[](std::string::value_type c) {
						return static_cast<std::string::value_type>(std::toupper(c));
					});

				static const std::unordered_map<std::string_view, uint32_t> s_enum_values = {
					{ "NONE", 0 }, { "ZERO", 0 }, { "ONE", 1 },
					{ "ADD", uint32_t(blend_op::add) },
					{ "SUBTRACT", uint32_t(blend_op::subtract) },
					{ "REVSUBTRACT", uint32_t(blend_op::reverse_subtract) },
					{ "MIN", uint32_t(blend_op::min) },
					{ "MAX", uint32_t(blend_op::max) },
					{ "SRCCOLOR", uint32_t(blend_factor::source_color) },
					{ "INVSRCCOLOR", uint32_t(blend_factor::one_minus_source_color) },
					{ "DESTCOLOR", uint32_t(blend_factor::dest_color) },
					{ "INVDESTCOLOR", uint32_t(blend_factor::one_minus_dest_color) },
					{ "SRCALPHA", uint32_t(blend_factor::source_alpha) },
					{ "INVSRCALPHA", uint32_t(blend_factor::one_minus_source_alpha) },
					{ "DESTALPHA", uint32_t(blend_factor::dest_alpha) },
					{ "INVDESTALPHA", uint32_t(blend_factor::one_minus_dest_alpha) },
					{ "KEEP", uint32_t(stencil_op::keep) },
					{ "REPLACE", uint32_t(stencil_op::replace) },
					{ "INVERT", uint32_t(stencil_op::invert) },
					{ "INCR", uint32_t(stencil_op::increment) },
					{ "INCRSAT", uint32_t(stencil_op::increment_saturate) },
					{ "DECR", uint32_t(stencil_op::decrement) },
					{ "DECRSAT", uint32_t(stencil_op::decrement_saturate) },
					{ "NEVER", uint32_t(stencil_func::never) },
					{ "EQUAL", uint32_t(stencil_func::equal) },
					{ "NEQUAL", uint32_t(stencil_func::not_equal) }, { "NOTEQUAL", uint32_t(stencil_func::not_equal)  },
					{ "LESS", uint32_t(stencil_func::less) },
					{ "GREATER", uint32_t(stencil_func::greater) },
					{ "LEQUAL", uint32_t(stencil_func::less_equal) }, { "LESSEQUAL", uint32_t(stencil_func::less_equal) },
					{ "GEQUAL", uint32_t(stencil_func::greater_equal) }, { "GREATEREQUAL", uint32_t(stencil_func::greater_equal) },
					{ "ALWAYS", uint32_t(stencil_func::always) },
					{ "POINTS", uint32_t(primitive_topology::point_list) },
					{ "POINTLIST", uint32_t(primitive_topology::point_list) },
					{ "LINES", uint32_t(primitive_topology::line_list) },
					{ "LINELIST", uint32_t(primitive_topology::line_list) },
					{ "LINESTRIP", uint32_t(primitive_topology::line_strip) },
					{ "TRIANGLES", uint32_t(primitive_topology::triangle_list) },
					{ "TRIANGLELIST", uint32_t(primitive_topology::triangle_list) },
					{ "TRIANGLESTRIP", uint32_t(primitive_topology::triangle_strip) },
				};

				// Look up identifier in list of possible enumeration names
				if (const auto it = s_enum_values.find(_token.literal_as_string);
					it != s_enum_values.end())
					state_exp.reset_to_rvalue_constant(_token.location, it->second);
				else // No match found, so rewind to parser state before the identifier was consumed and try parsing it as a normal expression
					restore();
			}

			// Parse right hand side as normal expression if no special enumeration name was matched already
			if (!state_exp.is_constant && !parse_expression_multary(state_exp))
			{
				consume_until('}');
				return false;
			}

			if (!state_exp.is_constant || !state_exp.type.is_scalar())
			{
				parse_success = false;
				error(state_exp.location, 3011, "pass state value must be a literal scalar expression");
			}

			// All states below expect the value to be of an unsigned integer type
			state_exp.add_cast_operation({ type::t_uint, 1, 1 });
			const unsigned int value = state_exp.constant.as_uint[0];

#define IMPLEMENT_STATE_VALUE_INDEXED(name, info_name, value) \
	else if (constexpr size_t name##_len = sizeof(#name) - 1; state_name.compare(0, name##_len, #name) == 0 && \
		(state_name.size() == name##_len || (state_name[name##_len] >= '0' && state_name[name##_len] < ('0' + static_cast<char>(std::size(info.info_name)))))) \
	{ \
		if (state_name.size() != name##_len) \
			info.info_name[state_name[name##_len] - '0'] = (value); \
		else \
			for (int i = 0; i < static_cast<int>(std::size(info.info_name)); ++i) \
				info.info_name[i] = (value); \
	}

			if (state_name == "GenerateMipmaps" || state_name == "GenerateMipMaps")
				info.generate_mipmaps = (value != 0);
			else if (state_name == "ClearRenderTargets")
				info.clear_render_targets = (value != 0);
			IMPLEMENT_STATE_VALUE_INDEXED(BlendEnable, blend_enable, value != 0)
			IMPLEMENT_STATE_VALUE_INDEXED(SrcBlend, source_color_blend_factor, static_cast<blend_factor>(value))
			IMPLEMENT_STATE_VALUE_INDEXED(SrcBlendAlpha, source_alpha_blend_factor, static_cast<blend_factor>(value))
			IMPLEMENT_STATE_VALUE_INDEXED(BlendOp, color_blend_op, static_cast<blend_op>(value))
			IMPLEMENT_STATE_VALUE_INDEXED(DestBlend, dest_color_blend_factor, static_cast<blend_factor>(value))
			IMPLEMENT_STATE_VALUE_INDEXED(DestBlendAlpha, dest_alpha_blend_factor, static_cast<blend_factor>(value))
			IMPLEMENT_STATE_VALUE_INDEXED(BlendOpAlpha, alpha_blend_op, static_cast<blend_op>(value))
			else if (state_name == "SRGBWriteEnable")
				info.srgb_write_enable = (value != 0);
			IMPLEMENT_STATE_VALUE_INDEXED(ColorWriteMask, render_target_write_mask, value & 0xFF)
			IMPLEMENT_STATE_VALUE_INDEXED(RenderTargetWriteMask, render_target_write_mask, value & 0xFF)
			else if (state_name == "StencilEnable")
				info.stencil_enable = (value != 0);
			else if (state_name == "StencilReadMask" || state_name == "StencilMask")
				info.stencil_read_mask = value & 0xFF;
			else if (state_name == "StencilWriteMask")
				info.stencil_write_mask = value & 0xFF;
			else if (state_name == "StencilRef")
				info.stencil_reference_value = value & 0xFF;
			else if (state_name == "StencilFunc")
				info.stencil_comparison_func = static_cast<stencil_func>(value);
			else if (state_name == "StencilPass" || state_name == "StencilPassOp")
				info.stencil_pass_op = static_cast<stencil_op>(value);
			else if (state_name == "StencilFail" || state_name == "StencilFailOp")
				info.stencil_fail_op = static_cast<stencil_op>(value);
			else if (state_name == "StencilZFail" || state_name == "StencilDepthFail" || state_name == "StencilDepthFailOp")
				info.stencil_depth_fail_op = static_cast<stencil_op>(value);
			else if (state_name == "PrimitiveType" || state_name == "PrimitiveTopology")
				info.topology = static_cast<primitive_topology>(value);
			else if (state_name == "VertexCount")
				info.num_vertices = value;
			else if (state_name == "DispatchSizeX")
				info.viewport_width = value;
			else if (state_name == "DispatchSizeY")
				info.viewport_height = value;
			else if (state_name == "DispatchSizeZ")
				info.viewport_dispatch_z = value;
			else
				error(state_location, 3004, "unrecognized pass state '" + state_name + '\'');

#undef IMPLEMENT_STATE_VALUE_INDEXED
		}

		if (!expect(';'))
		{
			consume_until('}');
			return false;
		}
	}

	if (parse_success)
	{
		if (!info.cs_entry_point.empty())
		{
			if (info.viewport_width == 0 || info.viewport_height == 0)
			{
				parse_success = false;
				error(pass_location, 3012, "pass is missing 'DispatchSizeX' or 'DispatchSizeY' property");
			}

			if (!info.vs_entry_point.empty())
				warning(pass_location, 3089, "pass is specifying both 'VertexShader' and 'ComputeShader' which cannot be used together");
			if (!info.ps_entry_point.empty())
				warning(pass_location, 3089, "pass is specifying both 'PixelShader' and 'ComputeShader' which cannot be used together");
		}
		else
		{
			if (info.vs_entry_point.empty())
			{
				parse_success = false;
				error(pass_location, 3012, "pass is missing 'VertexShader' property");
			}

			// Verify that shader signatures between VS and PS match (both semantics and interpolation qualifiers)
			std::unordered_map<std::string_view, type> vs_semantic_mapping;
			if (vs_info.return_semantic.empty())
			{
				if (!vs_info.return_type.is_void() && !vs_info.return_type.is_struct())
				{
					parse_success = false;
					error(pass_location, 3503, '\'' + vs_info.name + "': function return value is missing semantics");
				}
			}
			else
			{
				vs_semantic_mapping[vs_info.return_semantic] = vs_info.return_type;
			}

			for (const member_type &param : vs_info.parameter_list)
			{
				if (param.semantic.empty())
				{
					if (!param.type.is_struct())
					{
						parse_success = false;
						if (param.type.has(type::q_in))
							error(pass_location, 3502, '\'' + vs_info.name + "': input parameter '" + param.name + "' is missing semantics");
						else
							error(pass_location, 3503, '\'' + vs_info.name + "': output parameter '" + param.name + "' is missing semantics");
					}
				}
				else if (param.type.has(type::q_out))
				{
					vs_semantic_mapping[param.semantic] = param.type;
				}
			}

			if (ps_info.return_semantic.empty())
			{
				if (!ps_info.return_type.is_void() && !ps_info.return_type.is_struct())
				{
					parse_success = false;
					error(pass_location, 3503, '\'' + ps_info.name + "': function return value is missing semantics");
				}
			}

			for (const member_type &param : ps_info.parameter_list)
			{
				if (param.semantic.empty())
				{
					if (!param.type.is_struct())
					{
						parse_success = false;
						if (param.type.has(type::q_in))
							error(pass_location, 3502, '\'' + ps_info.name + "': input parameter '" + param.name + "' is missing semantics");
						else
							error(pass_location, 3503, '\'' + ps_info.name + "': output parameter '" + param.name + "' is missing semantics");
					}
				}
				else if (param.type.has(type::q_in))
				{
					if (const auto it = vs_semantic_mapping.find(param.semantic);
						it == vs_semantic_mapping.end() || it->second != param.type)
					{
						warning(pass_location, 4576, '\'' + ps_info.name + "': input parameter '" + param.name + "' semantic does not match vertex shader one");
					}
					else if (((it->second.qualifiers ^ param.type.qualifiers) & (type::q_linear | type::q_noperspective | type::q_centroid | type::q_nointerpolation)) != 0)
					{
						parse_success = false;
						error(  pass_location, 4568, '\'' + ps_info.name + "': input parameter '" + param.name + "' interpolation qualifiers do not match vertex shader ones");
					}
				}
			}

			for (codegen::id id : vs_info.referenced_samplers)
			{
				const sampler &sampler = _codegen->get_sampler(id);
				if (std::find(std::begin(info.render_target_names), std::end(info.render_target_names), sampler.texture_name) != std::end(info.render_target_names))
					error(pass_location, 3020, '\'' + sampler.texture_name + "': cannot sample from texture that is also used as render target in the same pass");
			}
			for (codegen::id id : ps_info.referenced_samplers)
			{
				const sampler &sampler = _codegen->get_sampler(id);
				if (std::find(std::begin(info.render_target_names), std::end(info.render_target_names), sampler.texture_name) != std::end(info.render_target_names))
					error(pass_location, 3020, '\'' + sampler.texture_name + "': cannot sample from texture that is also used as render target in the same pass");
			}

			if (!vs_info.referenced_storages.empty() || !ps_info.referenced_storages.empty())
			{
				parse_success = false;
				error(pass_location, 3667, "storage writes are only valid in compute shaders");
			}

			// Verify render target format supports sRGB writes if enabled
			if (info.srgb_write_enable && !targets_support_srgb)
			{
				parse_success = false;
				error(pass_location, 4582, "one or more render target(s) do not support sRGB writes (only textures with RGBA8 format do)");
			}
		}
	}

	return expect('}') && parse_success;
}

void reshadefx::codegen::optimize_bindings()
{
	struct sampler_group
	{
		std::vector<function *> vs_entry_points;
		std::vector<function *> ps_entry_points;
		std::vector<id> vs_referenced_samplers;
	};

	std::vector<sampler_group> sampler_groups;

	// Build a list of samplers referenced by all vertex and pixel shader combinations
	for (const technique &tech : _module.techniques)
	{
		for (const pass &pass : tech.passes)
		{
			if (!pass.cs_entry_point.empty())
				continue;

			function *const vs = find_function(pass.vs_entry_point);
			function *const ps = !pass.ps_entry_point.empty() ? find_function(pass.ps_entry_point) : nullptr;

			bool has_vs_entry_point = false;
			bool has_ps_entry_point = false;
			auto group_it = std::find_if(sampler_groups.begin(), sampler_groups.end(),
				[vs, ps, &has_vs_entry_point, &has_ps_entry_point](const sampler_group &group) {
					has_vs_entry_point = std::find(group.vs_entry_points.begin(), group.vs_entry_points.end(), vs) != group.vs_entry_points.end();
					has_ps_entry_point = std::find(group.ps_entry_points.begin(), group.ps_entry_points.end(), ps) != group.ps_entry_points.end();
					return has_vs_entry_point || has_ps_entry_point;
				});
			if (sampler_groups.end() == group_it)
				group_it = sampler_groups.insert(sampler_groups.end(), sampler_group {});

			if (!has_vs_entry_point)
				group_it->vs_entry_points.push_back(vs);
			if (!pass.ps_entry_point.empty() && !has_ps_entry_point)
				group_it->ps_entry_points.push_back(ps);

			std::vector<codegen::id> vs_referenced_samplers;
			std::set_union(group_it->vs_referenced_samplers.begin(), group_it->vs_referenced_samplers.end(), vs->referenced_samplers.begin(), vs->referenced_samplers.end(), std::back_inserter(vs_referenced_samplers));
			group_it->vs_referenced_samplers = std::move(vs_referenced_samplers);
		}
	}

	for (const sampler_group &group : sampler_groups)
	{
		for (function *const vs_entry_point : group.vs_entry_points)
		{
			for (size_t binding = 0; binding < std::min(vs_entry_point->referenced_samplers.size(), group.vs_referenced_samplers.size()); ++binding)
			{
				if (vs_entry_point->referenced_samplers[binding] != group.vs_referenced_samplers[binding])
					vs_entry_point->referenced_samplers.insert(vs_entry_point->referenced_samplers.begin() + binding, 0);
			}
		}

		for (function *const ps_entry_point : group.ps_entry_points)
		{
			// Add samplers referenced in vertex shader to all pixel shaders that are used with it, while keeping the vertex shader ones at the front to ensure binding compatibility
			for (size_t binding = 0; binding < std::min(ps_entry_point->referenced_samplers.size(), group.vs_referenced_samplers.size()); ++binding)
			{
				if (ps_entry_point->referenced_samplers[binding] != group.vs_referenced_samplers[binding])
				{
					const auto it = std::find(ps_entry_point->referenced_samplers.begin(), ps_entry_point->referenced_samplers.end(), group.vs_referenced_samplers[binding]);
					if (it != ps_entry_point->referenced_samplers.end())
						std::swap(ps_entry_point->referenced_samplers[binding], *it);
					else
						ps_entry_point->referenced_samplers.insert(ps_entry_point->referenced_samplers.begin() + binding, 0);
				}
			}
		}
	}

	// Finally apply the generated bindings to all passes
	for (technique &tech : _module.techniques)
	{
		for (pass &pass : tech.passes)
		{
			std::vector<id> referenced_samplers;
			std::vector<id> referenced_storages;

			if (!pass.cs_entry_point.empty())
			{
				const function *const cs = find_function(pass.cs_entry_point);

				referenced_samplers = cs->referenced_samplers;
				referenced_storages = cs->referenced_storages;
			}
			else
			{
				const function *const vs = find_function(pass.vs_entry_point);

				referenced_samplers = vs->referenced_samplers;

				if (!pass.ps_entry_point.empty())
				{
					const function *const ps = find_function(pass.ps_entry_point);

					referenced_samplers.resize(std::max(referenced_samplers.size(), ps->referenced_samplers.size()));

					for (uint32_t binding = 0; binding < ps->referenced_samplers.size(); ++binding)
						if (ps->referenced_samplers[binding] != 0)
							referenced_samplers[binding] = ps->referenced_samplers[binding];
				}
			}

			for (uint32_t binding = 0; binding < referenced_samplers.size(); ++binding)
			{
				if (referenced_samplers[binding] == 0)
					continue;

				const sampler &sampler = get_sampler(referenced_samplers[binding]);

				sampler_binding s;
				s.index = &sampler - _module.samplers.data();
				s.entry_point_binding = binding;
				pass.sampler_bindings.push_back(s);

				texture_binding t;
				t.index = s.index;
				t.entry_point_binding = s.entry_point_binding;
				t.srgb = sampler.srgb;
				pass.texture_bindings.push_back(t);
			}

			for (uint32_t binding = 0; binding < referenced_storages.size(); ++binding)
			{
				if (referenced_storages[binding] == 0)
					continue;

				const storage &storage = get_storage(referenced_storages[binding]);

				storage_binding u;
				u.index = &storage - _module.storages.data();
				u.entry_point_binding = binding;
				pass.storage_bindings.push_back(u);
			}
		}
	}
}

// sopt: parses text as top-level declarations (exp == nullptr) or as one expression, with its own lexer
// starting at loc, so errors point at the construct the text was made from.
bool reshadefx::parser::sopt_parse_text(const std::string &text, const location &loc, expression *exp)
{
	lexer *const outer = _lexer;
	const token saved_token = _token, saved_next = _token_next, saved_backup = _token_backup;
	_lexer = new lexer(text, true, true, true, false, false, true, loc);
	consume();
	bool success = true;
	if (exp != nullptr)
	{
		success = parse_expression_assignment(*exp) && peek(tokenid::end_of_file);
	}
	else
	{
		while (success && !peek(tokenid::end_of_file))
		{
			bool current_success = true;
			success = parse_top(current_success) && current_success;
		}
	}
	delete _lexer;
	_lexer = outer;
	_token = saved_token;
	_token_next = saved_next;
	_token_backup = saved_backup;
	return success;
}

// sopt: consumes a parenthesized token sequence, e.g. register(t0) or packoffset(c1.y).
bool reshadefx::parser::sopt_skip_parens()
{
	if (!expect('('))
		return false;
	for (int depth = 1; depth > 0;)
	{
		if (peek(tokenid::end_of_file))
			return expect(')');
		if (peek('('))
			++depth;
		if (peek(')'))
			--depth;
		consume();
	}
	return true;
}

// sopt: HLSL resource declarations (see sopt_hlsl). Returns false on a fatal error; 'handled' says whether
// the next tokens were such a declaration.
bool reshadefx::parser::sopt_hlsl_declaration(bool &handled, bool &parse_success)
{
	handled = false;
	const std::string word = _token_next.literal_as_string;
	if (!peek(tokenid::identifier) && !peek(tokenid::reserved))
		return true;

	// cbuffer / tbuffer Name [: register(bN)] { members } [;]: the members are uniforms
	if (peek(tokenid::identifier) && (word == "cbuffer" || word == "tbuffer"))
	{
		handled = true;
		consume();
		if (!expect(tokenid::identifier))
			return parse_success = false, true;
		if (accept(':') && !((accept(tokenid::identifier) || expect(tokenid::reserved)) && sopt_skip_parens()))
			return parse_success = false, true;
		if (!expect('{'))
			return parse_success = false, true;
		bool members_success = true;
		while (!peek('}') && !peek(tokenid::end_of_file))
		{
			bool current_success = true;
			if (!parse_top(current_success))
				return false;
			members_success = members_success && current_success;
		}
		parse_success = expect('}') && members_success;
		accept(';');
		return true;
	}

	// SamplerState / SamplerComparisonState Name [: register(sN)] [{ ... }];: not needed (the implicit
	// sampler of each texture is used), skipped
	if (peek(tokenid::reserved) && (word == "SamplerState" || word == "SamplerComparisonState"))
	{
		handled = true;
		consume();
		if (!expect(tokenid::identifier))
			return parse_success = false, true;
		if (accept(':') && !((accept(tokenid::identifier) || expect(tokenid::reserved)) && sopt_skip_parens()))
			return parse_success = false, true;
		if (accept('{'))
			for (int depth = 1; depth > 0 && !peek(tokenid::end_of_file); consume())
				depth += peek('{') ? 1 : peek('}') ? -1 : 0;
		parse_success = expect(';');
		return true;
	}

	// Resource objects [: register(xN)];:
	//   Texture1D/2D/3D/Cube/2DArray[<T>], Buffer<T>, StructuredBuffer<T> of a scalar or vector T: a texture
	//     with an unknown format (float formats) and an implicit sampler __sopt_smp_Name (cube and array textures
	//     as 2D: their fetches are only region inputs);
	//   RWTexture1D/2D/3D/2DArray<T>, RWBuffer<T>, RWStructuredBuffer<T> of a scalar or vector T: a storage object
	//     Name on a texture __sopt_rwtex<rows>_Name (element float / float4 as ReShade FX storages have them);
	//   structured buffers of a struct, Append / Consume buffers: a static global __sopt_buf_Name of the element
	//     type; ByteAddressBuffer / RWByteAddressBuffer: a static uint4 __sopt_buf_Name (enough to parse; their
	//     reads and writes end regions).
	static const char *const resource_words[] = {
		"Texture1D", "Texture2D", "Texture3D", "TextureCube", "Texture2DArray", "Buffer", "StructuredBuffer",
		"RWTexture1D", "RWTexture2D", "RWTexture3D", "RWTexture2DArray", "RWBuffer", "RWStructuredBuffer",
		"AppendStructuredBuffer", "ConsumeStructuredBuffer", "ByteAddressBuffer", "RWByteAddressBuffer" };
	bool resource_word = false;
	for (const char *const w : resource_words)
		resource_word = resource_word || word == w;
	if ((peek(tokenid::reserved) || peek(tokenid::identifier)) && resource_word)
	{
		handled = true;
		consume();
		const bool rw = word.rfind("RW", 0) == 0;
		const bool byte_address = word.find("ByteAddress") != std::string::npos;
		const bool buffer = word.find("Buffer") != std::string::npos;
		const unsigned int dimension = (word.find("1D") != std::string::npos || buffer) ? 1 :
			(word.find("3D") != std::string::npos || word == "RWTexture2DArray") ? 3 : 2;
		std::string element = byte_address ? "uint4" : "float4";
		if (accept('<'))
		{
			const size_t begin = _token_next.offset;
			size_t end = begin;
			for (int depth = 1; !peek(tokenid::end_of_file);)
			{
				if (peek('<'))
					++depth;
				if (peek('>') && --depth == 0)
					break;
				consume();
				end = _token.offset + _token.length;
			}
			element = _lexer->input_string().substr(begin, end - begin);
			if (!expect('>'))
				return parse_success = false, true;
		}
		if (!expect(tokenid::identifier))
			return parse_success = false, true;
		const std::string name = _token.literal_as_string;
		const location name_location = _token.location;
		if (peek('['))
		{
			error(_token_next.location, 3000, "sopt: arrays of resources are not supported");
			return parse_success = false, true;
		}
		if (accept(':') && !((accept(tokenid::identifier) || expect(tokenid::reserved)) && sopt_skip_parens()))
			return parse_success = false, true;
		if (!expect(';'))
			return parse_success = false, true;

		// The element type: base 'f' / 'i' / 'u' and rows, or a struct (base 0).
		std::string e = element;
		for (const char *const q : { "unorm ", "snorm " })
			if (e.rfind(q, 0) == 0)
				e.erase(0, std::strlen(q));
		while (!e.empty() && e.back() == ' ')
			e.pop_back();
		unsigned int rows = 1;
		if (!e.empty() && e.back() >= '1' && e.back() <= '4')
			rows = e.back() - '0', e.pop_back();
		char base = 0;
		if (e == "float" || e == "half" || e == "double" || e == "min16float" || e == "min10float")
			base = 'f';
		else if (e == "int" || e == "min16int" || e == "min12int")
			base = 'i';
		else if (e == "uint" || e == "min16uint" || e == "dword" || e == "bool")
			base = 'u';

		sopt_resource res;
		res.name = name;
		res.dimension = dimension;
		res.rows = rows;
		res.buffer = buffer;
		res.base = base;
		const std::string d = std::to_string(dimension);
		std::string text;
		if (byte_address || base == 0 || word == "AppendStructuredBuffer" || word == "ConsumeStructuredBuffer")
		{
			res.kind = byte_address ? 'b' : 'g';
			text = "static " + element + " __sopt_buf_" + name + "; static uint __sopt_cnt_" + name + ";";
		}
		else
		{
			const std::string scalar = base == 'f' ? "float" : base == 'i' ? "int" : "uint";
			const std::string type = rows == 1 ? scalar : scalar + '4';
			const std::string format = base == 'f' ? (rows == 1 ? "R32F" : "RGBA32F") :
				base == 'i' ? (rows == 1 ? "R32I" : "RGBA32I") : (rows == 1 ? "R32U" : "RGBA32U");
			if (rw)
			{
				res.kind = 'u';
				text = "texture" + d + "D __sopt_rwtex" + std::to_string(rows) + '_' + name + " { Format = " + format +
					"; }; storage" + d + "D<" + type + "> " + name + " { Texture = __sopt_rwtex" + std::to_string(rows) + '_' +
					name + "; };";
			}
			else
			{
				res.kind = 't';
				text = "texture" + d + "D " + name + " { Format = " + format + "; }; sampler" + d + "D<" + type +
					"> __sopt_smp_" + name + " { Texture = " + name + "; };";
			}
		}
		_sopt_resources.push_back(res);
		if (res.kind == 't' || res.kind == 'u')
			sopt_hlsl_fetch_names.push_back(name);
		if (buffer && (res.kind == 't' || res.kind == 'u'))
			sopt_hlsl_buffer_names.push_back(name);
		parse_success = sopt_parse_text(text, name_location, nullptr);
		return true;
	}
	return true;
}

// sopt: the text of a parenthesized / bracketed argument list (the opening token is next), split at depth-0
// commas.
bool reshadefx::parser::sopt_argument_texts(char close, std::vector<std::string> &args)
{
	consume(); // '(' or '['
	while (!peek(close) && !peek(tokenid::end_of_file))
	{
		const size_t begin = _token_next.offset;
		size_t end = begin;
		for (int depth = 0; !peek(tokenid::end_of_file) && !(depth == 0 && (peek(',') || peek(close)));)
		{
			if (peek('(') || peek('[') || peek('{'))
				++depth;
			if (peek(')') || peek(']') || peek('}'))
				--depth;
			consume();
			end = _token.offset + _token.length;
		}
		args.push_back(_lexer->input_string().substr(begin, end - begin));
		if (!accept(','))
			break;
	}
	return expect(close);
}

// sopt: the source text of an expression up to the next depth-0 ';', ',', ')', ']' or '}' (the right side of an
// assignment).
std::string reshadefx::parser::sopt_expression_text()
{
	const size_t begin = _token_next.offset;
	size_t end = begin;
	for (int depth = 0; !peek(tokenid::end_of_file);)
	{
		if (depth == 0 && (peek(';') || peek(',') || peek(')') || peek(']') || peek('}')))
			break;
		if (peek('(') || peek('[') || peek('{'))
			++depth;
		if (peek(')') || peek(']') || peek('}'))
			--depth;
		consume();
		end = _token.offset + _token.length;
	}
	return _lexer->input_string().substr(begin, end - begin);
}

// sopt: GetDimensions(out a, out b, ...) as assignments of 1 to the arguments that are names (enough to parse).
static std::string sopt_dimensions_text(const std::vector<std::string> &args)
{
	std::string text;
	for (const std::string &a : args)
	{
		size_t i = a.find_first_not_of(" \t\r\n");
		const size_t j = a.find_last_not_of(" \t\r\n");
		if (i == std::string::npos || !(std::isalpha(static_cast<unsigned char>(a[i])) || a[i] == '_'))
			continue;
		bool name = true;
		for (; i <= j; ++i)
			name = name && (std::isalnum(static_cast<unsigned char>(a[i])) || a[i] == '_' || a[i] == '.');
		if (name)
			text += '(' + a + ") = ";
	}
	return text.empty() ? std::string("0") : text + '1';
}

// sopt: an HLSL resource access (see sopt_hlsl and sopt_hlsl_declaration): Name.Method(args) or Name[index]
// (also as the target of an assignment), the '.' or '[' being the next token.
bool reshadefx::parser::sopt_hlsl_resource_access(const sopt_resource &res, const location &loc, expression &exp)
{
	const std::string d = std::to_string(res.dimension);
	const std::string coord = res.dimension == 1 ? ".x" : res.dimension == 2 ? ".xy" : ".xyz";
	const std::string icoord = res.dimension == 1 ? "int" : "int" + d;
	const std::string widened = res.rows == 2 ? ".xy" : res.rows == 3 ? ".xyz" : "";
	const std::string sampler = "__sopt_smp_" + res.name;
	const std::string global = "__sopt_buf_" + res.name;
	const auto arg = [](const std::vector<std::string> &args, size_t i) {
		return i < args.size() ? '(' + args[i] + ')' : std::string("0");
	};
	// A read of element `index` of a texture, buffer or storage.
	const auto read = [&](const std::string &index) {
		return "tex" + d + "Dfetch(" + (res.kind == 't' ? sampler : res.name) + ", " + icoord + "((" + index + ')' +
			coord + "))" + widened;
	};

	std::string text;
	if (peek('['))
	{
		std::vector<std::string> index;
		if (!sopt_argument_texts(']', index) || index.size() != 1)
			return false;
		if (res.kind == 'g' || res.kind == 'b')
		{
			text = global;
		}
		else if (res.kind == 'u' && (peek('=') || peek(tokenid::plus_equal) || peek(tokenid::minus_equal) ||
			peek(tokenid::star_equal) || peek(tokenid::slash_equal) || peek(tokenid::percent_equal) ||
			peek(tokenid::ampersand_equal) || peek(tokenid::pipe_equal) || peek(tokenid::caret_equal) ||
			peek(tokenid::less_less_equal) || peek(tokenid::greater_greater_equal)))
		{
			// Name[index] = value (or op=): texNDstore(Name, index, value), the value widened to the storage's float4
			consume();
			std::string op = _token.id == static_cast<tokenid>('=') ? std::string() : token::id_to_name(_token.id);
			if (!op.empty())
				op.pop_back(); // "+=" -> "+"
			std::string value = '(' + sopt_expression_text() + ')';
			if (!op.empty())
				value = '(' + read(index[0]) + ") " + op + ' ' + value;
			if (res.rows == 2 || res.rows == 3)
				// float2 / float3 elements: the storage is 4-wide, the value widened by a swizzle (sopt's codegen
				// records the value without it as the stored one)
				value = '(' + value + (res.rows == 2 ? ").xyyy" : ").xyzz");
			text = "tex" + d + "Dstore(" + res.name + ", " + icoord + "((" + index[0] + ')' + coord + "), " + value + ')';
		}
		else
		{
			text = read(index[0]);
		}
		return sopt_parse_text(text, loc, &exp);
	}

	consume(); // '.'
	if (!expect(tokenid::identifier))
		return false;
	const std::string method = _token.literal_as_string;
	if (!peek('('))
	{
		error(_token_next.location, 3000, "sopt: expected '(' after resource method '" + method + '\'');
		return false;
	}
	std::vector<std::string> args;
	if (!sopt_argument_texts(')', args))
		return false;

	if (method == "GetDimensions")
		text = sopt_dimensions_text(args);
	else if (res.kind == 'g')
	{
		if (method == "Append")
			text = global + " = " + arg(args, 0);
		else if (method == "Consume")
			text = global;
		else if (method == "IncrementCounter" || method == "DecrementCounter")
			text = "__sopt_cnt_" + res.name;
	}
	else if (res.kind == 'b')
	{
		static const char *const parts[] = { ".x", ".xy", ".xyz", "" };
		const size_t n = method.size() > 4 && std::isdigit(static_cast<unsigned char>(method.back())) ? method.back() - '1' : 0;
		if (method.rfind("Load", 0) == 0 && n < 4)
			text = global + parts[n];
		else if (method.rfind("Store", 0) == 0 && n < 4)
			text = global + parts[n] + " = " + arg(args, 1);
		else if (method.rfind("Interlocked", 0) == 0)
		{
			std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
			rest.insert(rest.begin(), global + ".x");
			return sopt_hlsl_interlocked(method, rest, loc, exp);
		}
	}
	else if (res.kind == 'u')
	{
		if (method == "Load")
			text = read(args.empty() ? "0" : args[0]);
	}
	else if (method == "Sample" || method == "SampleLevel" || method == "SampleBias" || method == "SampleGrad")
		text = "tex" + d + "D(" + sampler + ", " + arg(args, 1) + coord + ')' + widened;
	else if (method == "SampleCmp" || method == "SampleCmpLevelZero")
		text = "tex" + d + "D(" + sampler + ", " + arg(args, 1) + coord + ").x";
	else if (method == "Load")
		text = read(args.empty() ? "0" : args[0]);
	else if (res.dimension == 2 && (method == "Gather" || method == "GatherRed" || method == "GatherGreen" ||
		method == "GatherBlue" || method == "GatherAlpha"))
		text = std::string("tex2Dgather") + (method == "Gather" ? 'R' : method[6]) + '(' + sampler + ", " + arg(args, 1) + ".xy)";

	if (text.empty())
	{
		error(loc, 3000, "sopt: resource method '" + method + "' is not supported");
		return false;
	}
	return sopt_parse_text(text, loc, &exp);
}

// sopt: InterlockedOp(dest, value[, original]) and InterlockedCompareExchange(dest, compare, value, original) /
// InterlockedCompareStore(dest, compare, value) as ReShade FX atomics (on a storage element when dest is one).
bool reshadefx::parser::sopt_hlsl_interlocked(const std::string &name, const std::vector<std::string> &args, const location &loc, expression &exp)
{
	const bool compare = name == "InterlockedCompareExchange" || name == "InterlockedCompareStore";
	const std::string fx = compare ? "atomicCompareExchange" : "atomic" + name.substr(11);
	const size_t inputs = compare ? 3 : 2;
	if (args.size() < inputs)
	{
		error(loc, 3000, "sopt: too few arguments to '" + name + '\'');
		return false;
	}
	// dest = Name[index] of a storage: the storage overload atomicOp(storage, coord, ...)
	std::string dest = '(' + args[0] + ')';
	const std::string &a0 = args[0];
	const size_t open = a0.find('['), first = a0.find_first_not_of(" \t\r\n");
	if (open != std::string::npos && first != std::string::npos && a0.find_last_not_of(" \t\r\n") == a0.rfind(']'))
	{
		std::string base = a0.substr(first, open - first);
		while (!base.empty() && (base.back() == ' ' || base.back() == '\t'))
			base.pop_back();
		for (const auto &r : _sopt_resources)
			if (r.kind == 'u' && r.name == base)
			{
				const std::string coord = r.dimension == 1 ? ".x" : r.dimension == 2 ? ".xy" : ".xyz";
				const std::string icoord = r.dimension == 1 ? "int" : "int" + std::to_string(r.dimension);
				dest = r.name + ", " + icoord + "((" + a0.substr(open + 1, a0.rfind(']') - open - 1) + ')' + coord + ')';
			}
	}
	std::string text = fx + '(' + dest;
	for (size_t i = 1; i < inputs; ++i)
		text += ", (" + args[i] + ')';
	text += ')';
	if (args.size() > inputs && name != "InterlockedCompareStore")
		text = '(' + args[inputs] + ") = " + text;
	return sopt_parse_text(text, loc, &exp);
}

// sopt: HLSL intrinsics with another name in ReShade FX (barriers, Interlocked*).
static const std::pair<const char *, const char *> sopt_barriers[] = {
	{ "GroupMemoryBarrierWithGroupSync", "barrier()" }, { "DeviceMemoryBarrierWithGroupSync", "barrier()" },
	{ "AllMemoryBarrierWithGroupSync", "barrier()" }, { "GroupMemoryBarrier", "groupMemoryBarrier()" },
	{ "DeviceMemoryBarrier", "memoryBarrier()" }, { "AllMemoryBarrier", "memoryBarrier()" } };

bool reshadefx::parser::sopt_hlsl_intrinsic_name(const std::string &name)
{
	for (const auto &b : sopt_barriers)
		if (name == b.first)
			return true;
	return name.rfind("Interlocked", 0) == 0;
}

// The '(' is next.
bool reshadefx::parser::sopt_hlsl_intrinsic(const std::string &name, const location &loc, expression &exp)
{
	std::vector<std::string> args;
	if (!sopt_argument_texts(')', args))
		return false;
	for (const auto &b : sopt_barriers)
		if (name == b.first)
			return sopt_parse_text(b.second, loc, &exp);
	return sopt_hlsl_interlocked(name, args, loc, exp);
}

// sopt: GLSL declarations (see sopt_glsl). Returns false on a fatal error; 'handled' says whether the next tokens
// were such a declaration:
//   precision lowp|mediump|highp T;                      skipped
//   [layout(...)] [flat|smooth|noperspective|centroid] in|out T name[, name];
//                                                         recorded as the entry point's parameters (and declared as
//                                                         static globals, so other functions still parse)
//   uniform samplerND / isamplerND / usamplerND name;    a texture __sopt_tex_name plus a sampler name
//   uniform Block { members } [;]                         the members as uniforms
//   other declarations with GLSL-only qualifiers          parsed without them
bool reshadefx::parser::sopt_glsl_declaration(bool &handled, bool &parse_success)
{
	handled = false;
	const auto word = [this]() { return _lexer->input_string().substr(_token_next.offset, _token_next.length); };
	if (peek(tokenid::identifier) && word() == "precision")
	{
		handled = true;
		consume_until(';');
		return true;
	}

	const location start = _token_next.location;
	backup();
	unsigned int interpolation = 0, layout_location = UINT32_MAX;
	bool in = false, out = false, uniform = false, glsl_only = false;
	for (;;)
	{
		const std::string w = word();
		if (peek(tokenid::identifier) && w == "layout")
		{
			consume();
			std::vector<std::string> args;
			if (!peek('(') || !sopt_argument_texts(')', args))
				return parse_success = false, handled = true, true;
			for (const std::string &a : args)
				if (const size_t eq = a.find('='); eq != std::string::npos && a.find("location") != std::string::npos)
					layout_location = static_cast<unsigned int>(std::strtoul(a.c_str() + eq + 1, nullptr, 0));
			glsl_only = true;
		}
		else if (peek(tokenid::identifier) && w == "flat")
			consume(), interpolation |= type::q_nointerpolation, glsl_only = true;
		else if (peek(tokenid::identifier) && (w == "smooth" || w == "invariant" || w == "highp" || w == "mediump" || w == "lowp"))
			consume(), glsl_only = true;
		else if (accept(tokenid::noperspective))
			interpolation |= type::q_noperspective;
		else if (accept(tokenid::centroid))
			interpolation |= type::q_centroid;
		else if (accept(tokenid::in))
			in = true;
		else if (accept(tokenid::out))
			out = true;
		else if (accept(tokenid::uniform_))
			uniform = true;
		else
			break;
	}
	if (!in && !out && !uniform && !glsl_only && interpolation == 0)
		return true;
	// A plain uniform of a ReShade FX type (not a sampler or a block) parses as it is.
	if (uniform && !in && !out && !glsl_only && interpolation == 0)
	{
		const std::string t = word();
		reshadefx::type sampler_type = {};
		const bool sampler = t == "sampler1D" || t == "sampler2D" || t == "sampler3D" ||
			(sopt_glsl_type(t, sampler_type) && sampler_type.is_sampler());
		bool block = false;
		if (!sampler && peek(tokenid::identifier))
		{
			consume();
			block = peek('{');
		}
		if (!sampler && !block)
		{
			restore();
			return true;
		}
		restore();
		consume(); // 'uniform' again
	}
	handled = true;

	// The rest of the declaration up to its ';' (braces included).
	const size_t begin = _token_next.offset;
	size_t end = begin;
	for (int depth = 0; !peek(tokenid::end_of_file) && !(depth == 0 && peek(';'));)
	{
		if (peek('{'))
			++depth;
		if (peek('}') && --depth == 0)
		{
			consume();
			end = _token.offset + _token.length;
			if (!peek(';') && !peek(tokenid::identifier))
				break; // a uniform block without a trailing ';'
			continue;
		}
		consume();
		end = _token.offset + _token.length;
	}
	accept(';');
	const std::string rest = _lexer->input_string().substr(begin, end - begin);
	if (rest.empty()) // e.g. layout(local_size_x = 8) in;
		return true;

	// Split "T name, name" into the type word and the names (no arrays).
	const auto words = [](const std::string &text) {
		std::vector<std::string> list;
		std::string current;
		for (char c : text)
		{
			if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
				current += c;
			else
			{
				if (!current.empty())
					list.push_back(current), current.clear();
				if (c == '[' || c == '=' || c == '{')
					list.push_back(std::string(1, c));
			}
		}
		if (!current.empty())
			list.push_back(current);
		return list;
	};

	std::string text;
	const std::vector<std::string> w = words(rest);
	if (uniform && !w.empty())
	{
		const std::string &t = w[0];
		const bool keyword_sampler = t == "sampler1D" || t == "sampler2D" || t == "sampler3D";
		reshadefx::type sampler_type = {};
		if (keyword_sampler || (sopt_glsl_type(t, sampler_type) && sampler_type.is_sampler()))
		{
			const char base = t[0] == 'i' ? 'i' : t[0] == 'u' ? 'u' : 'f';
			const unsigned int dim = (t.find("1D") != std::string::npos) ? 1 :
				(t.find("3D") != std::string::npos || t == "samplerCube" || t == "sampler2DArray") ? 3 : 2;
			const std::string d = std::to_string(dim);
			const std::string format = base == 'i' ? "RGBA32I" : base == 'u' ? "RGBA32U" : "RGBA32F";
			const std::string element = base == 'i' ? "int4" : base == 'u' ? "uint4" : "float4";
			for (size_t i = 1; i < w.size(); ++i)
			{
				if (w[i] == "[" || w[i] == "=")
				{
					error(start, 3000, "sopt: arrays of GLSL samplers are not supported");
					return parse_success = false, true;
				}
				text += "texture" + d + "D __sopt_tex_" + w[i] + " { Format = " + format + "; }; sampler" + d + "D<" + element +
					"> " + w[i] + " { Texture = __sopt_tex_" + w[i] + "; };";
				sopt_glsl_samplers.push_back(w[i]);
				_sopt_glsl_sampler_dims.push_back({ w[i], dim });
			}
		}
		else if (w.size() >= 2 && w[1] == "{")
		{
			// uniform Block { T a; T b; } [instance];: the members as uniforms (an instance name is not supported)
			const size_t open = rest.find('{'), close = rest.rfind('}');
			if (close == std::string::npos || words(rest.substr(close + 1)).size() > 0)
			{
				error(start, 3000, "sopt: GLSL uniform blocks with an instance name are not supported");
				return parse_success = false, true;
			}
			const std::string members = rest.substr(open + 1, close - open - 1);
			size_t a = 0;
			for (size_t b; (b = members.find(';', a)) != std::string::npos; a = b + 1)
				if (members.find_first_not_of(" \t\r\n", a) < b)
					text += "uniform " + members.substr(a, b - a) + ";";
		}
		else
			text = "uniform " + rest + ";";
	}
	else if ((in || out) && w.size() >= 2)
	{
		// The type is the first word (GLSL types are one word), then the names.
		for (size_t i = 1; i < w.size(); ++i)
		{
			if (w[i] == "[" || w[i] == "=")
			{
				error(start, 3000, "sopt: arrays of GLSL inputs and outputs are not supported");
				return parse_success = false, true;
			}
			sopt_glsl_io io;
			io.name = w[i];
			io.type_text = w[0];
			io.loc = start;
			io.out = out;
			io.interpolation = interpolation;
			size_t index = 0;
			for (const sopt_glsl_io &other : _sopt_glsl_io)
				index += other.out == out;
			if (layout_location != UINT32_MAX)
				index = layout_location;
			if (out)
				io.semantic = index == 0 ? "SV_TARGET" : "SV_TARGET" + std::to_string(index);
			else
			{
				// A 2-component input named like a texture coordinate gets a TEXCOORD semantic (the [0, 1] convention),
				// any other one a semantic without a convention.
				std::string lower = io.name;
				for (char &c : lower)
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				const bool texcoord = w[0] == "vec2" && (lower.find("uv") != std::string::npos ||
					lower.find("coord") != std::string::npos || lower.find("tex") != std::string::npos);
				io.semantic = (texcoord ? "TEXCOORD" : "GLSLIN") + std::to_string(index);
			}
			_sopt_glsl_io.push_back(io);
			text += "static " + w[0] + ' ' + w[i] + ';';
		}
	}
	else
	{
		text = rest + ";";
	}
	if (!text.empty())
		parse_success = sopt_parse_text(text, start, nullptr);
	return true;
}

// sopt: GLSL built-in functions with another name or form in ReShade FX.
static const char *const sopt_glsl_renames[][2] = {
	{ "fract", "frac" }, { "inversesqrt", "rsqrt" }, { "dFdx", "ddx" }, { "dFdy", "ddy" }, { "dFdxFine", "ddx_fine" },
	{ "dFdyFine", "ddy_fine" }, { "dFdxCoarse", "ddx_coarse" }, { "dFdyCoarse", "ddy_coarse" }, { "fwidthFine", "fwidth" },
	{ "fwidthCoarse", "fwidth" }, { "roundEven", "round" }, { "floatBitsToUint", "asuint" }, { "floatBitsToInt", "asint" },
	{ "uintBitsToFloat", "asfloat" }, { "intBitsToFloat", "asfloat" }, { "fma", "mad" }, { "mix", "lerp" } };
static const char *const sopt_glsl_specials[] = {
	"atan", "mod", "not", "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual", "equal", "notEqual",
	"texture", "texture2D", "texture3D", "textureLod", "textureGrad", "texelFetch", "textureSize", "textureOffset",
	"texelFetchOffset", "textureLodOffset", "textureGradOffset", "textureGather", "textureGatherOffset" };

bool reshadefx::parser::sopt_glsl_intrinsic_name(const std::string &name)
{
	for (const auto &r : sopt_glsl_renames)
		if (name == r[0])
			return true;
	for (const char *const s : sopt_glsl_specials)
		if (name == s)
			return true;
	return false;
}

// The '(' is next.
bool reshadefx::parser::sopt_glsl_intrinsic(const std::string &name, const location &loc, expression &exp)
{
	std::vector<std::string> args;
	if (!sopt_argument_texts(')', args))
		return false;
	const auto arg = [&](size_t i) { return i < args.size() ? '(' + args[i] + ')' : std::string("(0)"); };
	const auto list = [&](size_t from) {
		std::string s;
		for (size_t i = from; i < args.size(); ++i)
			s += (i > from ? ", " : "") + args[i];
		return s;
	};
	std::string text;
	for (const auto &r : sopt_glsl_renames)
		if (name == r[0])
			text = std::string(r[1]) + '(' + list(0) + ')';
	if (text.empty())
	{
		// Texture functions: the dimension of the sampler (a global sampler's, else 2D).
		std::string sampler = args.empty() ? std::string() : args[0];
		sampler.erase(0, sampler.find_first_not_of(" \t\r\n"));
		sampler.erase(sampler.find_last_not_of(" \t\r\n") + 1);
		unsigned int dim = 2;
		for (const auto &s : _sopt_glsl_sampler_dims)
			if (s.first == sampler)
				dim = s.second;
		const std::string tex = "tex" + std::to_string(dim) + "D";
		const std::string lod_coord = dim == 3 ? "float4(" + arg(1) + ", " + arg(2) + ")" :
			dim == 1 ? "float4(" + arg(1) + ", 0.0, 0.0, " + arg(2) + ")" : "float4(" + arg(1) + ", 0.0, " + arg(2) + ")";
		if (name == "atan")
			text = args.size() == 2 ? "atan2(" + list(0) + ')' : "atan(" + list(0) + ')';
		else if (name == "mod")
			text = '(' + arg(0) + " - " + arg(1) + " * floor(" + arg(0) + " / " + arg(1) + "))";
		else if (name == "not")
			text = "(!" + arg(0) + ')';
		else if (name == "lessThan" || name == "lessThanEqual" || name == "greaterThan" || name == "greaterThanEqual" ||
			name == "equal" || name == "notEqual")
		{
			const char *op = name == "lessThan" ? " < " : name == "lessThanEqual" ? " <= " : name == "greaterThan" ? " > " :
				name == "greaterThanEqual" ? " >= " : name == "equal" ? " == " : " != ";
			text = '(' + arg(0) + op + arg(1) + ')';
		}
		else if (name == "texture" || name == "texture2D" || name == "texture3D")
			text = tex + '(' + args[0] + ", " + arg(1) + ')'; // a bias argument is dropped (fetches are region inputs)
		else if (name == "textureLod")
			text = tex + "lod(" + args[0] + ", " + lod_coord + ')';
		else if (name == "textureGrad" || name == "textureGradOffset")
			text = tex + "grad(" + args[0] + ", " + arg(1) + ", " + arg(2) + ", " + arg(3) + ')';
		else if (name == "texelFetch")
			text = tex + "fetch(" + args[0] + ", " + arg(1) + ", " + arg(2) + ')';
		else if (name == "texelFetchOffset")
			text = tex + "fetch(" + args[0] + ", " + arg(1) + " + " + arg(3) + ", " + arg(2) + ')';
		else if (name == "textureSize")
			text = tex + "size(" + args[0] + (args.size() > 1 ? ", " + arg(1) : std::string()) + ')';
		else if (name == "textureOffset")
			text = tex + '(' + args[0] + ", " + arg(1) + ", " + arg(2) + ')';
		else if (name == "textureLodOffset")
			text = tex + "lod(" + args[0] + ", " + lod_coord + ", " + arg(3) + ')';
		else if (name == "textureGather" || name == "textureGatherOffset")
		{
			const bool offset = name == "textureGatherOffset";
			std::string comp = args.size() > (offset ? 3u : 2u) ? args[offset ? 3 : 2] : std::string("0");
			comp.erase(0, comp.find_first_not_of(" \t\r\n"));
			const char *channel = comp.rfind('1', 0) == 0 ? "G" : comp.rfind('2', 0) == 0 ? "B" : comp.rfind('3', 0) == 0 ? "A" : "R";
			text = "tex2Dgather" + std::string(channel) + '(' + args[0] + ", " + arg(1) + (offset ? ", " + arg(2) : std::string()) + ')';
		}
	}
	if (text.empty())
	{
		error(loc, 3004, "sopt: unsupported GLSL built-in '" + name + '\'');
		return false;
	}
	return sopt_parse_text(text, loc, &exp);
}
