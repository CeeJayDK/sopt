/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "effect_symbol_table.hpp"

namespace reshadefx
{
	/// <summary>
	/// A parser for the ReShade FX shader language.
	/// </summary>
	class parser : symbol_table
	{
	public:
		// Define constructor explicitly because lexer class is not included here
		parser();
		~parser();

		/// <summary>
		/// Parses the provided source code and generate code for it.
		/// </summary>
		/// <param name="source">Source code string to parse.</param>
		/// <param name="backend">Code generation implementation to use.</param>
		/// <returns><see langword="true"/> if parsing was successfull, <see langword="false"/> otherwise.</returns>
		bool parse(std::string source, class codegen *backend);

		/// <summary>
		/// sopt addition: global 'static const' variables whose initial value is not a literal expression
		/// (only possible with symbolic macros) become named expressions: each use parses the initializer
		/// again, so the value stays an expression of the symbolic macros.
		/// </summary>
		bool sopt_named_expressions = false;

		/// <summary>
		/// sopt addition: plain HLSL (SM5 pixel shaders) instead of ReShade FX. cbuffer / tbuffer blocks declare
		/// their members as uniforms, Texture1D/2D/3D/Cube/2DArray objects become a texture plus an implicit
		/// sampler, SamplerState declarations are skipped, texture method calls (Sample, SampleLevel, Load,
		/// Gather, ...) become texture fetch intrinsics, register / packoffset annotations are skipped, and the
		/// function named sopt_hlsl_entry is the pixel shader entry point.
		/// </summary>
		bool sopt_hlsl = false;
		std::string sopt_hlsl_entry = "main";
		/// sopt (HLSL mode): the entry point is a compute shader when it has a [numthreads] attribute. Resources whose
		/// elements are read as Name[index] (textures, buffers, RW textures and typed RW buffers), and of those the
		/// buffers (filled while parsing).
		std::vector<std::string> sopt_hlsl_fetch_names;
		std::vector<std::string> sopt_hlsl_buffer_names;

		/// <summary>
		/// Gets the list of error messages.
		/// </summary>
		const std::string &errors() const { return _errors; }

	private:
		void error(const location &location, unsigned int code, const std::string &message);
		void warning(const location &location, unsigned int code, const std::string &message);

		void backup();
		void restore();

		bool peek(char tok) const { return _token_next.id == static_cast<tokenid>(tok); }
		bool peek(tokenid tokid) const { return _token_next.id == tokid; }
		void consume();
		void consume_until(char tok) { return consume_until(static_cast<tokenid>(tok)); }
		void consume_until(tokenid tokid);
		bool accept(char tok) { return accept(static_cast<tokenid>(tok)); }
		bool accept(tokenid tokid);
		bool expect(char tok) { return expect(static_cast<tokenid>(tok)); }
		bool expect(tokenid tokid);

		bool accept_symbol(std::string &identifier, scoped_symbol &symbol);
		bool accept_type_class(type &type);
		bool accept_type_qualifiers(type &type);
		bool accept_unary_op();
		bool accept_postfix_op();
		bool peek_multary_op(unsigned int &precedence) const;
		bool accept_assignment_op();

		bool parse_top(bool &parse_success);
		bool parse_struct();
		bool parse_function(type type, std::string name, shader_type stype, int num_threads[3]);
		bool parse_variable(type type, std::string name, bool global = false);
		bool parse_technique();
		bool parse_technique_pass(pass &info);
		bool parse_type(type &type);
		bool parse_array_length(type &type);
		bool parse_expression(expression &expression);
		bool parse_expression_unary(expression &expression);
		bool parse_expression_multary(expression &expression, unsigned int precedence = 0);
		bool parse_expression_assignment(expression &expression);
		bool parse_annotations(std::vector<annotation> &annotations);
		bool parse_statement(bool scoped);
		bool parse_statement_block(bool scoped);

		std::string _errors;

		class lexer *_lexer = nullptr;
		class codegen *_codegen = nullptr;

		token _token;
		token _token_next;
		token _token_backup;

		std::vector<std::pair<std::string, location>> _sopt_named; // sopt: initializer text and location

		// sopt: HLSL mode helpers (see sopt_hlsl)
		bool sopt_parse_text(const std::string &text, const location &loc, expression *exp);
		bool sopt_skip_parens();
		bool sopt_hlsl_declaration(bool &handled, bool &parse_success);
		struct sopt_resource
		{
			std::string name;
			unsigned int dimension = 2;
			unsigned int rows = 4;   // element components
			char kind = 't';         // 't' texture / buffer, 'u' RW texture / buffer, 'g' struct buffer, 'b' byte address buffer
			char base = 'f';         // element base type: 'f', 'i', 'u' (0 for a struct)
			bool buffer = false;
		};
		bool sopt_argument_texts(char close, std::vector<std::string> &args);
		std::string sopt_expression_text();
		bool sopt_hlsl_resource_access(const sopt_resource &res, const location &loc, expression &exp);
		bool sopt_hlsl_interlocked(const std::string &name, const std::vector<std::string> &args, const location &loc, expression &exp);
		static bool sopt_hlsl_intrinsic_name(const std::string &name);
		bool sopt_hlsl_intrinsic(const std::string &name, const location &loc, expression &exp);
		std::vector<sopt_resource> _sopt_resources; // HLSL resource objects

		std::vector<uint32_t> _loop_break_target_stack;
		std::vector<uint32_t> _loop_continue_target_stack;
	};
}
